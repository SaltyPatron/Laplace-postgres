/* Laplace-postgres: Laplace's 4D expansion of PostgreSQL and PostGIS.
 *
 * Standard geometry types stay as they are; these functions add what Laplace needs next to what PostGIS provides.
 * They read PostGIS's serialized geometry directly (a LINESTRING ZM's vertices are 32-byte X, Y, Z, M blocks, the same
 * layout the Laplace-Native kernels use), return IDs as uuid (16 fixed bytes), and call Laplace-Native for all math. */
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/uuid.h"
#include "mb/pg_wchar.h"
#include "varatt.h"
#include "utils/guc.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "lib/stringinfo.h"
#include "laplace/laplace.h"

PG_MODULE_MAGIC_EXT(.name = "laplace", .version = "0.1");

/* ---------------------------------------------------------------- PostGIS serialized geometry (version 2) */
#define G2_Z 0x01
#define G2_M 0x02
#define G2_BBOX 0x04
#define G2_GEODETIC 0x08
#define G2_EXTENDED 0x10
#define G2_VERSION 0x40

typedef struct { uint32 type; uint32 n; const double *xyzm; } Geo;   /* POINT or LINESTRING with Z and M */

static Geo geo_of(Datum d){
    bytea *g = (bytea *)PG_DETOAST_DATUM(d); const uint8 *p = (const uint8 *)g; Geo r;
    uint8 flags = p[7];
    if (!(flags & G2_VERSION)) ereport(ERROR, (errmsg("laplace: expected a PostGIS geometry in serialization version 2")));
    if (!(flags & G2_Z) || !(flags & G2_M)) ereport(ERROR, (errmsg("laplace: expected a geometry with Z and M")));
    size_t off = 8;
    if (flags & G2_EXTENDED) off += 8;
    if (flags & G2_BBOX) off += (flags & G2_GEODETIC) ? 6 * sizeof(float) : 2 * 4 * sizeof(float);
    memcpy(&r.type, p + off, 4); memcpy(&r.n, p + off + 4, 4);
    if (r.type != 1 && r.type != 2) ereport(ERROR, (errmsg("laplace: expected a POINT or LINESTRING, got type %u", r.type)));
    r.xyzm = (const double *)(p + off + 8);
    if (off + 8 + (size_t)r.n * 32 > VARSIZE(g)) ereport(ERROR, (errmsg("laplace: truncated geometry")));
    return r;
}

/* A PostGIS LINESTRING/POINT body is laid out like an EWKB vertex block, so Native's kernels take it after a small
 * EWKB-style header built on the stack. */
static uint8 *as_ewkb(const Geo *g, size_t *len){
    *len = 9 + (size_t)g->n * 32;
    uint8 *e = palloc(*len); uint32 t = 2u | 0x80000000u | 0x40000000u, n = g->n;
    e[0] = 1; memcpy(e + 1, &t, 4); memcpy(e + 5, &n, 4); memcpy(e + 9, g->xyzm, (size_t)g->n * 32);
    return e;
}

static Datum uuid_datum(const lp_id *id){ pg_uuid_t *u = palloc(sizeof(pg_uuid_t)); memcpy(u->data, id->b, 16); return UUIDPGetDatum(u); }
static ArrayType *uuid_array(const lp_id *ids, int n){
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1));
    for (int i = 0; i < n; i++) d[i] = uuid_datum(&ids[i]);
    return construct_array(d, n, UUIDOID, UUID_LEN, false, TYPALIGN_CHAR);
}
static lp_id *ids_of(ArrayType *a, int *n){
    Datum *d; bool *nulls; deconstruct_array(a, UUIDOID, UUID_LEN, false, TYPALIGN_CHAR, &d, &nulls, n);
    lp_id *ids = palloc(sizeof(lp_id) * (*n ? *n : 1));
    for (int i = 0; i < *n; i++) {
        if (nulls[i]) ereport(ERROR, (errmsg("laplace: IDs cannot be null")));
        memcpy(ids[i].b, DatumGetUUIDP(d[i])->data, 16);
    }
    return ids;
}

/* ---------------------------------------------------------------- identity */
PG_FUNCTION_INFO_V1(laplace_cp_id);
Datum laplace_cp_id(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); lp_id id;
    if (cp < 0 || (uint32)cp >= LP_NCP) ereport(ERROR, (errmsg("laplace_cp_id: %d is outside the codespace", cp)));
    lp_id_codepoint((uint32)cp, &id); return uuid_datum(&id);
}

PG_FUNCTION_INFO_V1(laplace_text_id);
Datum laplace_text_id(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); lp_id id;
    if (!lp_id_codepoints_utf8(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &id)) PG_RETURN_NULL();
    return uuid_datum(&id);
}

PG_FUNCTION_INFO_V1(laplace_compose);
Datum laplace_compose(PG_FUNCTION_ARGS){
    int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n), id;
    if (n == 0) PG_RETURN_NULL();
    lp_id_compose(ids, (size_t)n, &id); return uuid_datum(&id);
}

/* ---------------------------------------------------------------- physicality paths */
PG_FUNCTION_INFO_V1(laplace_path_ewkb);
Datum laplace_path_ewkb(PG_FUNCTION_ARGS){
    int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (n == 0) PG_RETURN_NULL();
    size_t need = lp_ewkb_path(ids, (size_t)n, NULL, 0);
    bytea *b = palloc(VARHDRSZ + need); SET_VARSIZE(b, VARHDRSZ + need);
    lp_ewkb_path(ids, (size_t)n, (uint8 *)VARDATA(b), need);
    PG_RETURN_BYTEA_P(b);
}

static int cmp_id(const void *a, const void *b){ return memcmp(a, b, 16); }

PG_FUNCTION_INFO_V1(laplace_vertex_ids);
Datum laplace_vertex_ids(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0));
    lp_id *ids = palloc(sizeof(lp_id) * (g.n ? g.n : 1)); int m = 0;
    for (uint32 i = 0; i < g.n; i++) lp_xyz_to_id(g.xyzm + 4 * i, &ids[i]);
    qsort(ids, g.n, 16, cmp_id);
    for (uint32 i = 0; i < g.n; i++) if (m == 0 || memcmp(&ids[i], &ids[m - 1], 16)) ids[m++] = ids[i];
    PG_RETURN_ARRAYTYPE_P(uuid_array(ids, m));
}

PG_FUNCTION_INFO_V1(laplace_follows);
Datum laplace_follows(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0)); int np; lp_id *phrase = ids_of(PG_GETARG_ARRAYTYPE_P(1), &np);
    size_t len; uint8 *e = as_ewkb(&g, &len);
    size_t cap = g.n * 4 + 16; lp_id *out = palloc(sizeof(lp_id) * cap);
    size_t k = lp_follows(e, len, phrase, (size_t)np, out, cap);
    if (k == 0) PG_RETURN_NULL();
    PG_RETURN_ARRAYTYPE_P(uuid_array(out, (int)(k < cap ? k : cap)));
}

/* ---------------------------------------------------------------- 4D geometry on real coordinates */
PG_FUNCTION_INFO_V1(laplace_distance4d);
Datum laplace_distance4d(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1));
    if (a.type != 1 || b.type != 1) ereport(ERROR, (errmsg("laplace_distance4d: expected two POINT ZM")));
    PG_RETURN_FLOAT8(lp_distance4(a.xyzm, b.xyzm));
}

PG_FUNCTION_INFO_V1(laplace_frechet4d);
Datum laplace_frechet4d(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1));
    PG_RETURN_FLOAT8(lp_frechet4(a.xyzm, a.n, b.xyzm, b.n));
}

static lp_coord coord_of(const Geo *g){
    if (g->type != 1 || g->n != 1) ereport(ERROR, (errmsg("laplace: expected a POINT ZM")));
    lp_coord c; for (int d = 0; d < 4; d++) c.m[d] = (int64)(g->xyzm[d] * LP_FIXED_ONE); return c;
}

PG_FUNCTION_INFO_V1(laplace_hilbert4);
Datum laplace_hilbert4(PG_FUNCTION_ARGS){ Geo g = geo_of(PG_GETARG_DATUM(0)); lp_coord c = coord_of(&g); PG_RETURN_INT64((int64)lp_hilbert4(&c)); }

PG_FUNCTION_INFO_V1(laplace_inside);
Datum laplace_inside(PG_FUNCTION_ARGS){ Geo g = geo_of(PG_GETARG_DATUM(0)); lp_coord c = coord_of(&g); PG_RETURN_BOOL(lp_coord_inside(&c)); }

/* Exact 4D centroid aggregate: 128-bit sums of fixed-point coordinates, one division at the end. */
typedef struct { __int128 s[4]; int64 n; } CentroidState;

PG_FUNCTION_INFO_V1(laplace_centroid4d_step);
Datum laplace_centroid4d_step(PG_FUNCTION_ARGS){
    MemoryContext agg; if (!AggCheckCallContext(fcinfo, &agg)) ereport(ERROR, (errmsg("laplace_centroid4d_step called outside an aggregate")));
    CentroidState *st = PG_ARGISNULL(0) ? NULL : (CentroidState *)PG_GETARG_POINTER(0);
    if (!st) { st = MemoryContextAllocZero(agg, sizeof *st); }
    if (!PG_ARGISNULL(1)) { Geo g = geo_of(PG_GETARG_DATUM(1)); lp_coord c = coord_of(&g); for (int d = 0; d < 4; d++) st->s[d] += c.m[d]; st->n++; }
    PG_RETURN_POINTER(st);
}

PG_FUNCTION_INFO_V1(laplace_centroid4d_final);
Datum laplace_centroid4d_final(PG_FUNCTION_ARGS){
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    CentroidState *st = (CentroidState *)PG_GETARG_POINTER(0);
    if (st->n == 0) PG_RETURN_NULL();
    uint8 e[5 + 32]; uint32 t = 1u | 0x80000000u | 0x40000000u; e[0] = 1; memcpy(e + 1, &t, 4);
    for (int d = 0; d < 4; d++) { double v = (double)(int64)(st->s[d] / (__int128)st->n) / LP_FIXED_ONE; memcpy(e + 5 + 8 * d, &v, 8); }
    bytea *b = palloc(VARHDRSZ + sizeof e); SET_VARSIZE(b, VARHDRSZ + sizeof e); memcpy(VARDATA(b), e, sizeof e);
    PG_RETURN_BYTEA_P(b);
}

/* ---------------------------------------------------------------- the tier-0 perf-cache: coordinates computed in place */
static char *tier0_path = NULL;
static const lp_tier0_record *T0;
static const lp_tier0_record *tier0(void){
    if (!T0) {
        T0 = lp_tier0_map(tier0_path);
        if (!T0) ereport(ERROR, (errmsg("laplace: cannot map tier 0 at \"%s\" (laplace.tier0)", tier0_path)));
    }
    return T0;
}
void _PG_init(void);
void _PG_init(void){
    DefineCustomStringVariable("laplace.tier0", "Path of the tier-0 perf-cache (1,114,112 64-byte records).", NULL, &tier0_path,
                               "/repos/src/Laplace-Prototype/tier0/tier0.bin", PGC_SUSET, 0, NULL, NULL, NULL);
}

/* The coordinate of a text taken as one composition of its codepoints: the exact centroid of their tier-0 points. */
static bool text_coord(text *t, lp_coord *out){
    const unsigned char *s = (const unsigned char *)VARDATA_ANY(t); int len = VARSIZE_ANY_EXHDR(t), n = 0;
    const lp_tier0_record *T = tier0(); __int128 sum[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < len; ) {
        int l = pg_utf_mblen(s + i); pg_wchar cp = utf8_to_unicode(s + i); i += l;
        if (cp >= LP_NCP) return false;
        for (int d = 0; d < 4; d++) sum[d] += T[cp].m[d];
        n++;
    }
    if (!n) return false;
    for (int d = 0; d < 4; d++) out->m[d] = (int64)(sum[d] / (__int128)n);
    return true;
}

PG_FUNCTION_INFO_V1(laplace_text_coord_ewkb);
Datum laplace_text_coord_ewkb(PG_FUNCTION_ARGS){
    lp_coord c; if (!text_coord(PG_GETARG_TEXT_PP(0), &c)) PG_RETURN_NULL();
    double x[4]; for (int d = 0; d < 4; d++) x[d] = (double)c.m[d] / LP_FIXED_ONE;
    bytea *b = palloc(VARHDRSZ + 37); SET_VARSIZE(b, VARHDRSZ + 37); lp_ewkb_point4(x, (uint8 *)VARDATA(b), 37);
    PG_RETURN_BYTEA_P(b);
}

/* The stored Hilbert key: the Hilbert value with its top bit flipped, so bigint order is Hilbert order. */
PG_FUNCTION_INFO_V1(laplace_text_hilbert);
Datum laplace_text_hilbert(PG_FUNCTION_ARGS){
    lp_coord c; if (!text_coord(PG_GETARG_TEXT_PP(0), &c)) PG_RETURN_NULL();
    PG_RETURN_INT64((int64)(lp_hilbert4(&c) ^ 0x8000000000000000ull));
}

PG_FUNCTION_INFO_V1(laplace_cp_coord_ewkb);
Datum laplace_cp_coord_ewkb(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); const lp_tier0_record *T = tier0(); double x[4];
    if (cp < 0 || (uint32)cp >= LP_NCP) PG_RETURN_NULL();
    for (int d = 0; d < 4; d++) x[d] = (double)T[cp].m[d] / LP_FIXED_ONE;
    bytea *b = palloc(VARHDRSZ + 37); SET_VARSIZE(b, VARHDRSZ + 37); lp_ewkb_point4(x, (uint8 *)VARDATA(b), 37);
    PG_RETURN_BYTEA_P(b);
}

/* ---------------------------------------------------------------- recomposition: an entity back to its text */
/* Atoms: tier-0 IDs to codepoints, an open-addressed table built once per backend from the perf-cache. */
static uint32 *atom_slot; static const uint32 ATOM_CAP = 1u << 22;          /* holds codepoint + 1; 0 is empty */
static int64 atom_of(const uint8 *id){
    const lp_tier0_record *T = tier0();
    if (!atom_slot) {
        atom_slot = MemoryContextAllocZero(TopMemoryContext, sizeof(uint32) * ATOM_CAP);
        for (uint32 cp = 0; cp < LP_NCP; cp++) {
            uint64 h; memcpy(&h, T[cp].id.b, 8); uint32 s = (uint32)(h & (ATOM_CAP - 1));
            while (atom_slot[s]) s = (s + 1) & (ATOM_CAP - 1);
            atom_slot[s] = cp + 1;
        }
    }
    uint64 h; memcpy(&h, id, 8); uint32 s = (uint32)(h & (ATOM_CAP - 1));
    while (atom_slot[s]) { if (!memcmp(T[atom_slot[s] - 1].id.b, id, 16)) return atom_slot[s] - 1; s = (s + 1) & (ATOM_CAP - 1); }
    return -1;
}
static SPIPlanPtr path_plan;
static void recompose(const uint8 *id, StringInfo out, int depth){
    int64 cp = atom_of(id);
    if (cp >= 0) { unsigned char u[4]; unicode_to_utf8((pg_wchar)cp, u); appendBinaryStringInfo(out, (const char *)u, pg_utf_mblen(u)); return; }
    if (depth > 64) ereport(ERROR, (errmsg("laplace_text: composition deeper than 64 tiers")));
    Datum arg = UUIDPGetDatum((pg_uuid_t *)id);
    if (SPI_execute_plan(path_plan, &arg, NULL, true, 1) != SPI_OK_SELECT || SPI_processed == 0)
        ereport(ERROR, (errmsg("laplace_text: no physicality for an entity")));
    bool isnull; Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    bytea *e = DatumGetByteaPCopy(d); size_t len = VARSIZE_ANY_EXHDR(e); const uint8_t *v; size_t nv = lp_ewkb_vertices((const uint8_t *)VARDATA_ANY(e), len, &v);
    for (size_t i = 0; i < nv; i++) {
        double m; memcpy(&m, v + 32 * i + 24, 8); lp_id cid; lp_xyz_to_id((const double *)(v + 32 * i), &cid);
        for (int r = 0; r < (m < 1 ? 1 : (int)m); r++) recompose(cid.b, out, depth + 1);
    }
}

PG_FUNCTION_INFO_V1(laplace_text);
Datum laplace_text(PG_FUNCTION_ARGS){
    pg_uuid_t *u = PG_GETARG_UUID_P(0); StringInfoData out; initStringInfo(&out);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_text: SPI")));
    if (!path_plan) { Oid t = UUIDOID; path_plan = SPI_prepare("SELECT st_asewkb(path) FROM physicality WHERE entity = $1 LIMIT 1", 1, &t); SPI_keepplan(path_plan); }
    recompose(u->data, &out, 0);
    SPI_finish();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}

/* ---------------------------------------------------------------- observability */
PG_FUNCTION_INFO_V1(laplace_isa);
Datum laplace_isa(PG_FUNCTION_ARGS){
    char buf[256];
    snprintf(buf, sizeof buf, "cpu: %s; dispatch: %s", lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()));
    PG_RETURN_TEXT_P(cstring_to_text(buf));
}
