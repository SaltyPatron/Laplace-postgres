/* 4D GiST on PostGIS geometry POINT ZM: laplace_point4d_ops (ordered by <~>, Euclidean) and laplace_angular4d_ops
 * (ordered by <=>, the angle between directions on S^3; below).
 *
 * The keys are double-precision 4D boxes (laplace_box4d, 64 bytes) built from the geometry's own coordinates, not
 * PostGIS's float32 boxes rounded outward. The ordering operator <~> is the exact 4D Euclidean distance
 * (lp_distance4). A leaf key is its point (min == max), and its distance is computed exactly as lp_distance4 computes
 * it: the same differences, squared and summed in the same order, ((dx^2 + dy^2) + dz^2) + dm^2, then a correctly
 * rounded sqrt. Leaf recheck also applies the query operator to the heap value. An inner key's distance takes, on each axis,
 * the gap from the query to the box (0 inside it), which is no larger than the difference to any point in the box;
 * rounding is monotone, so the bound is never above the exact distance of anything under it.
 *
 * Built sorted (sortsupport): keys in 4D Hilbert order of their centres, so CREATE INDEX writes pages in one pass
 * instead of inserting row by row. */
#include "postgres.h"
#include "pg_core.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/gist.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "utils/sortsupport.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "partitioning/partdesc.h"
#include "partitioning/partbounds.h"
#include "storage/bufmgr.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "geo.h"
#include "laplace/laplace.h"
#include <math.h>

typedef struct { float lo[4], hi[4]; } Box4;

static inline float f32_lo(double v){
    if (isnan(v)) return NAN;
    float f = (float)v;
    return (double)f > v ? nextafterf(f, -INFINITY) : f;
}
static inline float f32_hi(double v){
    if (isnan(v)) return NAN;
    float f = (float)v;
    return (double)f < v ? nextafterf(f, INFINITY) : f;
}

#define LAPLACE_KNN 15             /* ORDER BY geometry <~>, <=> or <%> geometry: each class's own distance */
#define LAPLACE_KNN_HAUSDORFF 16   /* ORDER BY geometry <%%> geometry (laplace_path4d_ops) */

/* ---------------------------------------------------------------- the key type */
static const double *point_of(Datum d){
    Geo g = geo_of(d);
    if (g.type != 1 || g.p.n != 1) ereport(ERROR, (errmsg("laplace_point4d_ops: expected a POINT ZM, not an empty point or a LINESTRING")));
    return xyzm(&g);
}

PG_FUNCTION_INFO_V1(laplace_box4d_in);
Datum laplace_box4d_in(PG_FUNCTION_ARGS){
    const char *s = PG_GETARG_CSTRING(0); Box4 *b = palloc(sizeof *b); double lo[4], hi[4]; int used = 0;
    if (sscanf(s, " BOX4D ( %lf %lf %lf %lf , %lf %lf %lf %lf ) %n", &lo[0], &lo[1], &lo[2], &lo[3],
               &hi[0], &hi[1], &hi[2], &hi[3], &used) != 8 || s[used] != '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION), errmsg("laplace_box4d: expected BOX4D(x y z m, x y z m), got \"%s\"", s)));
    for (int d = 0; d < 4; d++) { b->lo[d] = f32_lo(lo[d]); b->hi[d] = f32_hi(hi[d]); }
    PG_RETURN_POINTER(b);
}

PG_FUNCTION_INFO_V1(laplace_box4d_out);
Datum laplace_box4d_out(PG_FUNCTION_ARGS){
    const Box4 *b = (const Box4 *)PG_GETARG_POINTER(0);
    PG_RETURN_CSTRING(psprintf("BOX4D(%.9g %.9g %.9g %.9g,%.9g %.9g %.9g %.9g)",
                               (double)b->lo[0], (double)b->lo[1], (double)b->lo[2], (double)b->lo[3],
                               (double)b->hi[0], (double)b->hi[1], (double)b->hi[2], (double)b->hi[3]));
}

/* ---------------------------------------------------------------- the operator */
PG_FUNCTION_INFO_V1(laplace_point4d_distance);
Datum laplace_point4d_distance(PG_FUNCTION_ARGS){
    PG_RETURN_FLOAT8(lp_distance4(point_of(PG_GETARG_DATUM(0)), point_of(PG_GETARG_DATUM(1))));
}

/* Direction on S^3: the point divided by its length (the length summed in lp_distance4's order). The origin has none:
 * NaN, which orders after every angle. */
static inline void direction_of(const double p[4], double u[4]){
    double n = sqrt(((p[0] * p[0] + p[1] * p[1]) + p[2] * p[2]) + p[3] * p[3]);
    for (int d = 0; d < 4; d++) u[d] = p[d] / n;
}
/* The angle from a chord between unit vectors: 2 asin(c/2), with c/2 held to 1 (unit vectors are unit within
 * rounding, so a chord between opposite ones can exceed 2 by an ulp). Monotone in c. */
static inline double angle_of_chord(double c){ double h = c / 2.0; return 2.0 * asin(h > 1.0 ? 1.0 : h); }   /* NaN (the origin) passes through */

/* <=>: the angle between two points' directions, in radians. */
PG_FUNCTION_INFO_V1(laplace_angular4d);
Datum laplace_angular4d(PG_FUNCTION_ARGS){
    double u[4], v[4];
    direction_of(point_of(PG_GETARG_DATUM(0)), u); direction_of(point_of(PG_GETARG_DATUM(1)), v);
    PG_RETURN_FLOAT8(angle_of_chord(lp_distance4(u, v)));
}

PG_FUNCTION_INFO_V1(laplace_direction4d_ewkb);
Datum laplace_direction4d_ewkb(PG_FUNCTION_ARGS){
    double u[4]; direction_of(point_of(PG_GETARG_DATUM(0)), u);
    bytea *b = palloc(VARHDRSZ + 37); SET_VARSIZE(b, VARHDRSZ + 37); lp_ewkb_point4(u, (uint8 *)VARDATA(b), 37);
    PG_RETURN_BYTEA_P(b);
}

/* ---------------------------------------------------------------- box arithmetic */
static inline bool box_is_nan(const Box4 *b){ return isnan(b->lo[0]) || isnan(b->hi[0]); }
static inline void box_of_point(Box4 *b, const double p[4]){ for (int d = 0; d < 4; d++) { b->lo[d] = f32_lo(p[d]); b->hi[d] = f32_hi(p[d]); } }
static inline void box_join(Box4 *a, const Box4 *b){
    if (box_is_nan(b)) return;
    if (box_is_nan(a)) { *a = *b; return; }
    for (int d = 0; d < 4; d++) { if (b->lo[d] < a->lo[d]) a->lo[d] = b->lo[d]; if (b->hi[d] > a->hi[d]) a->hi[d] = b->hi[d]; }
}
static inline double box_edges(const Box4 *b){
    if (box_is_nan(b)) return 0.0;
    return (((double)b->hi[0] - (double)b->lo[0]) + ((double)b->hi[1] - (double)b->lo[1]))
         + ((double)b->hi[2] - (double)b->lo[2]) + ((double)b->hi[3] - (double)b->lo[3]);
}
/* The query's distance to the outward-rounded box: guaranteed lower bound on exact lp_distance4. */
static inline double box4_distance(const Box4 *b, const double q[4]){
    if (box_is_nan(b)) return NAN;
    double g[4];
    for (int d = 0; d < 4; d++) {
        double lo = (double)b->lo[d], hi = (double)b->hi[d];
        g[d] = q[d] < lo ? lo - q[d] : q[d] > hi ? q[d] - hi : 0.0;
    }
    return sqrt(((g[0] * g[0] + g[1] * g[1]) + g[2] * g[2]) + g[3] * g[3]);
}
static inline uint64 box_hilbert(const Box4 *b){
    uint32_t g[4];
    for (int d = 0; d < 4; d++) {
        double c = ((double)b->lo[d] + (double)b->hi[d]) / 2.0, v = (c + 1.0) / 2.0 * 65536.0;
        g[d] = !(v >= 0) ? 0 : v > 65535.0 ? 65535u : (uint32_t)v;   /* NaN and below the ball: 0 */
    }
    return lp_hilbert4_grid(g);
}

/* ---------------------------------------------------------------- GiST support */
PG_FUNCTION_INFO_V1(laplace_point4d_gist_compress);
Datum laplace_point4d_gist_compress(PG_FUNCTION_ARGS){
    GISTENTRY *e = (GISTENTRY *)PG_GETARG_POINTER(0);
    if (!e->leafkey) PG_RETURN_POINTER(e);
    GISTENTRY *r = palloc(sizeof *r); Box4 *b = palloc(sizeof *b);
    box_of_point(b, point_of(e->key));
    gistentryinit(*r, PointerGetDatum(b), e->rel, e->page, e->offset, false);
    PG_RETURN_POINTER(r);
}

/* laplace_angular4d_ops: the same keys over each point's direction, ordered by <=>. A leaf's angle is <=>'s, bit for
 * bit: the same direction, chord and arcsine. An inner box gives a chord no longer than to any direction in it, and
 * the angle is monotone in the chord. The executor rechecks leaf values. */
PG_FUNCTION_INFO_V1(laplace_angular4d_gist_compress);
Datum laplace_angular4d_gist_compress(PG_FUNCTION_ARGS){
    GISTENTRY *e = (GISTENTRY *)PG_GETARG_POINTER(0);
    if (!e->leafkey) PG_RETURN_POINTER(e);
    GISTENTRY *r = palloc(sizeof *r); Box4 *b = palloc(sizeof *b); double u[4];
    direction_of(point_of(e->key), u); box_of_point(b, u);
    gistentryinit(*r, PointerGetDatum(b), e->rel, e->page, e->offset, false);
    PG_RETURN_POINTER(r);
}

PG_FUNCTION_INFO_V1(laplace_angular4d_gist_distance);
Datum laplace_angular4d_gist_distance(PG_FUNCTION_ARGS){
    GISTENTRY *e = (GISTENTRY *)PG_GETARG_POINTER(0);
    StrategyNumber strategy = (StrategyNumber)PG_GETARG_UINT16(2); bool *recheck = (bool *)PG_GETARG_POINTER(4);
    if (strategy != LAPLACE_KNN) ereport(ERROR, (errmsg("laplace_angular4d_ops: unknown strategy %u", strategy)));
    double u[4]; direction_of(point_of(PG_GETARG_DATUM(1)), u);
    *recheck = true;
    PG_RETURN_FLOAT8(angle_of_chord(box4_distance((const Box4 *)DatumGetPointer(e->key), u)));
}

PG_FUNCTION_INFO_V1(laplace_point4d_gist_consistent);
Datum laplace_point4d_gist_consistent(PG_FUNCTION_ARGS){
    /* The class has an ordering operator only; GiST calls this for search operators, of which there are none. */
    bool *recheck = (bool *)PG_GETARG_POINTER(4);
    *recheck = true;
    PG_RETURN_BOOL(true);
}

/* An inner key: the box, and for a path key (below) its ends marked absent. Every key starts with its Box4, so the
 * box functions serve both key sizes. */
static void *inner_key(const Box4 *b, size_t size){
    float *k = palloc(size); memcpy(k, b, sizeof *b);
    for (size_t i = sizeof *b / sizeof(float); i < size / sizeof(float); i++) k[i] = NAN;
    return k;
}
static Datum keys_union(GistEntryVector *v, int *size, size_t keysize){
    Box4 u = *(const Box4 *)DatumGetPointer(v->vector[0].key);
    for (int i = 1; i < v->n; i++) box_join(&u, (const Box4 *)DatumGetPointer(v->vector[i].key));
    *size = (int)keysize;
    return PointerGetDatum(inner_key(&u, keysize));
}
PG_FUNCTION_INFO_V1(laplace_point4d_gist_union);
Datum laplace_point4d_gist_union(PG_FUNCTION_ARGS){
    return keys_union((GistEntryVector *)PG_GETARG_POINTER(0), (int *)PG_GETARG_POINTER(1), sizeof(Box4));
}

/* Growth of the edge sum, so a point (all edges 0) still costs what it stretches. */
PG_FUNCTION_INFO_V1(laplace_point4d_gist_penalty);
Datum laplace_point4d_gist_penalty(PG_FUNCTION_ARGS){
    const Box4 *o = (const Box4 *)DatumGetPointer(((GISTENTRY *)PG_GETARG_POINTER(0))->key);
    const Box4 *n = (const Box4 *)DatumGetPointer(((GISTENTRY *)PG_GETARG_POINTER(1))->key);
    float *penalty = (float *)PG_GETARG_POINTER(2);
    Box4 j = *o; box_join(&j, n);
    *penalty = (float)(box_edges(&j) - box_edges(o));
    PG_RETURN_POINTER(penalty);
}

/* Split on the axis where the entries' centres spread most, at the median: two halves of equal size, ordered by
 * centre then by position, so the split is the same on every machine. */
typedef struct { double c; OffsetNumber i; } Centre;
static int centre_cmp(const void *a, const void *b){
    const Centre *x = a, *y = b;
    bool xn = isnan(x->c), yn = isnan(y->c);
    if (xn != yn) return xn ? 1 : -1;
    if (!xn) {
        if (x->c < y->c) return -1;
        if (x->c > y->c) return 1;
    }
    return (int)x->i - (int)y->i;
}
static Datum keys_split(GistEntryVector *v, GIST_SPLITVEC *s, size_t keysize){
    OffsetNumber last = (OffsetNumber)(v->n - 1);
    int n = last - FirstOffsetNumber + 1, axis = 0; double best = -1;
    for (int d = 0; d < 4; d++) {
        double lo = INFINITY, hi = -INFINITY;
        for (OffsetNumber i = FirstOffsetNumber; i <= last; i++) {
            const Box4 *b = (const Box4 *)DatumGetPointer(v->vector[i].key); double c = (b->lo[d] + b->hi[d]) / 2.0;
            lo = c < lo ? c : lo; hi = c > hi ? c : hi;
        }
        if (hi - lo > best) { best = hi - lo; axis = d; }
    }
    Centre *c = palloc(sizeof *c * (size_t)n);
    for (OffsetNumber i = FirstOffsetNumber; i <= last; i++) {
        const Box4 *b = (const Box4 *)DatumGetPointer(v->vector[i].key);
        c[i - FirstOffsetNumber] = (Centre){ (b->lo[axis] + b->hi[axis]) / 2.0, i };
    }
    qsort(c, (size_t)n, sizeof *c, centre_cmp);
    s->spl_left = palloc(sizeof(OffsetNumber) * (size_t)n); s->spl_right = palloc(sizeof(OffsetNumber) * (size_t)n);
    s->spl_nleft = s->spl_nright = 0;
    Box4 lb, rb, *l = &lb, *r = &rb;
    for (int k = 0; k < n; k++) {
        const Box4 *b = (const Box4 *)DatumGetPointer(v->vector[c[k].i].key);
        if (k < n / 2) { if (s->spl_nleft++ == 0) *l = *b; else box_join(l, b); s->spl_left[s->spl_nleft - 1] = c[k].i; }
        else { if (s->spl_nright++ == 0) *r = *b; else box_join(r, b); s->spl_right[s->spl_nright - 1] = c[k].i; }
    }
    s->spl_ldatum = PointerGetDatum(inner_key(l, keysize)); s->spl_rdatum = PointerGetDatum(inner_key(r, keysize));
    return PointerGetDatum(s);
}
PG_FUNCTION_INFO_V1(laplace_point4d_gist_picksplit);
Datum laplace_point4d_gist_picksplit(PG_FUNCTION_ARGS){
    return keys_split((GistEntryVector *)PG_GETARG_POINTER(0), (GIST_SPLITVEC *)PG_GETARG_POINTER(1), sizeof(Box4));
}


PG_FUNCTION_INFO_V1(laplace_point4d_gist_same);
Datum laplace_point4d_gist_same(PG_FUNCTION_ARGS){
    const Box4 *a = (const Box4 *)PG_GETARG_POINTER(0), *b = (const Box4 *)PG_GETARG_POINTER(1); bool *r = (bool *)PG_GETARG_POINTER(2);
    *r = memcmp(a, b, sizeof *a) == 0;
    PG_RETURN_POINTER(r);
}

/* The query is read in place each call: a POINT ZM is stored inline, so this is a header check, not a copy. */
PG_FUNCTION_INFO_V1(laplace_point4d_gist_distance);
Datum laplace_point4d_gist_distance(PG_FUNCTION_ARGS){
    GISTENTRY *e = (GISTENTRY *)PG_GETARG_POINTER(0);
    StrategyNumber strategy = (StrategyNumber)PG_GETARG_UINT16(2); bool *recheck = (bool *)PG_GETARG_POINTER(4);
    if (strategy != LAPLACE_KNN) ereport(ERROR, (errmsg("laplace_point4d_ops: unknown strategy %u", strategy)));
    *recheck = true;    /* always evaluate the exact ordering operator on heap leaves */
    PG_RETURN_FLOAT8(box4_distance((const Box4 *)DatumGetPointer(e->key), point_of(PG_GETARG_DATUM(1))));
}

/* ---------------------------------------------------------------- laplace_path4d_ops: Fréchet KNN
 * Keys over LINESTRING ZM (a POINT ZM is a path of one vertex): the vertices' box, and at a leaf the first and last
 * vertex (inner keys mark them absent), 128 bytes. Ordered by <%>, laplace_frechet4d, the discrete Fréchet distance
 * (lp_frechet4). Every coupling pairs the first vertices, pairs the last ones, and pairs each query vertex with some
 * vertex of the path, which lies in the box. So the Fréchet distance is at least the largest of: the query vertices'
 * distances to the box, and at a leaf the first-to-first and last-to-last distances. Each is summed in lp_frechet4's
 * order, and the gap to a box is no larger than the difference to any point in it, so the bound never exceeds the
 * exact distance; the leaf is rechecked, which puts the rows in exact Fréchet order. The class also orders by <%%>,
 * the discrete Hausdorff distance (lp_hausdorff4): it too pairs each query vertex with some vertex of the path, so the
 * box term bounds it (the ends do not pair up under it). */
typedef struct { Box4 box; float first[4], last[4]; } PathKey;

static inline double d2_of_f32(const double *q, const float *p){
    double g[4];
    for (int d = 0; d < 4; d++) {
        double d0 = fabs(q[d] - (double)p[d]);
        float up = nextafterf(p[d], INFINITY);
        double eps = fabs((double)up - (double)p[d]);
        g[d] = d0 > eps ? d0 - eps : 0.0;
    }
    return ((g[0] * g[0] + g[1] * g[1]) + g[2] * g[2]) + g[3] * g[3];
}
static inline double box_gap2(const Box4 *b, const double q[4]){
    double g[4];
    for (int d = 0; d < 4; d++) {
        double lo = (double)b->lo[d], hi = (double)b->hi[d];
        g[d] = q[d] < lo ? lo - q[d] : q[d] > hi ? q[d] - hi : 0.0;
    }
    return ((g[0] * g[0] + g[1] * g[1]) + g[2] * g[2]) + g[3] * g[3];
}
static Geo path_of(Datum d){
    Geo g = geo_of(d);
    if (g.p.n == 0) ereport(ERROR, (errmsg("laplace_path4d_ops: expected a LINESTRING ZM or POINT ZM with a vertex")));
    return g;
}

/* The key type exists for the index's storage only: shown as its box, never typed in. */
PG_FUNCTION_INFO_V1(laplace_path4d_key_in);
Datum laplace_path4d_key_in(PG_FUNCTION_ARGS){
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("laplace_path4d_key is an index key, not an input type")));
    PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(laplace_path4d_key_out);
Datum laplace_path4d_key_out(PG_FUNCTION_ARGS){ return DirectFunctionCall1(laplace_box4d_out, PG_GETARG_DATUM(0)); }

PG_FUNCTION_INFO_V1(laplace_path4d_gist_compress);
Datum laplace_path4d_gist_compress(PG_FUNCTION_ARGS){
    GISTENTRY *e = (GISTENTRY *)PG_GETARG_POINTER(0);
    if (!e->leafkey) PG_RETURN_POINTER(e);
    Geo g = path_of(e->key); const double *x = xyzm(&g); size_t n = g.p.n;
    GISTENTRY *r = palloc(sizeof *r); PathKey *k = palloc(sizeof *k);
    box_of_point(&k->box, x);
    for (size_t i = 1; i < n; i++) { Box4 v; box_of_point(&v, x + 4 * i); box_join(&k->box, &v); }
    for (int d = 0; d < 4; d++) { k->first[d] = (float)x[d]; k->last[d] = (float)x[4 * (n - 1) + d]; }
    gistentryinit(*r, PointerGetDatum(k), e->rel, e->page, e->offset, false);
    PG_RETURN_POINTER(r);
}

PG_FUNCTION_INFO_V1(laplace_path4d_gist_union);
Datum laplace_path4d_gist_union(PG_FUNCTION_ARGS){
    return keys_union((GistEntryVector *)PG_GETARG_POINTER(0), (int *)PG_GETARG_POINTER(1), sizeof(PathKey));
}
PG_FUNCTION_INFO_V1(laplace_path4d_gist_picksplit);
Datum laplace_path4d_gist_picksplit(PG_FUNCTION_ARGS){
    return keys_split((GistEntryVector *)PG_GETARG_POINTER(0), (GIST_SPLITVEC *)PG_GETARG_POINTER(1), sizeof(PathKey));
}
PG_FUNCTION_INFO_V1(laplace_path4d_gist_same);
Datum laplace_path4d_gist_same(PG_FUNCTION_ARGS){
    bool *r = (bool *)PG_GETARG_POINTER(2);
    *r = memcmp(PG_GETARG_POINTER(0), PG_GETARG_POINTER(1), sizeof(PathKey)) == 0;
    PG_RETURN_POINTER(r);
}

PG_FUNCTION_INFO_V1(laplace_path4d_gist_distance);
Datum laplace_path4d_gist_distance(PG_FUNCTION_ARGS){
    GISTENTRY *e = (GISTENTRY *)PG_GETARG_POINTER(0);
    StrategyNumber strategy = (StrategyNumber)PG_GETARG_UINT16(2); bool *recheck = (bool *)PG_GETARG_POINTER(4);
    if (strategy != LAPLACE_KNN && strategy != LAPLACE_KNN_HAUSDORFF) ereport(ERROR, (errmsg("laplace_path4d_ops: unknown strategy %u", strategy)));
    const PathKey *k = (const PathKey *)DatumGetPointer(e->key);
    Geo q = path_of(PG_GETARG_DATUM(1)); const double *x = xyzm(&q); size_t n = q.p.n;
    double b2 = 0;
    for (size_t i = 0; i < n; i++) { double g = box_gap2(&k->box, x + 4 * i); if (g > b2) b2 = g; }
    if (strategy == LAPLACE_KNN && GIST_LEAF(e)) {   /* the ends pair up under Fréchet only */
        double f = d2_of_f32(x, k->first), l = d2_of_f32(x + 4 * (n - 1), k->last);
        if (f > b2) b2 = f;
        if (l > b2) b2 = l;
    }
    *recheck = true;   /* a lower bound: the executor computes <%> on the row and reorders */
    PG_RETURN_FLOAT8(sqrt(b2));
}

PG_FUNCTION_INFO_V1(laplace_hausdorff4d);
Datum laplace_hausdorff4d(PG_FUNCTION_ARGS){
    Geo a = path_of(PG_GETARG_DATUM(0)), b = path_of(PG_GETARG_DATUM(1));
    PG_RETURN_FLOAT8(lp_hausdorff4(xyzm(&a), a.p.n, xyzm(&b), b.p.n));
}

/* ---------------------------------------------------------------- 4D boxes as Hilbert ranges
 * The cells of the box between two corners, as ranges of the stored Hilbert values: a table's hilbert column holds
 * lp_hilbert4 XOR 2^63 (bigint order = Hilbert order), so each range maps to one btree range. The cells come from
 * the corners as lp_hilbert4 makes them (lp_hilbert4_axis), so the ranges cover every point in the box; a cell on the
 * box's faces also holds points outside it, which laplace_within4d filters. Flat: lo, hi, lo, hi, ... */
PG_FUNCTION_INFO_V1(laplace_hilbert_ranges_flat);
Datum laplace_hilbert_ranges_flat(PG_FUNCTION_ARGS){
    const double *a = point_of(PG_GETARG_DATUM(0)), *b = point_of(PG_GETARG_DATUM(1)); int32 cap = PG_GETARG_INT32(2);
    if (cap < 1 || cap > 65536) ereport(ERROR, (errmsg("laplace_hilbert_ranges: the budget is 1 to 65536 ranges, not %d", cap)));
    uint32_t lo[4], hi[4];
    for (int d = 0; d < 4; d++) { lo[d] = lp_hilbert4_axis(a[d]); hi[d] = lp_hilbert4_axis(b[d]); }
    lp_hrange *r = palloc(sizeof *r * (size_t)cap);
    size_t n = lp_hilbert4_ranges(lo, hi, r, (size_t)cap);
    Datum *v = palloc(sizeof *v * (2 * n + 1));
    for (size_t i = 0; i < n; i++) {
        v[2 * i] = Int64GetDatum((int64)(r[i].lo ^ 0x8000000000000000ull));
        v[2 * i + 1] = Int64GetDatum((int64)(r[i].hi ^ 0x8000000000000000ull));
    }
    PG_RETURN_ARRAYTYPE_P(construct_array_builtin(v, (int)(2 * n), INT8OID));
}

/* A point inside the box between two corners, faces included. */
PG_FUNCTION_INFO_V1(laplace_within4d);
Datum laplace_within4d(PG_FUNCTION_ARGS){
    const double *p = point_of(PG_GETARG_DATUM(0)), *a = point_of(PG_GETARG_DATUM(1)), *b = point_of(PG_GETARG_DATUM(2));
    for (int d = 0; d < 4; d++) if (!(p[d] >= a[d] && p[d] <= b[d])) PG_RETURN_BOOL(false);
    PG_RETURN_BOOL(true);
}

/* ---------------------------------------------------------------- sorted build */
/* The sort compares each key's Hilbert index, computed once per key as its abbreviation (a Datum is 64 bits), so the
 * full comparison is only the fallback the sort may ask for. */
static int hilbert_cmp(Datum a, Datum b, SortSupport ssup){
    uint64 x = box_hilbert((const Box4 *)DatumGetPointer(a)), y = box_hilbert((const Box4 *)DatumGetPointer(b));
    (void)ssup;
    return x < y ? -1 : x > y;
}
static Datum hilbert_abbrev(Datum key, SortSupport ssup){ (void)ssup; return UInt64GetDatum(box_hilbert((const Box4 *)DatumGetPointer(key))); }
static bool hilbert_abbrev_abort(int memtupcount, SortSupport ssup){ (void)memtupcount; (void)ssup; return false; }
PG_FUNCTION_INFO_V1(laplace_point4d_gist_sortsupport);
Datum laplace_point4d_gist_sortsupport(PG_FUNCTION_ARGS){
    SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);
    if (ssup->abbreviate) {
        ssup->comparator = ssup_datum_unsigned_cmp;
        ssup->abbrev_converter = hilbert_abbrev;
        ssup->abbrev_abort = hilbert_abbrev_abort;
        ssup->abbrev_full_comparator = hilbert_cmp;
    } else
        ssup->comparator = hilbert_cmp;
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(laplace_hilbert_ranges);
Datum laplace_hilbert_ranges(PG_FUNCTION_ARGS){
    TupleDesc descriptor;
    Tuplestorestate *output = set_begin(fcinfo, "laplace_hilbert_ranges", &descriptor);
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();
    int32 cap = PG_ARGISNULL(2) ? 64 : PG_GETARG_INT32(2);
    if (cap < 1 || cap > 65536)
        ereport(ERROR, (errmsg("laplace_hilbert_ranges: the budget is 1 to 65536 ranges, not %d", cap)));
    const double *a = point_of(PG_GETARG_DATUM(0)), *b = point_of(PG_GETARG_DATUM(1));
    uint32_t lo[4], hi[4];
    for (int d = 0; d < 4; d++) { lo[d] = lp_hilbert4_axis(a[d]); hi[d] = lp_hilbert4_axis(b[d]); }
    lp_hrange *r = palloc(sizeof *r * (size_t)cap);
    size_t n = lp_hilbert4_ranges(lo, hi, r, (size_t)cap);
    for (size_t i = 0; i < n; i++) {
        Datum v[2] = {
            Int64GetDatum((int64)(r[i].lo ^ 0x8000000000000000ull)),
            Int64GetDatum((int64)(r[i].hi ^ 0x8000000000000000ull))
        };
        bool nl[2] = {false, false};
        set_put(fcinfo, output, descriptor, v, nl);
    }
    pfree(r);
    return (Datum)0;
}

PG_FUNCTION_INFO_V1(laplace_hilbert_ball_ranges_flat);
Datum laplace_hilbert_ball_ranges_flat(PG_FUNCTION_ARGS){
    const double *pt = point_of(PG_GETARG_DATUM(0));
    double radius = PG_GETARG_FLOAT8(1);
    int32 cap = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 64;
    if (cap < 1 || cap > 65536)
        ereport(ERROR, (errmsg("laplace_hilbert_ball_ranges: the budget is 1 to 65536 ranges, not %d", cap)));
    if (isnan(radius) || radius < 0.0)
        ereport(ERROR, (errmsg("laplace_hilbert_ball_ranges: radius must be non-negative")));
    lp_coord c;
    lp_coord_trunc(pt, &c);
    lp_hrange *r = palloc(sizeof *r * (size_t)cap);
    size_t n = lp_hilbert4_ball_ranges(&c, radius, r, (size_t)cap);
    Datum *v = palloc(sizeof *v * (2 * n + 1));
    for (size_t i = 0; i < n; i++) {
        v[2 * i] = Int64GetDatum((int64)(r[i].lo ^ 0x8000000000000000ull));
        v[2 * i + 1] = Int64GetDatum((int64)(r[i].hi ^ 0x8000000000000000ull));
    }
    ArrayType *arr = construct_array_builtin(v, (int)(2 * n), INT8OID);
    pfree(v);
    pfree(r);
    PG_RETURN_ARRAYTYPE_P(arr);
}

PG_FUNCTION_INFO_V1(laplace_hilbert_ball_ranges);
Datum laplace_hilbert_ball_ranges(PG_FUNCTION_ARGS){
    TupleDesc descriptor;
    Tuplestorestate *output = set_begin(fcinfo, "laplace_hilbert_ball_ranges", &descriptor);
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();
    const double *pt = point_of(PG_GETARG_DATUM(0));
    double radius = PG_GETARG_FLOAT8(1);
    int32 cap = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 64;
    if (cap < 1 || cap > 65536)
        ereport(ERROR, (errmsg("laplace_hilbert_ball_ranges: the budget is 1 to 65536 ranges, not %d", cap)));
    if (isnan(radius) || radius < 0.0)
        ereport(ERROR, (errmsg("laplace_hilbert_ball_ranges: radius must be non-negative")));
    lp_coord c;
    lp_coord_trunc(pt, &c);
    lp_hrange *r = palloc(sizeof *r * (size_t)cap);
    size_t n = lp_hilbert4_ball_ranges(&c, radius, r, (size_t)cap);
    for (size_t i = 0; i < n; i++) {
        Datum v[2] = {
            Int64GetDatum((int64)(r[i].lo ^ 0x8000000000000000ull)),
            Int64GetDatum((int64)(r[i].hi ^ 0x8000000000000000ull))
        };
        bool nl[2] = {false, false};
        set_put(fcinfo, output, descriptor, v, nl);
    }
    pfree(r);
    return (Datum)0;
}

static bytea *make_point4d_geom(const double p[4]){
    bytea *g = palloc0(48);
    SET_VARSIZE(g, 48);
    uint8 *b = (uint8 *)g;
    b[7] = G2_VERSION | G2_Z | G2_M;
    uint32 type = 1, n = 1;
    memcpy(b + 8, &type, 4);
    memcpy(b + 12, &n, 4);
    memcpy(b + 16, p, 32);
    return g;
}

typedef struct { lp_id id; int16 tier; double p[4]; double distance; } KnnCand;
typedef struct {
    Relation part, idx;
    TupleTableSlot *slot;
    IndexScanDesc scan;
    AttrNumber a_id, a_tier, a_coord;
    double bound;
    bool active;
} KnnPartScan;

static int knn_cand_better(const KnnCand *a, const KnnCand *b){
    if (isnan(a->distance) != isnan(b->distance)) return isnan(a->distance) ? 1 : -1;
    if (a->distance < b->distance) return -1;
    if (a->distance > b->distance) return 1;
    return memcmp(a->id.b, b->id.b, sizeof(lp_id));
}

static void knn_insert_topk(KnnCand *top, int *count, int k, const KnnCand *cand){
    if (*count < k) {
        int i = (*count)++;
        while (i > 0 && knn_cand_better(cand, &top[i - 1]) < 0) {
            top[i] = top[i - 1];
            i--;
        }
        top[i] = *cand;
    } else if (knn_cand_better(cand, &top[k - 1]) < 0) {
        int i = k - 1;
        while (i > 0 && knn_cand_better(cand, &top[i - 1]) < 0) {
            top[i] = top[i - 1];
            i--;
        }
        top[i] = *cand;
    }
}

static void knn_advance_partition(KnnPartScan *sc, const double query[4], const double uquery[4],
                                  bool angular, int16 min_tier, bool has_max_tier, int16 max_tier,
                                  const lp_id *exclude_id, KnnCand *top, int *count, int k){
    while (index_getnext_slot(sc->scan, ForwardScanDirection, sc->slot)) {
        CHECK_FOR_INTERRUPTS();
        bool ni = false, nt = false, nc = false;
        Datum di = slot_getattr(sc->slot, sc->a_id, &ni);
        Datum dt = slot_getattr(sc->slot, sc->a_tier, &nt);
        Datum dc = slot_getattr(sc->slot, sc->a_coord, &nc);
        if (ni || nt || nc)
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("laplace_knn4d: null entity geometry metadata")));
        int16 tier = DatumGetInt16(dt);
        double bound = (!sc->scan->xs_orderbynulls || !sc->scan->xs_orderbynulls[0])
            ? DatumGetFloat8(sc->scan->xs_orderbyvals[0]) : INFINITY;
        const double *cp = point_of(dc);
        double dist;
        if (angular) {
            double uc[4];
            direction_of(cp, uc);
            dist = angle_of_chord(lp_distance4(uc, uquery));
        } else {
            dist = lp_distance4(query, cp);
        }
        const uint8 *id_bytes = (const uint8 *)DatumGetPointer(di);
        bool tier_ok = tier >= min_tier && (!has_max_tier || tier <= max_tier);
        bool id_ok = !exclude_id || memcmp(id_bytes, exclude_id->b, sizeof(lp_id)) != 0;
        if (tier_ok && id_ok && !isnan(dist)) {
            KnnCand cand = { .tier = tier, .distance = dist };
            memcpy(cand.id.b, id_bytes, sizeof(lp_id));
            memcpy(cand.p, cp, sizeof(cand.p));
            knn_insert_topk(top, count, k, &cand);
        }
        ExecClearTuple(sc->slot);
        sc->bound = isnan(bound) ? dist : bound;
        if (isnan(sc->bound)) {
            sc->active = false;
            return;
        }
        /* Stop once the lower bound exceeds the current k-th best distance;
         * otherwise keep pulling in best-first GiST order if filtered out. */
        if (*count >= k && !isnan(sc->bound) && sc->bound > top[k - 1].distance) {
            sc->active = false;
            return;
        }
        sc->active = true;
        return;
    }
    sc->active = false;
}

int lp_pg_knn4d(const double query[4], int k, bool angular, int16 min_tier, bool has_max_tier, int16 max_tier,
                const lp_id *exclude_id, PgKnnHit *out, MemoryContext result_ctx){
    if (k <= 0 || (has_max_tier && min_tier > max_tier)) return 0;
    Oid target = lp_pg_relation("entity");
    const char *selected[] = {"id", "tier", "coord"};
    const char *column;
    Relation parent = lp_pg_read_parent(target, &column, selected, 3);
    Oid geom_oid = get_atttype(target, get_attnum(target, "coord"));
    table_close(parent, NoLock);

    const Oid *parts = NULL, *aux = NULL, *aux2 = NULL;
    const int16 *min_tiers = NULL, *max_tiers = NULL;
    int nparts = lp_pg_canonical_partitions("entity", &parts, NULL, &aux, &aux2);
    (void)lp_pg_canonical_tier_bounds("entity", &min_tiers, &max_tiers);
    const Oid *gists = angular ? aux2 : aux;

    bytea *qgeom = make_point4d_geom(query);
    Datum qdatum = PointerGetDatum(qgeom);
    double uquery[4];
    if (angular) direction_of(query, uquery);
    if (angular && isnan(uquery[0])) { pfree(qgeom); return 0; }

    KnnPartScan *scans = palloc0((Size)nparts * sizeof(*scans));
    KnnCand *top = palloc((Size)k * sizeof(*top));
    int found = 0;
    Snapshot snap = GetActiveSnapshot();
    int active_count = 0;

    PG_TRY(); {
        for (int p = 0; p < nparts; p++) {
            KnnPartScan *sc = &scans[p];
            if (min_tiers && max_tiers &&
                (max_tiers[p] < min_tier || (has_max_tier && min_tiers[p] > max_tier)))
                continue;
            if (!OidIsValid(gists[p]))
                ereport(ERROR, (errmsg("laplace_knn4d: entity partition missing canonical %s GiST index",
                    angular ? "laplace_angular4d_ops" : "laplace_point4d_ops")));
            sc->part = table_open(parts[p], AccessShareLock);
            sc->idx = index_open(gists[p], AccessShareLock);
            sc->a_id = get_attnum(parts[p], "id");
            sc->a_tier = get_attnum(parts[p], "tier");
            sc->a_coord = get_attnum(parts[p], "coord");
            Oid dist_op = get_opfamily_member(sc->idx->rd_opfamily[0], geom_oid, geom_oid, LAPLACE_KNN);
            if (!OidIsValid(dist_op))
                ereport(ERROR, (errmsg("laplace_knn4d: GiST opfamily missing distance operator")));
            ScanKeyData okey;
            ScanKeyEntryInitialize(&okey, SK_ORDER_BY, 1, LAPLACE_KNN, geom_oid, InvalidOid,
                get_opcode(dist_op), qdatum);
            sc->slot = table_slot_create(sc->part, NULL);
            sc->scan = index_beginscan(sc->part, sc->idx, snap, NULL, 0, 1);
            index_rescan(sc->scan, NULL, 0, &okey, 1);
            knn_advance_partition(sc, query, uquery, angular, min_tier, has_max_tier, max_tier, exclude_id, top, &found, k);
            if (sc->active) active_count++;
        }

        while (active_count > 0) {
            CHECK_FOR_INTERRUPTS();
            int best_p = -1;
            for (int p = 0; p < nparts; p++) {
                if (!scans[p].active) continue;
                if (best_p < 0 || (!isnan(scans[p].bound) && (isnan(scans[best_p].bound) || scans[p].bound < scans[best_p].bound)))
                    best_p = p;
            }
            if (best_p < 0) break;
            if (found >= k && (isnan(scans[best_p].bound) || scans[best_p].bound > top[k - 1].distance))
                break;
            knn_advance_partition(&scans[best_p], query, uquery, angular, min_tier, has_max_tier, max_tier, exclude_id, top, &found, k);
            if (!scans[best_p].active) active_count--;
        }
    } PG_FINALLY(); {
        for (int p = 0; p < nparts; p++) {
            if (scans[p].scan) index_endscan(scans[p].scan);
            if (scans[p].slot) ExecDropSingleTupleTableSlot(scans[p].slot);
            if (scans[p].idx) index_close(scans[p].idx, NoLock);
            if (scans[p].part) table_close(scans[p].part, NoLock);
        }
        pfree(scans);
        pfree(qgeom);
    } PG_END_TRY();

    MemoryContext old = MemoryContextSwitchTo(result_ctx ? result_ctx : CurrentMemoryContext);
    for (int i = 0; i < found; i++) {
        out[i].id = top[i].id;
        out[i].tier = top[i].tier;
        out[i].coord = PointerGetDatum(make_point4d_geom(top[i].p));
        out[i].distance = top[i].distance;
    }
    MemoryContextSwitchTo(old);
    pfree(top);
    return found;
}

PG_FUNCTION_INFO_V1(laplace_knn4d);
Datum laplace_knn4d(PG_FUNCTION_ARGS){
    TupleDesc descriptor;
    Tuplestorestate *output = set_begin(fcinfo, "laplace_knn4d", &descriptor);
    if (PG_ARGISNULL(0)) return (Datum)0;
    const double *q = point_of(PG_GETARG_DATUM(0));
    int32 k = PG_ARGISNULL(1) ? 16 : PG_GETARG_INT32(1);
    bool angular = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
    int16 min_tier = PG_ARGISNULL(3) ? 0 : PG_GETARG_INT16(3);
    bool has_max_tier = (PG_NARGS() > 4 && !PG_ARGISNULL(4));
    int16 max_tier = has_max_tier ? PG_GETARG_INT16(4) : 0;
    lp_id excl;
    const lp_id *exclude_id = NULL;
    if (PG_NARGS() > 5 && !PG_ARGISNULL(5)) {
        memcpy(excl.b, PG_GETARG_POINTER(5), sizeof(lp_id));
        exclude_id = &excl;
    }
    if (k < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_knn4d: k must be non-negative")));
    if (k == 0 || (has_max_tier && min_tier > max_tier)) return (Datum)0;
    PgKnnHit *hits = palloc((Size)k * sizeof(*hits));
    int found = lp_pg_knn4d(q, k, angular, min_tier, has_max_tier, max_tier, exclude_id, hits, CurrentMemoryContext);
    for (int i = 0; i < found; i++) {
        Datum v[5] = {
            Int64GetDatum((int64)i + 1),
            id_datum(&hits[i].id),
            Int16GetDatum(hits[i].tier),
            hits[i].coord,
            Float8GetDatum(hits[i].distance)
        };
        bool nl[5] = {false, false, false, false, false};
        set_put(fcinfo, output, descriptor, v, nl);
        pfree(DatumGetPointer(v[1]));
        pfree(DatumGetPointer(v[3]));
    }
    pfree(hits);
    return (Datum)0;
}

/* ---------------------------------------------------------------- Direct-AM Hilbert B-tree kernels */
typedef struct {
    lp_id id;
    int64 hilbert;
} HilbertNeighbor;

static Oid find_btree_on(Relation part, AttrNumber col0, int min_keys, AttrNumber col1)
{
    List *idxs = RelationGetIndexList(part);
    ListCell *lc;
    Oid found = InvalidOid;
    foreach(lc, idxs) {
        Oid ioid = lfirst_oid(lc);
        Relation idx = index_open(ioid, AccessShareLock);
        if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index &&
            idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive &&
            idx->rd_index->indnkeyatts >= min_keys &&
            idx->rd_index->indkey.values[0] == col0 &&
            (min_keys < 2 || idx->rd_index->indkey.values[1] == col1)) {
            found = ioid;
            index_close(idx, NoLock);
            break;
        }
        index_close(idx, NoLock);
    }
    list_free(idxs);
    return found;
}

static int cmp_hilb_asc(const void *pa, const void *pb)
{
    const HilbertNeighbor *a = pa, *b = pb;
    if (a->hilbert != b->hilbert) return a->hilbert < b->hilbert ? -1 : 1;
    return memcmp(a->id.b, b->id.b, sizeof(lp_id));
}

static int cmp_hilb_desc(const void *pa, const void *pb)
{
    const HilbertNeighbor *a = pa, *b = pb;
    if (a->hilbert != b->hilbert) return a->hilbert > b->hilbert ? -1 : 1;
    return -memcmp(a->id.b, b->id.b, sizeof(lp_id));
}

static void insert_hilb_topk(HilbertNeighbor *buf, int *cnt, int cap, const HilbertNeighbor *item, bool desc)
{
    if (cap <= 0) return;
    if (*cnt < cap) {
        int i = (*cnt)++;
        while (i > 0 && (desc ? cmp_hilb_desc(item, &buf[i - 1]) : cmp_hilb_asc(item, &buf[i - 1])) < 0) {
            buf[i] = buf[i - 1];
            i--;
        }
        buf[i] = *item;
    } else if ((desc ? cmp_hilb_desc(item, &buf[cap - 1]) : cmp_hilb_asc(item, &buf[cap - 1])) < 0) {
        int i = cap - 1;
        while (i > 0 && (desc ? cmp_hilb_desc(item, &buf[i - 1]) : cmp_hilb_asc(item, &buf[i - 1])) < 0) {
            buf[i] = buf[i - 1];
            i--;
        }
        buf[i] = *item;
    }
}

/* Direct-AM 2-sided B-tree seek on (tier, hilbert, id) matching geometry.prompt.hilbert tie semantics. */
PG_FUNCTION_INFO_V1(laplace_hilbert_neighbors);
Datum laplace_hilbert_neighbors(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_hilbert_neighbors", &td);
    if (PG_ARGISNULL(0)) return (Datum)0;
    Geo g = geo_of(PG_GETARG_DATUM(0));
    if (g.type != 1 || g.p.n != 1) ereport(ERROR, (errmsg("laplace_hilbert_neighbors: expected a POINT ZM")));
    lp_coord anchor_c;
    lp_coord_trunc(xyzm(&g), &anchor_c);
    int64 anchor_key = (int64)lp_hilbert_key(lp_hilbert4(&anchor_c));

    bool has_excl = (PG_NARGS() > 1 && !PG_ARGISNULL(1));
    lp_id excl = {0};
    if (has_excl) memcpy(excl.b, PG_GETARG_POINTER(1), sizeof(lp_id));
    int32 k = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 16;
    int16 tier = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT16(3) : 0;
    if (k <= 0) return (Datum)0;
    if (k > 65536) k = 65536;

    Oid target = lp_pg_relation("entity");
    const char *cols[] = { "id", "tier", "hilbert" }, *col = NULL;
    Relation parent = lp_pg_read_parent(target, &col, cols, 3);
    table_close(parent, NoLock);

    const Oid *parts = NULL;
    const int16 *min_tiers = NULL, *max_tiers = NULL;
    int nparts = lp_pg_canonical_partitions("entity", &parts, NULL, NULL, NULL);
    (void)lp_pg_canonical_tier_bounds("entity", &min_tiers, &max_tiers);
    Snapshot snap = GetActiveSnapshot();

    HilbertNeighbor *upper = palloc((Size)k * sizeof(*upper));
    HilbertNeighbor *lower = palloc((Size)(k + 1) * sizeof(*lower));
    int n_upper = 0, n_lower = 0;

    for (int p = 0; p < nparts; p++) {
        CHECK_FOR_INTERRUPTS();
        if (min_tiers && max_tiers && (tier < min_tiers[p] || tier > max_tiers[p]))
            continue;
        Relation part = table_open(parts[p], AccessShareLock);
        AttrNumber a_id = get_attnum(parts[p], "id");
        AttrNumber a_tier = get_attnum(parts[p], "tier");
        AttrNumber a_hilb = get_attnum(parts[p], "hilbert");
        Oid idx_oid = find_btree_on(part, a_tier, 2, a_hilb);
        if (!OidIsValid(idx_oid))
            ereport(ERROR, (errmsg("laplace_hilbert_neighbors: entity partition missing (tier, hilbert, id) index")));
        Relation idx = index_open(idx_oid, AccessShareLock);
        TupleTableSlot *slot = table_slot_create(part, NULL);

        /* Upper seek: tier = tier AND hilbert >= anchor_key */
        ScanKeyData sk[2];
        ScanKeyEntryInitialize(&sk[0], 0, 1, BTEqualStrategyNumber, INT2OID,
                               idx->rd_indcollation[0], F_INT2EQ, Int16GetDatum(tier));
        ScanKeyEntryInitialize(&sk[1], 0, 2, BTGreaterEqualStrategyNumber, INT8OID,
                               idx->rd_indcollation[1], F_INT8GE, Int64GetDatum(anchor_key));
        IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 2, 0);
        index_rescan(scan, sk, 2, NULL, 0);
        while (index_getnext_slot(scan, ForwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool ni = true, nh = true;
            Datum di = slot_getattr(slot, a_id, &ni);
            Datum dh = slot_getattr(slot, a_hilb, &nh);
            if (!ni && !nh) {
                HilbertNeighbor cand = { .hilbert = DatumGetInt64(dh) };
                memcpy(cand.id.b, DatumGetPointer(di), sizeof(lp_id));
                if (has_excl && lp_id_eq(&cand.id, &excl)) { ExecClearTuple(slot); continue; }
                if (n_upper >= k && cmp_hilb_asc(&cand, &upper[k - 1]) >= 0) { ExecClearTuple(slot); break; }
                insert_hilb_topk(upper, &n_upper, k, &cand, false);
            }
            ExecClearTuple(slot);
        }

        /* Lower seek: tier = tier AND hilbert < anchor_key (keep k + 1 to detect tie boundary) */
        ScanKeyEntryInitialize(&sk[1], 0, 2, BTLessStrategyNumber, INT8OID,
                               idx->rd_indcollation[1], F_INT8LT, Int64GetDatum(anchor_key));
        index_rescan(scan, sk, 2, NULL, 0);
        while (index_getnext_slot(scan, BackwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool ni = true, nh = true;
            Datum di = slot_getattr(slot, a_id, &ni);
            Datum dh = slot_getattr(slot, a_hilb, &nh);
            if (!ni && !nh) {
                HilbertNeighbor cand = { .hilbert = DatumGetInt64(dh) };
                memcpy(cand.id.b, DatumGetPointer(di), sizeof(lp_id));
                if (has_excl && lp_id_eq(&cand.id, &excl)) { ExecClearTuple(slot); continue; }
                if (n_lower >= k + 1 && cmp_hilb_desc(&cand, &lower[k]) >= 0) { ExecClearTuple(slot); break; }
                insert_hilb_topk(lower, &n_lower, k + 1, &cand, true);
            }
            ExecClearTuple(slot);
        }

        index_endscan(scan);
        ExecDropSingleTupleTableSlot(slot);
        index_close(idx, NoLock);
        table_close(part, NoLock);
    }

    bool has_tie = (n_lower == k + 1 && lower[k - 1].hilbert == lower[k].hilbert);
    int64 tie_key = has_tie ? lower[k - 1].hilbert : 0;
    HilbertNeighbor *tie_rows = has_tie ? palloc((Size)k * sizeof(*tie_rows)) : NULL;
    int n_tie = 0;

    if (has_tie) {
        for (int p = 0; p < nparts; p++) {
            CHECK_FOR_INTERRUPTS();
            if (min_tiers && max_tiers && (tier < min_tiers[p] || tier > max_tiers[p]))
                continue;
            Relation part = table_open(parts[p], AccessShareLock);
            AttrNumber a_id = get_attnum(parts[p], "id");
            AttrNumber a_tier = get_attnum(parts[p], "tier");
            AttrNumber a_hilb = get_attnum(parts[p], "hilbert");
            Oid idx_oid = find_btree_on(part, a_tier, 2, a_hilb);
            Relation idx = index_open(idx_oid, AccessShareLock);
            TupleTableSlot *slot = table_slot_create(part, NULL);

            ScanKeyData sk[2];
            ScanKeyEntryInitialize(&sk[0], 0, 1, BTEqualStrategyNumber, INT2OID,
                                   idx->rd_indcollation[0], F_INT2EQ, Int16GetDatum(tier));
            ScanKeyEntryInitialize(&sk[1], 0, 2, BTEqualStrategyNumber, INT8OID,
                                   idx->rd_indcollation[1], F_INT8EQ, Int64GetDatum(tie_key));
            IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 2, 0);
            index_rescan(scan, sk, 2, NULL, 0);
            while (index_getnext_slot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool ni = true, nh = true;
                Datum di = slot_getattr(slot, a_id, &ni);
                Datum dh = slot_getattr(slot, a_hilb, &nh);
                if (!ni && !nh) {
                    HilbertNeighbor cand = { .hilbert = DatumGetInt64(dh) };
                    memcpy(cand.id.b, DatumGetPointer(di), sizeof(lp_id));
                    if (has_excl && lp_id_eq(&cand.id, &excl)) { ExecClearTuple(slot); continue; }
                    if (n_tie >= k && cmp_hilb_asc(&cand, &tie_rows[k - 1]) >= 0) { ExecClearTuple(slot); break; }
                    insert_hilb_topk(tie_rows, &n_tie, k, &cand, false);
                }
                ExecClearTuple(slot);
            }
            index_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
            index_close(idx, NoLock);
            table_close(part, NoLock);
        }
    }

    for (int i = 0; i < n_upper; i++) {
        Datum v[3] = { id_datum(&upper[i].id), Int64GetDatum(upper[i].hilbert), Int64GetDatum(anchor_key) };
        bool nl[3] = { false, false, false };
        set_put(fcinfo, ts, td, v, nl);
        pfree(DatumGetPointer(v[0]));
    }
    int lower_keep = Min(n_lower, k);
    for (int i = 0; i < lower_keep; i++) {
        if (has_tie && lower[i].hilbert == tie_key) continue;
        Datum v[3] = { id_datum(&lower[i].id), Int64GetDatum(lower[i].hilbert), Int64GetDatum(anchor_key) };
        bool nl[3] = { false, false, false };
        set_put(fcinfo, ts, td, v, nl);
        pfree(DatumGetPointer(v[0]));
    }
    for (int i = 0; i < n_tie; i++) {
        Datum v[3] = { id_datum(&tie_rows[i].id), Int64GetDatum(tie_rows[i].hilbert), Int64GetDatum(anchor_key) };
        bool nl[3] = { false, false, false };
        set_put(fcinfo, ts, td, v, nl);
        pfree(DatumGetPointer(v[0]));
    }

    if (tie_rows) pfree(tie_rows);
    pfree(upper);
    pfree(lower);
    return (Datum)0;
}

typedef struct {
    lp_id id;
    double c[4];
    int64 hilbert;
    int32 path_len;
    bool found;
} ConstellationPick;

static void constellation_path_len(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    lp_idmap *lens = context;
    Geo g = geo_of(path);
    int32 len = (int32)Min((size_t)PG_INT32_MAX, lp_path_len(g.p));
    *(int32 *)lp_idmap_get(lens, id, NULL) = len;
}

static void constellation_stride_id(int32 i, int32 n, lp_id *out)
{
    memset(out->b, 0, sizeof(out->b));
    if (i <= 0 || n <= 0) return;
    unsigned __int128 num = ((unsigned __int128)(uint32)i) << 64;
    uint64 hi = (uint64)(num / (uint32)n);
    uint64 rem = (uint64)(num % (uint32)n);
    uint64 lo = (uint64)((((unsigned __int128)rem) << 64) / (uint32)n);
    lp_put_be(out->b, hi, 8);
    lp_put_be(out->b + 8, lo, 8);
}

/* Direct-AM equi-width BLAKE3 identity-stride constellation sampler over entity pkeys (100% inside B^4). */
PG_FUNCTION_INFO_V1(laplace_constellation_sample);
Datum laplace_constellation_sample(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_constellation_sample", &td);
    int32 n = PG_ARGISNULL(0) ? 64 : PG_GETARG_INT32(0);
    if (n < 0) n = 0;
    if (n > 1024) n = 1024;
    if (n == 0) return (Datum)0;

    Oid target = lp_pg_relation("entity");
    const char *cols[] = { "id", "coord", "hilbert" }, *col = NULL;
    Relation parent = lp_pg_read_parent(target, &col, cols, 3);
    Oid native_id = get_atttype(target, get_attnum(target, "id"));
    AttrNumber parent_id_att = get_attnum(target, "id");
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;
    bool by_id_range = (pkey && pdesc && pkey->strategy == PARTITION_STRATEGY_RANGE &&
                        pkey->partnatts == 1 && pkey->partattrs[0] == parent_id_att);

    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions("entity", &parts, &pks, NULL, NULL);
    Snapshot snap = GetActiveSnapshot();

    typedef struct {
        Relation part, idx;
        TupleTableSlot *slot;
        IndexScanDesc scan;
        Oid part_oid;
        RegProcedure ge_proc;
        AttrNumber a_id, a_coord, a_hilb;
    } IdPart;
    IdPart *hp = palloc0((Size)nparts * sizeof(*hp));
    int *ordered = palloc(sizeof(int) * (Size)(nparts > 0 ? nparts : 1));
    int n_ordered = 0;
    ConstellationPick *picks = palloc0((Size)n * sizeof(*picks));
    lp_idmap *seen = lp_idmap_new();
    lp_idmap *lens = lp_idmap_sized(sizeof(int32));
    lp_vec(lp_id) distinct_ids = {0};

    PG_TRY();
    {
        for (int p = 0; p < nparts; p++) {
            hp[p].part_oid = parts[p];
            Relation part = table_open(parts[p], AccessShareLock);
            if (RelationGetNumberOfBlocks(part) == 0) {
                table_close(part, NoLock);
                continue;
            }
            hp[p].part = part;
            hp[p].a_id = get_attnum(parts[p], "id");
            hp[p].a_coord = get_attnum(parts[p], "coord");
            hp[p].a_hilb = get_attnum(parts[p], "hilbert");
            Oid idx_oid = (pks && OidIsValid(pks[p])) ? pks[p] : find_btree_on(part, hp[p].a_id, 1, InvalidAttrNumber);
            if (!OidIsValid(idx_oid))
                ereport(ERROR, (errmsg("laplace_constellation_sample: entity partition missing (id) unique index")));
            hp[p].idx = index_open(idx_oid, AccessShareLock);
            Oid ge_op = get_opfamily_member(hp[p].idx->rd_opfamily[0], native_id, native_id, BTGreaterEqualStrategyNumber);
            if (!OidIsValid(ge_op))
                ereport(ERROR, (errmsg("laplace_constellation_sample: entity (id) index missing >= operator")));
            hp[p].ge_proc = get_opcode(ge_op);
            hp[p].slot = table_slot_create(hp[p].part, NULL);
            hp[p].scan = index_beginscan(hp[p].part, hp[p].idx, snap, NULL, 1, 0);
        }

        if (by_id_range && pdesc->boundinfo && pdesc->boundinfo->ndatums >= 2) {
            PartitionBoundInfo bi = pdesc->boundinfo;
            for (int b = 0; b + 1 < bi->ndatums; b++) {
                int pi = bi->indexes[b + 1];
                if (pi < 0 || pi >= pdesc->nparts) continue;
                Oid poid = pdesc->oids[pi];
                for (int p = 0; p < nparts; p++) {
                    if (hp[p].part_oid == poid) {
                        ordered[n_ordered++] = p;
                        break;
                    }
                }
            }
        }
        if (n_ordered == 0) {
            for (int p = 0; p < nparts; p++) ordered[n_ordered++] = p;
        }

        for (int32 i = 0; i < n; i++) {
            CHECK_FOR_INTERRUPTS();
            lp_id lo_id, hi_id;
            constellation_stride_id(i, n, &lo_id);
            bool has_hi = (i + 1 < n);
            if (has_hi) constellation_stride_id(i + 1, n, &hi_id);

            int start_k = 0;
            if (by_id_range) {
                Oid start_oid = lp_pg_enrichment_partition(target, pkey, pdesc, PointerGetDatum(&lo_id));
                for (int k = 0; k < n_ordered; k++) {
                    if (hp[ordered[k]].part_oid == start_oid) { start_k = k; break; }
                }
            }

            ConstellationPick best = { .found = false };
            for (int k = start_k; k < n_ordered; k++) {
                int p = ordered[k];
                if (!hp[p].scan) continue;
                ScanKeyData sk;
                ScanKeyEntryInitialize(&sk, 0, 1, BTGreaterEqualStrategyNumber, native_id,
                                       hp[p].idx->rd_indcollation[0], hp[p].ge_proc, PointerGetDatum(&lo_id));
                index_rescan(hp[p].scan, &sk, 1, NULL, 0);
                bool done_bucket = false;
                while (index_getnext_slot(hp[p].scan, ForwardScanDirection, hp[p].slot)) {
                    bool ni = true, nc = true, nh = true;
                    Datum di = slot_getattr(hp[p].slot, hp[p].a_id, &ni);
                    Datum dc = slot_getattr(hp[p].slot, hp[p].a_coord, &nc);
                    Datum dh = slot_getattr(hp[p].slot, hp[p].a_hilb, &nh);
                    if (ni || nc || nh) { ExecClearTuple(hp[p].slot); continue; }
                    lp_id cid;
                    memcpy(cid.b, DatumGetPointer(di), sizeof(lp_id));
                    if (has_hi && memcmp(cid.b, hi_id.b, sizeof(lp_id)) >= 0) {
                        ExecClearTuple(hp[p].slot);
                        if (by_id_range) done_bucket = true;
                        break;
                    }
                    if (!best.found || memcmp(cid.b, best.id.b, sizeof(lp_id)) < 0) {
                        const double *pt = point_of(dc);
                        best.id = cid;
                        memcpy(best.c, pt, sizeof(best.c));
                        best.hilbert = DatumGetInt64(dh);
                        best.found = true;
                    }
                    ExecClearTuple(hp[p].slot);
                    if (by_id_range) done_bucket = true;
                    break;
                }
                if (done_bucket) break;
            }

            if (best.found) {
                bool fresh = false;
                lp_idmap_put(seen, &best.id, &fresh);
                if (fresh) {
                    picks[i] = best;
                    lp_push(&distinct_ids, best.id);
                }
            }
        }

        if (distinct_ids.n > 0)
            lp_pg_lookup_paths(distinct_ids.v, distinct_ids.n, false, constellation_path_len, lens);

        int32 ord = 0;
        for (int32 i = 0; i < n; i++) {
            if (!picks[i].found) continue;
            ord++;
            const ConstellationPick *pk = &picks[i];
            int32 *pl = lp_idmap_lookup(lens, &pk->id);
            int32 plen = pl ? *pl : 0;
            double r = sqrt(((pk->c[0] * pk->c[0] + pk->c[1] * pk->c[1]) + pk->c[2] * pk->c[2]) + pk->c[3] * pk->c[3]);
            Datum v[9] = {
                Int32GetDatum(ord),
                id_datum(&pk->id),
                Float8GetDatum(pk->c[0]),
                Float8GetDatum(pk->c[1]),
                Float8GetDatum(pk->c[2]),
                Float8GetDatum(pk->c[3]),
                Float8GetDatum(r),
                Int32GetDatum(plen),
                Int64GetDatum(pk->hilbert)
            };
            bool nl[9] = { false };
            set_put(fcinfo, ts, td, v, nl);
            pfree(DatumGetPointer(v[1]));
        }
    }
    PG_FINALLY();
    {
        for (int p = 0; p < nparts; p++) {
            if (hp[p].scan) index_endscan(hp[p].scan);
            if (hp[p].slot) ExecDropSingleTupleTableSlot(hp[p].slot);
            if (hp[p].idx) index_close(hp[p].idx, NoLock);
            if (hp[p].part) table_close(hp[p].part, NoLock);
        }
        table_close(parent, NoLock);
        pfree(ordered);
        pfree(hp);
        pfree(picks);
        lp_vec_free(&distinct_ids);
        lp_idmap_free(seen);
        lp_idmap_free(lens);
    }
    PG_END_TRY();
    return (Datum)0;
}
