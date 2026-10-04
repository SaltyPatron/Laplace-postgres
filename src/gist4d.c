/* 4D GiST on PostGIS geometry POINT ZM: laplace_point4d_ops (ordered by <~>, Euclidean) and laplace_angular4d_ops
 * (ordered by <=>, the angle between directions on S^3; below).
 *
 * The keys are double-precision 4D boxes (laplace_box4d, 64 bytes) built from the geometry's own coordinates, not
 * PostGIS's float32 boxes rounded outward. The ordering operator <~> is the exact 4D Euclidean distance
 * (lp_distance4). A leaf key is its point (min == max), and its distance is computed exactly as lp_distance4 computes
 * it: the same differences, squared and summed in the same order, ((dx^2 + dy^2) + dz^2) + dm^2, then a correctly
 * rounded sqrt. So the index returns rows in <~> order with no recheck. An inner key's distance takes, on each axis,
 * the gap from the query to the box (0 inside it), which is no larger than the difference to any point in the box;
 * rounding is monotone, so the bound is never above the exact distance of anything under it.
 *
 * Built sorted (sortsupport): keys in 4D Hilbert order of their centres, so CREATE INDEX writes pages in one pass
 * instead of inserting row by row. */
#include "postgres.h"
#include "fmgr.h"
#include "access/gist.h"
#include "access/stratnum.h"
#include "utils/sortsupport.h"
#include "geo.h"
#include "laplace/laplace.h"
#include <math.h>

typedef struct { double lo[4], hi[4]; } Box4;

#define LAPLACE_KNN 15   /* ORDER BY geometry <~> geometry */

/* ---------------------------------------------------------------- the key type */
static const double *point_of(Datum d){
    Geo g = geo_of(d);
    if (g.type != 1 || g.n != 1) ereport(ERROR, (errmsg("laplace_point4d_ops: expected a POINT ZM, not an empty point or a LINESTRING")));
    return g.xyzm;
}

PG_FUNCTION_INFO_V1(laplace_box4d_in);
Datum laplace_box4d_in(PG_FUNCTION_ARGS){
    const char *s = PG_GETARG_CSTRING(0); Box4 *b = palloc(sizeof *b); int used = 0;
    if (sscanf(s, " BOX4D ( %lf %lf %lf %lf , %lf %lf %lf %lf ) %n", &b->lo[0], &b->lo[1], &b->lo[2], &b->lo[3],
               &b->hi[0], &b->hi[1], &b->hi[2], &b->hi[3], &used) != 8 || s[used] != '\0')
        ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION), errmsg("laplace_box4d: expected BOX4D(x y z m, x y z m), got \"%s\"", s)));
    PG_RETURN_POINTER(b);
}

PG_FUNCTION_INFO_V1(laplace_box4d_out);
Datum laplace_box4d_out(PG_FUNCTION_ARGS){
    const Box4 *b = (const Box4 *)PG_GETARG_POINTER(0);
    PG_RETURN_CSTRING(psprintf("BOX4D(%.17g %.17g %.17g %.17g,%.17g %.17g %.17g %.17g)",
                               b->lo[0], b->lo[1], b->lo[2], b->lo[3], b->hi[0], b->hi[1], b->hi[2], b->hi[3]));
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
static inline double angle_of_chord(double c){ double h = c / 2.0; return 2.0 * asin(h < 1.0 ? h : 1.0); }

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
static inline void box_of_point(Box4 *b, const double p[4]){ for (int d = 0; d < 4; d++) b->lo[d] = b->hi[d] = p[d]; }
static inline void box_join(Box4 *a, const Box4 *b){
    for (int d = 0; d < 4; d++) { if (b->lo[d] < a->lo[d]) a->lo[d] = b->lo[d]; if (b->hi[d] > a->hi[d]) a->hi[d] = b->hi[d]; }
}
static inline double box_edges(const Box4 *b){ return ((b->hi[0] - b->lo[0]) + (b->hi[1] - b->lo[1])) + (b->hi[2] - b->lo[2]) + (b->hi[3] - b->lo[3]); }
/* The query's distance to the box, in lp_distance4's order of operations (header). */
static inline double box_distance(const Box4 *b, const double q[4]){
    double g[4];
    for (int d = 0; d < 4; d++) g[d] = q[d] < b->lo[d] ? b->lo[d] - q[d] : q[d] > b->hi[d] ? q[d] - b->hi[d] : 0.0;
    return sqrt(((g[0] * g[0] + g[1] * g[1]) + g[2] * g[2]) + g[3] * g[3]);
}
static inline uint64 box_hilbert(const Box4 *b){
    uint32_t g[4];
    for (int d = 0; d < 4; d++) {
        double c = (b->lo[d] + b->hi[d]) / 2.0, v = (c + 1.0) / 2.0 * 65536.0;
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
 * the angle is monotone in the chord, so the order is exact with no recheck. */
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
    *recheck = false;
    PG_RETURN_FLOAT8(angle_of_chord(box_distance((const Box4 *)DatumGetPointer(e->key), u)));
}

PG_FUNCTION_INFO_V1(laplace_point4d_gist_consistent);
Datum laplace_point4d_gist_consistent(PG_FUNCTION_ARGS){
    /* The class has an ordering operator only; GiST calls this for search operators, of which there are none. */
    bool *recheck = (bool *)PG_GETARG_POINTER(4);
    *recheck = true;
    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(laplace_point4d_gist_union);
Datum laplace_point4d_gist_union(PG_FUNCTION_ARGS){
    GistEntryVector *v = (GistEntryVector *)PG_GETARG_POINTER(0); int *size = (int *)PG_GETARG_POINTER(1);
    Box4 *u = palloc(sizeof *u); *u = *(const Box4 *)DatumGetPointer(v->vector[0].key);
    for (int i = 1; i < v->n; i++) box_join(u, (const Box4 *)DatumGetPointer(v->vector[i].key));
    *size = sizeof *u;
    PG_RETURN_POINTER(u);
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
    return x->c < y->c ? -1 : x->c > y->c ? 1 : (int)x->i - (int)y->i;
}
PG_FUNCTION_INFO_V1(laplace_point4d_gist_picksplit);
Datum laplace_point4d_gist_picksplit(PG_FUNCTION_ARGS){
    GistEntryVector *v = (GistEntryVector *)PG_GETARG_POINTER(0); GIST_SPLITVEC *s = (GIST_SPLITVEC *)PG_GETARG_POINTER(1);
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
    Box4 *l = palloc(sizeof *l), *r = palloc(sizeof *r);
    for (int k = 0; k < n; k++) {
        const Box4 *b = (const Box4 *)DatumGetPointer(v->vector[c[k].i].key);
        if (k < n / 2) { if (s->spl_nleft++ == 0) *l = *b; else box_join(l, b); s->spl_left[s->spl_nleft - 1] = c[k].i; }
        else { if (s->spl_nright++ == 0) *r = *b; else box_join(r, b); s->spl_right[s->spl_nright - 1] = c[k].i; }
    }
    s->spl_ldatum = PointerGetDatum(l); s->spl_rdatum = PointerGetDatum(r);
    PG_RETURN_POINTER(s);
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
    *recheck = false;   /* a leaf's distance is lp_distance4's, bit for bit (header) */
    PG_RETURN_FLOAT8(box_distance((const Box4 *)DatumGetPointer(e->key), point_of(PG_GETARG_DATUM(1))));
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
