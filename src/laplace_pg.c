/* Laplace-postgres: Laplace's 4D expansion of PostgreSQL and PostGIS.
 *
 * Standard geometry types stay as they are; these functions add what Laplace needs next to what PostGIS provides.
 * They read PostGIS's serialized geometry directly (a LINESTRING ZM's vertices are 32-byte X, Y, Z, M blocks, the same
 * layout the Laplace-Native kernels use), and call Laplace-Native for all math.
 *
 * An ID is a BLAKE3 hash, 128 bits. It has a type of its own, blake3: 16 fixed bytes, written as 32 hexadecimal
 * digits, ordered and compared as bytes. */
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "libpq/pqformat.h"
#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "varatt.h"
#include "utils/guc.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "lib/stringinfo.h"
#include "laplace/laplace.h"

PG_MODULE_MAGIC_EXT(.name = "laplace", .version = "1.0");

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

static Datum id_datum(const lp_id *id){ uint8 *u = palloc(16); memcpy(u, id->b, 16); return PointerGetDatum(u); }
/* An array of IDs, of the type the function is declared to return. */
static ArrayType *id_array(FunctionCallInfo fcinfo, const lp_id *ids, int n){
    Oid el = get_element_type(get_fn_expr_rettype(fcinfo->flinfo));
    if (!OidIsValid(el)) ereport(ERROR, (errmsg("laplace: the function does not return an array of IDs")));
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1));
    for (int i = 0; i < n; i++) d[i] = id_datum(&ids[i]);
    return construct_array(d, n, el, 16, false, TYPALIGN_CHAR);
}
/* An array of IDs of a given element type: a slice of a prompt's constituents, as the argument's type. */
static ArrayType *id_array_of(Oid el, const lp_id *ids, int n){
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1));
    for (int i = 0; i < n; i++) d[i] = id_datum(&ids[i]);
    return construct_array(d, n, el, 16, false, TYPALIGN_CHAR);
}
static lp_id *ids_of(ArrayType *a, int *n){
    Datum *d; bool *nulls; deconstruct_array(a, ARR_ELEMTYPE(a), 16, false, TYPALIGN_CHAR, &d, &nulls, n);
    lp_id *ids = palloc(sizeof(lp_id) * (*n ? *n : 1));
    for (int i = 0; i < *n; i++) {
        if (nulls[i]) ereport(ERROR, (errmsg("laplace: IDs cannot be null")));
        memcpy(ids[i].b, DatumGetPointer(d[i]), 16);
    }
    return ids;
}

/* ---------------------------------------------------------------- the type of an ID: blake3 */
static int hexval(char c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
PG_FUNCTION_INFO_V1(blake3_in);
Datum blake3_in(PG_FUNCTION_ARGS){
    const char *t = PG_GETARG_CSTRING(0); uint8 *u = palloc(16);
    if (strlen(t) != 32) ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION), errmsg("a blake3 ID is 32 hexadecimal digits: \"%s\"", t)));
    for (int i = 0; i < 16; i++) { int h = hexval(t[2 * i]), l = hexval(t[2 * i + 1]);
        if (h < 0 || l < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION), errmsg("a blake3 ID is 32 hexadecimal digits: \"%s\"", t)));
        u[i] = (uint8)(h << 4 | l); }
    PG_RETURN_POINTER(u);
}
PG_FUNCTION_INFO_V1(blake3_out);
Datum blake3_out(PG_FUNCTION_ARGS){
    const uint8 *u = (const uint8 *)PG_GETARG_POINTER(0); char *t = palloc(33); static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { t[2 * i] = hex[u[i] >> 4]; t[2 * i + 1] = hex[u[i] & 15]; } t[32] = 0;
    PG_RETURN_CSTRING(t);
}
PG_FUNCTION_INFO_V1(blake3_recv);
Datum blake3_recv(PG_FUNCTION_ARGS){ StringInfo b = (StringInfo)PG_GETARG_POINTER(0); uint8 *u = palloc(16); memcpy(u, pq_getmsgbytes(b, 16), 16); PG_RETURN_POINTER(u); }
PG_FUNCTION_INFO_V1(blake3_send);
Datum blake3_send(PG_FUNCTION_ARGS){ StringInfoData b; pq_begintypsend(&b); pq_sendbytes(&b, PG_GETARG_POINTER(0), 16); PG_RETURN_BYTEA_P(pq_endtypsend(&b)); }
#define ID_CMP(name, test) PG_FUNCTION_INFO_V1(name); Datum name(PG_FUNCTION_ARGS){ int c = memcmp(PG_GETARG_POINTER(0), PG_GETARG_POINTER(1), 16); PG_RETURN_BOOL(test); }
ID_CMP(blake3_eq, c == 0) ID_CMP(blake3_ne, c != 0) ID_CMP(blake3_lt, c < 0) ID_CMP(blake3_le, c <= 0) ID_CMP(blake3_gt, c > 0) ID_CMP(blake3_ge, c >= 0)
PG_FUNCTION_INFO_V1(blake3_cmp);
Datum blake3_cmp(PG_FUNCTION_ARGS){ int c = memcmp(PG_GETARG_POINTER(0), PG_GETARG_POINTER(1), 16); PG_RETURN_INT32(c < 0 ? -1 : c > 0); }
/* A hash is already evenly spread: its own bytes are the hash a hash index or a hash join asks for. */
PG_FUNCTION_INFO_V1(blake3_hash);
Datum blake3_hash(PG_FUNCTION_ARGS){ uint32 h; memcpy(&h, PG_GETARG_POINTER(0), 4); PG_RETURN_UINT32(h); }
PG_FUNCTION_INFO_V1(blake3_hash_extended);
Datum blake3_hash_extended(PG_FUNCTION_ARGS){ return hash_any_extended((const unsigned char *)PG_GETARG_POINTER(0), 16, PG_GETARG_INT64(1)); }


/* ---------------------------------------------------------------- identity */
PG_FUNCTION_INFO_V1(laplace_cp_id);
Datum laplace_cp_id(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); lp_id id;
    if (cp < 0 || (uint32)cp >= LP_NCP) ereport(ERROR, (errmsg("laplace_cp_id: %d is outside the codespace", cp)));
    lp_id_codepoint((uint32)cp, &id); return id_datum(&id);
}

PG_FUNCTION_INFO_V1(laplace_text_id);
Datum laplace_text_id(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); lp_id id;
    if (!lp_id_codepoints_utf8(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &id)) PG_RETURN_NULL();
    return id_datum(&id);
}

PG_FUNCTION_INFO_V1(laplace_compose);
Datum laplace_compose(PG_FUNCTION_ARGS){
    int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n), id;
    if (n == 0) PG_RETURN_NULL();
    lp_id_compose(ids, (size_t)n, &id); return id_datum(&id);
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
    PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, ids, m));
}

PG_FUNCTION_INFO_V1(laplace_follows);
Datum laplace_follows(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0)); int np; lp_id *phrase = ids_of(PG_GETARG_ARRAYTYPE_P(1), &np);
    size_t len; uint8 *e = as_ewkb(&g, &len);
    size_t cap = g.n * 4 + 16; lp_id *out = palloc(sizeof(lp_id) * cap);
    size_t k = lp_follows(e, len, phrase, (size_t)np, out, cap);
    if (k == 0) PG_RETURN_NULL();
    PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, out, (int)(k < cap ? k : cap)));
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
static char *flags_path = NULL;
static char *highway_path = NULL;
static const lp_highway *HW;
static const lp_highway *highway(void){
    if (!HW) {
        HW = lp_highway_map(highway_path);
        if (!HW) ereport(ERROR, (errmsg("laplace: cannot map the highway at \"%s\" (laplace.highway; generate it with: laplace highway)", highway_path)));
    }
    return HW;
}
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
                               lp_tier0_path(), PGC_SUSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("laplace.flags", "Path of the flags that go with tier 0 (1,114,112 256-bit records; their layout beside them).", NULL, &flags_path,
                               lp_flags_path(), PGC_SUSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("laplace.highway", "Path of the highway perf-cache (the types the resources list, and the mappings between them; its layout beside it).", NULL, &highway_path,
                               lp_highway_path(), PGC_SUSET, 0, NULL, NULL, NULL);
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

/* ---------------------------------------------------------------- the flags that go with tier 0
 * What the Unicode Standard says of a codepoint, from the memory-mapped flags: by the property's name and the value's
 * name as the standard writes them, short or as they are said. Nothing is read from a table. */
static const lp_layout *FL;
static const lp_layout *flags(void){
    if (!FL) { FL = lp_flags_map(flags_path); if (!FL) ereport(ERROR, (errmsg("laplace: cannot map the flags at \"%s\" (laplace.flags)", flags_path ? flags_path : lp_flags_path()))); }
    return FL;
}
static const lp_field *field_of(text *t){
    char *n = text_to_cstring(t); const lp_field *f = lp_flags_field(flags(), n);
    if (!f) ereport(ERROR, (errmsg("laplace: the standard lists no property \"%s\"", n)));
    return f;
}
static int64 cp_of_id(const uint8 *u){ return lp_tier0_codepoint(tier0(), (const lp_id *)u); }
static Datum said(int64 cp, text *property){
    if (cp < 0 || cp >= LP_NCP) return (Datum)0;
    const lp_layout *l = flags(); const lp_field *f = field_of(property); uint32 v = lp_flags_get(l, (uint32)cp, f);
    return PointerGetDatum(cstring_to_text(f->nvalues ? l->value[f->first + v].say : v ? "Yes" : "No"));
}
static int is(int64 cp, text *property, text *value){
    if (cp < 0 || cp >= LP_NCP) return -1;
    const lp_layout *l = flags(); const lp_field *f = field_of(property); char *vn = text_to_cstring(value); int32 want = lp_flags_value(l, f, vn);
    if (want < 0) ereport(ERROR, (errmsg("laplace: the standard lists no value \"%s\" for %s", vn, f->say)));
    return lp_flags_get(l, (uint32)cp, f) == (uint32)want;
}
PG_FUNCTION_INFO_V1(laplace_cp_flags);
Datum laplace_cp_flags(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); if (cp < 0 || (uint32)cp >= LP_NCP) PG_RETURN_NULL();
    bytea *b = palloc(VARHDRSZ + 32); SET_VARSIZE(b, VARHDRSZ + 32); memcpy(VARDATA(b), flags()->flags[cp].b, 32); PG_RETURN_BYTEA_P(b);
}
PG_FUNCTION_INFO_V1(laplace_cp_said);
Datum laplace_cp_said(PG_FUNCTION_ARGS){ Datum d = said(PG_GETARG_INT32(0), PG_GETARG_TEXT_PP(1)); if (!d) PG_RETURN_NULL(); return d; }
PG_FUNCTION_INFO_V1(laplace_said);
Datum laplace_said(PG_FUNCTION_ARGS){ Datum d = said(cp_of_id((const uint8 *)PG_GETARG_POINTER(0)), PG_GETARG_TEXT_PP(1)); if (!d) PG_RETURN_NULL(); return d; }
PG_FUNCTION_INFO_V1(laplace_cp_is);
Datum laplace_cp_is(PG_FUNCTION_ARGS){ int r = is(PG_GETARG_INT32(0), PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)); if (r < 0) PG_RETURN_NULL(); PG_RETURN_BOOL(r); }
PG_FUNCTION_INFO_V1(laplace_is);
Datum laplace_is(PG_FUNCTION_ARGS){ int r = is(cp_of_id((const uint8 *)PG_GETARG_POINTER(0)), PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)); if (r < 0) PG_RETURN_NULL(); PG_RETURN_BOOL(r); }
PG_FUNCTION_INFO_V1(laplace_codepoint);
Datum laplace_codepoint(PG_FUNCTION_ARGS){ int64 cp = cp_of_id((const uint8 *)PG_GETARG_POINTER(0)); if (cp < 0) PG_RETURN_NULL(); PG_RETURN_INT32((int32)cp); }

/* ---------------------------------------------------------------- a text's entity, computed in place
 * The one decomposition of text (UAX #29, Laplace-Native), composed without recording: the ID, tier, coordinate and
 * constituents the engine gives the same text. A constant argument folds at plan time, so the query becomes an index
 * lookup on the result. */
static lp_text *TX;
static lp_ref trunk_of(text *t, lp_ref *parts, size_t cap, size_t *np){
    if (!TX) { TX = lp_text_new(tier0()); if (!TX) ereport(ERROR, (errmsg("laplace: cannot open ICU's break iterators"))); }
    lp_ref one; size_t n;
    return lp_text_parts(TX, (const uint8_t *)VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), parts ? parts : &one, parts ? cap : 1, np ? np : &n);
}

PG_FUNCTION_INFO_V1(laplace_id);
Datum laplace_id(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_NULL();
    lp_ref r = trunk_of(t, NULL, 0, NULL); return id_datum(&r.id);
}

PG_FUNCTION_INFO_V1(laplace_tier);
Datum laplace_tier(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_NULL();
    PG_RETURN_INT16((int16)trunk_of(t, NULL, 0, NULL).tier);
}

PG_FUNCTION_INFO_V1(laplace_coord_ewkb);
Datum laplace_coord_ewkb(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_NULL();
    lp_ref r = trunk_of(t, NULL, 0, NULL);
    double x[4]; for (int d = 0; d < 4; d++) x[d] = (double)r.c.m[d] / LP_FIXED_ONE;
    bytea *b = palloc(VARHDRSZ + 37); SET_VARSIZE(b, VARHDRSZ + 37); lp_ewkb_point4(x, (uint8 *)VARDATA(b), 37);
    PG_RETURN_BYTEA_P(b);
}

/* The stored Hilbert key of a text's entity. */
PG_FUNCTION_INFO_V1(laplace_hilbert);
Datum laplace_hilbert(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_NULL();
    lp_ref r = trunk_of(t, NULL, 0, NULL);
    PG_RETURN_INT64((int64)(lp_hilbert4(&r.c) ^ 0x8000000000000000ull));
}

/* The constituents of a text's entity, in order, repeats included: the phrase to look for inside paths. */
PG_FUNCTION_INFO_V1(laplace_parts);
Datum laplace_parts(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_NULL();
    size_t cap = VARSIZE_ANY_EXHDR(t) + 1, n; lp_ref *parts = palloc(sizeof(lp_ref) * cap);
    trunk_of(t, parts, cap, &n); if (n > cap) n = cap;
    lp_id *ids = palloc(sizeof(lp_id) * n); for (size_t i = 0; i < n; i++) ids[i] = parts[i].id;
    PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, ids, (int)n));
}

/* Which tier 0 this database computes with. */
PG_FUNCTION_INFO_V1(laplace_fingerprint);
Datum laplace_fingerprint(PG_FUNCTION_ARGS){
    uint8 h[32]; char hex[65]; lp_tier0_fingerprint(tier0(), h);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    PG_RETURN_TEXT_P(cstring_to_text(hex));
}

/* ---------------------------------------------------------------- shape measures on real coordinates */
PG_FUNCTION_INFO_V1(laplace_frechet4d_outliers);
Datum laplace_frechet4d_outliers(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1)); int32 k = PG_GETARG_INT32(2);
    if (k < 0 || k > 8) ereport(ERROR, (errmsg("laplace_frechet4d: between 0 and 8 vertices can be skipped")));
    PG_RETURN_FLOAT8(lp_frechet4_outliers(a.xyzm, a.n, b.xyzm, b.n, (unsigned)k));
}
PG_FUNCTION_INFO_V1(laplace_dtw4d);
Datum laplace_dtw4d(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1));
    PG_RETURN_FLOAT8(lp_dtw4(a.xyzm, a.n, b.xyzm, b.n, NULL));
}
PG_FUNCTION_INFO_V1(laplace_edr4d);
Datum laplace_edr4d(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1));
    PG_RETURN_INT64((int64)lp_edr4(a.xyzm, a.n, b.xyzm, b.n, PG_GETARG_FLOAT8(2)));
}

/* How hard a strand tugs back, from a standing. */
PG_FUNCTION_INFO_V1(laplace_confidence);
Datum laplace_confidence(PG_FUNCTION_ARGS){
    lp_rating r = { PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), 0.06 };
    PG_RETURN_FLOAT8(lp_confidence(&r, PG_GETARG_FLOAT8(2)));
}

/* ---------------------------------------------------------------- recomposition: an entity back to its text */
static int64 atom_of(const uint8 *id){ return lp_tier0_codepoint(tier0(), (const lp_id *)id); }
/* The composition is read one DAG level per statement: every entity a level names that is not an atom and not yet
 * read is fetched in one set, from the trunk down, and the text is then written out of memory in path order. */
typedef struct { lp_id id; uint32 *kid; uint32 nkid; uint8 have; } TNode;     /* kid: indexes into the node table, one per occurrence */
typedef struct { TNode *n; uint32 cnt, cap; uint32 *slot; uint32 nslot; MemoryContext ctx; } Tree;
static uint32 tnode(Tree *t, const lp_id *id){
    uint64 h; memcpy(&h, id->b, 8); uint32 k = (uint32)(h & (t->nslot - 1));
    while (t->slot[k] != UINT32_MAX) { if (!memcmp(&t->n[t->slot[k]].id, id, 16)) return t->slot[k]; k = (k + 1) & (t->nslot - 1); }
    if (t->cnt == t->cap) { t->cap *= 2; t->n = repalloc(t->n, sizeof(TNode) * t->cap); }
    if (t->cnt * 2 >= t->nslot) {                                                 /* grow the slots, rehash */
        t->nslot *= 2; t->slot = repalloc(t->slot, sizeof(uint32) * t->nslot); memset(t->slot, 0xff, sizeof(uint32) * t->nslot);
        for (uint32 i = 0; i < t->cnt; i++) { uint64 g; memcpy(&g, t->n[i].id.b, 8); uint32 q = (uint32)(g & (t->nslot - 1)); while (t->slot[q] != UINT32_MAX) q = (q + 1) & (t->nslot - 1); t->slot[q] = i; }
        k = (uint32)(h & (t->nslot - 1)); while (t->slot[k] != UINT32_MAX) k = (k + 1) & (t->nslot - 1);
    }
    TNode *x = &t->n[t->cnt]; x->id = *id; x->kid = NULL; x->nkid = 0; x->have = lp_tier0_codepoint(tier0(), id) >= 0;
    t->slot[k] = t->cnt; return t->cnt++;
}
static SPIPlanPtr level_plan;
static void read_levels(Tree *t, Oid idarr){
    for (int depth = 0; ; depth++) {
        if (depth > 64) ereport(ERROR, (errmsg("laplace_text: composition deeper than 64 tiers")));
        uint32 want = 0; for (uint32 i = 0; i < t->cnt; i++) want += !t->n[i].have;
        if (!want) return;
        Datum *ids = palloc(sizeof(Datum) * want); uint32 w = 0;
        for (uint32 i = 0; i < t->cnt; i++) if (!t->n[i].have) ids[w++] = PointerGetDatum(t->n[i].id.b);
        ArrayType *arr = construct_array(ids, (int)want, get_element_type(idarr), 16, false, TYPALIGN_CHAR);
        Datum arg = PointerGetDatum(arr);
        if (!level_plan) { level_plan = SPI_prepare("SELECT entity, st_asewkb(path) FROM physicality WHERE entity = ANY($1)", 1, &idarr); SPI_keepplan(level_plan); }
        if (SPI_execute_plan(level_plan, &arg, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_text: %s", SPI_result_code_string(SPI_result))));
        if (SPI_processed < want) ereport(ERROR, (errmsg("laplace_text: no physicality for an entity")));
        for (uint64 j = 0; j < SPI_processed; j++) {
            bool nl; lp_id id; memcpy(&id, DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 1, &nl)), 16);
            bytea *e = DatumGetByteaP(SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 2, &nl));
            const uint8_t *v; size_t nv = lp_ewkb_vertices((const uint8_t *)VARDATA_ANY(e), VARSIZE_ANY_EXHDR(e), &v);
            uint32 n = 0; for (size_t i = 0; i < nv; i++) { double m; memcpy(&m, v + 32 * i + 24, 8); n += lp_m_run(m); }
            uint32 *kid = MemoryContextAlloc(t->ctx, sizeof(uint32) * (n ? n : 1)), k = 0;
            for (size_t i = 0; i < nv; i++) {
                double m; memcpy(&m, v + 32 * i + 24, 8); lp_id cid; lp_xyz_to_id((const double *)(v + 32 * i), &cid);
                uint32 c = tnode(t, &cid); for (uint32 r = 0; r < lp_m_run(m); r++) kid[k++] = c;
            }
            uint32 me = tnode(t, &id); t->n[me].kid = kid; t->n[me].nkid = n; t->n[me].have = 1;
        }
        SPI_freetuptable(SPI_tuptable); pfree(ids);
    }
}
static void render(const Tree *t, uint32 i, StringInfo out){
    const TNode *x = &t->n[i]; int64 cp = x->nkid ? -1 : lp_tier0_codepoint(tier0(), &x->id);
    if (cp >= 0) { unsigned char u[4]; unicode_to_utf8((pg_wchar)cp, u); appendBinaryStringInfo(out, (const char *)u, pg_utf_mblen(u)); return; }
    for (uint32 k = 0; k < x->nkid; k++) render(t, x->kid[k], out);
}
PG_FUNCTION_INFO_V1(laplace_text);
Datum laplace_text(PG_FUNCTION_ARGS){
    const uint8 *u = (const uint8 *)PG_GETARG_POINTER(0); StringInfoData out; initStringInfo(&out);
    Tree t; t.ctx = CurrentMemoryContext; t.cap = 256; t.cnt = 0; t.n = palloc(sizeof(TNode) * t.cap); t.nslot = 1024; t.slot = palloc(sizeof(uint32) * t.nslot); memset(t.slot, 0xff, sizeof(uint32) * t.nslot);
    uint32 root = tnode(&t, (const lp_id *)u);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_text: SPI")));
    read_levels(&t, get_array_type(get_fn_expr_argtype(fcinfo->flinfo, 0)));
    SPI_finish();
    render(&t, root, &out);
    PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}

/* ---------------------------------------------------------------- the web: what tugs back when a strand is pulled
 * A claim is content like any other, hashed over its parts. A tier is a floor: what holds an entity sits above it, at
 * whatever tier its own recipe composed it, so the claims that hold an entity are found by the ID they hold, among the
 * paths above the entity's tier (only the tiers at and below are pruned), that the consensus knows. Each statement is planned once per backend and the plan kept; every call is one executor run over
 * a set, returned through a tuplestore. */
#include "funcapi.h"
typedef struct { const char *name; const char *sql; int nargs; SPIPlanPtr plan; } Kept;
static Tuplestorestate *set_begin(FunctionCallInfo fcinfo, const char *name, TupleDesc *td){
    ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
    if (!rsi || !IsA(rsi, ReturnSetInfo) || !(rsi->allowedModes & SFRM_Materialize)) ereport(ERROR, (errmsg("%s: set-valued function called in a context that cannot accept a set", name)));
    if (get_call_result_type(fcinfo, NULL, td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("%s: composite result expected", name)));
    MemoryContext old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    Tuplestorestate *ts = tuplestore_begin_heap(true, false, 65536); *td = CreateTupleDescCopy(*td); MemoryContextSwitchTo(old);
    rsi->returnMode = SFRM_Materialize; rsi->setResult = ts; rsi->setDesc = *td;
    return ts;
}
/* Run a kept statement over the call's arguments and hand every row through. */
static void set_run(FunctionCallInfo fcinfo, Kept *k, Tuplestorestate *ts, TupleDesc td){
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("%s: SPI_connect", k->name)));
    if (!k->plan) {
        Oid t[4]; for (int i = 0; i < k->nargs; i++) t[i] = get_fn_expr_argtype(fcinfo->flinfo, i);
        k->plan = SPI_prepare(k->sql, k->nargs, t);
        if (!k->plan) ereport(ERROR, (errmsg("%s: SPI_prepare: %s", k->name, SPI_result_code_string(SPI_result))));
        SPI_keepplan(k->plan);
    }
    Datum a[4]; for (int i = 0; i < k->nargs; i++) a[i] = PG_GETARG_DATUM(i);
    if (SPI_execute_plan(k->plan, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("%s: %s", k->name, SPI_result_code_string(SPI_result))));
    int nc = SPI_tuptable->tupdesc->natts;
    for (uint64 j = 0; j < SPI_processed; j++) {
        Datum v[8]; bool nl[8];
        for (int c = 0; c < nc; c++) v[c] = SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, c + 1, &nl[c]);
        tuplestore_putvalues(ts, td, v, nl);
    }
    SPI_finish();
}
/* laplace_claims(parts, fan): the claims holding every one of the parts, with the consensus on each: at most fan of
 * them. A claim sits above its highest part, at no fixed distance: every tier above is read, by the IDs held. */
static Kept claims_kept = { "laplace_claims",
    "SELECT p.entity, p.path, s.rating, s.deviation, s.volatility, s.matches FROM physicality p JOIN consensus s ON s.claim = p.entity "
    "WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path @> $1 AND p.mask ?& $3 LIMIT $2", 3, NULL };
PG_FUNCTION_INFO_V1(laplace_claims);
Datum laplace_claims(PG_FUNCTION_ARGS){ TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_claims", &td); set_run(fcinfo, &claims_kept, ts, td); PG_RETURN_NULL(); }
/* laplace_claims_each(ids, fan): for each of a set of entities, the claims holding it: one call for a whole level of
 * a walk. i is the entity's place in the set. The entity is a rare key (an observation, a claim), so the path alone
 * is the index condition and the bits are a check on the rows found: the kind bit's posting list is every claim. */
static Kept each_kept = { "laplace_claims_each",
    "SELECT u.i, c.entity, c.path, c.rating, c.deviation, c.volatility, c.matches FROM unnest($1) WITH ORDINALITY AS u(id, i) JOIN entity e ON e.id = u.id "
    "CROSS JOIN LATERAL (SELECT p.entity, p.path, s.rating, s.deviation, s.volatility, s.matches FROM physicality p JOIN consensus s ON s.claim = p.entity "
    "WHERE p.tier > e.tier AND p.path @> ARRAY[u.id] AND laplace_mask_has_all(p.mask, $3) LIMIT $2) c", 3, NULL };
PG_FUNCTION_INFO_V1(laplace_claims_each);
Datum laplace_claims_each(PG_FUNCTION_ARGS){ TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_claims_each", &td); set_run(fcinfo, &each_kept, ts, td); PG_RETURN_NULL(); }
/* laplace_containers(parts): every path that holds all of the parts, claims and observations alike. A path holds its
 * direct constituents, so what holds the parts sits above the highest of them, at no fixed distance: every tier above
 * is read, by the IDs held. */
static Kept containers_kept = { "laplace_containers",
    "SELECT p.entity, p.path, p.tier, p.mask FROM physicality p "
    "WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path @> $1 AND p.mask ?& $2", 2, NULL };
/* With no bits asked for, the mask takes no part: "any of no bits" would send the index over every row. */
static Kept containers_any_kept = { "laplace_containers",
    "SELECT p.entity, p.path, p.tier, p.mask FROM physicality p "
    "WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path @> $1", 1, NULL };
PG_FUNCTION_INFO_V1(laplace_containers);
Datum laplace_containers(PG_FUNCTION_ARGS){ TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_containers", &td);
    ArrayType *bits = PG_GETARG_ARRAYTYPE_P(1); set_run(fcinfo, ArrayGetNItems(ARR_NDIM(bits), ARR_DIMS(bits)) ? &containers_kept : &containers_any_kept, ts, td); PG_RETURN_NULL(); }
/* laplace_forward(ids, fan): the forward pass over every contiguous segment of a prompt at once. For each segment
 * [i..j] of the prompt's constituents: the observations holding all of its parts (at most fan of them, claims left
 * out), how many hold it as a run, and what follows the run in each, counted. One row per continuation (next, times);
 * a segment held by nothing, or held where nothing follows the run, has a row with next null. SQL only fetches the
 * paths that hold a segment, through the container index (Architecture: SQL fetches and writes records); the runs are
 * matched and what follows them is counted here, by Laplace-Native: precedes, contains and co-occurrence from the
 * trajectories, no softmax, no window. */
static Kept forward_kept = { "laplace_forward",
    "SELECT p.path FROM physicality p WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path @> $1 "
    "AND NOT laplace_mask_has(p.mask, 0::smallint) LIMIT $2", 2, NULL };
typedef struct { lp_id id; int64 times; } Followed;
PG_FUNCTION_INFO_V1(laplace_forward);
Datum laplace_forward(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_forward", &td);
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0); Oid el = ARR_ELEMTYPE(arr); int n; lp_id *ids = ids_of(arr, &n); int64 fan = PG_GETARG_INT64(1); if (n > 256) n = 256;
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_forward: SPI_connect")));
    Kept *k = &forward_kept;
    if (!k->plan) { Oid t[2] = { get_fn_expr_argtype(fcinfo->flinfo, 0), INT8OID }; k->plan = SPI_prepare(k->sql, 2, t);
        if (!k->plan) ereport(ERROR, (errmsg("laplace_forward: SPI_prepare: %s", SPI_result_code_string(SPI_result)))); SPI_keepplan(k->plan); }
    /* which constituents are compositions (words and above): a segment of atoms alone (a space, a letter) is a hub and
     * says nothing of the prompt, and a single constituent is the constituents' own step, not a segment */
    uint8 *comp = palloc0((size_t)n);
    { static Kept tiers_kept = { "laplace_forward tiers", "SELECT u.i FROM unnest($1) WITH ORDINALITY u(id, i) JOIN entity e ON e.id = u.id WHERE e.tier > 0", 1, NULL };
      if (!tiers_kept.plan) { Oid t[1] = { get_fn_expr_argtype(fcinfo->flinfo, 0) }; tiers_kept.plan = SPI_prepare(tiers_kept.sql, 1, t); if (!tiers_kept.plan) ereport(ERROR, (errmsg("laplace_forward: SPI_prepare tiers"))); SPI_keepplan(tiers_kept.plan); }
      Datum a0[1] = { PointerGetDatum(arr) }; if (SPI_execute_plan(tiers_kept.plan, a0, NULL, true, 0) == SPI_OK_SELECT)
          for (uint64 r = 0; r < SPI_processed; r++) { bool nl; int64 i = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)); if (!nl && i >= 1 && i <= n) comp[i - 1] = 1; } }
    /* what a segment's paths are read into is given back after the segment: a prompt has a segment for every pair of its constituents */
    MemoryContext seg = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward segment", ALLOCSET_DEFAULT_SIZES);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        int words = 0; for (int k = i; k <= j; k++) words += comp[k]; if (!words) continue;
        MemoryContext old = MemoryContextSwitchTo(seg); int np = j - i + 1;
        Datum a[2] = { PointerGetDatum(id_array_of(el, ids + i, np)), Int64GetDatum(fan) };
        if (SPI_execute_plan(k->plan, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_forward: %s", SPI_result_code_string(SPI_result))));
        SPITupleTable *tt = SPI_tuptable; uint64 paths = SPI_processed; int64 runs = 0; bool unfollowed = paths == 0;
        /* what follows the run in each path, counted by ID: open addressing over the distinct continuations */
        size_t nf = 0, cf = 64, cs = 256; Followed *fl = palloc(sizeof(Followed) * cf); uint32 *slot = palloc0(sizeof(uint32) * cs);
        for (uint64 r = 0; r < paths; r++) {
            bool isnull; Datum d = SPI_getbinval(tt->vals[r], tt->tupdesc, 1, &isnull); if (isnull) { unfollowed = true; continue; }
            Geo g = geo_of(d); size_t len; uint8 *e = as_ewkb(&g, &len);
            size_t cap = (size_t)g.n * 4 + 16; lp_id *out = palloc(sizeof(lp_id) * cap);
            size_t found = lp_follows(e, len, ids + i, (size_t)np, out, cap);
            if (!found) { unfollowed = true; continue; }                     /* it holds the parts, and not as a run that anything follows */
            runs++; if (found > cap) found = cap;
            for (size_t x = 0; x < found; x++) {
                if ((nf + 1) * 2 > cs) { cs *= 2; slot = palloc0(sizeof(uint32) * cs);
                    for (size_t y = 0; y < nf; y++) { uint64 h; memcpy(&h, fl[y].id.b, 8); size_t s = h & (cs - 1); while (slot[s]) s = (s + 1) & (cs - 1); slot[s] = (uint32)y + 1; } }
                uint64 h; memcpy(&h, out[x].b, 8); size_t s = h & (cs - 1);
                while (slot[s] && memcmp(&fl[slot[s] - 1].id, &out[x], 16)) s = (s + 1) & (cs - 1);
                if (!slot[s]) { if (nf == cf) { cf *= 2; fl = repalloc(fl, sizeof(Followed) * cf); } fl[nf].id = out[x]; fl[nf].times = 0; slot[s] = (uint32)++nf; }
                fl[slot[s] - 1].times++;
            }
        }
        Datum v[6]; bool nl[6] = { false, false, false, false, false, false };
        v[0] = Int32GetDatum(i + 1); v[1] = Int32GetDatum(j + 1); v[2] = Int64GetDatum((int64)paths); v[3] = Int64GetDatum(runs);
        for (size_t x = 0; x < nf; x++) { v[4] = id_datum(&fl[x].id); v[5] = Int64GetDatum(fl[x].times); tuplestore_putvalues(ts, td, v, nl); }
        if (unfollowed) { v[4] = (Datum)0; nl[4] = true; v[5] = Int64GetDatum(0); tuplestore_putvalues(ts, td, v, nl); }
        SPI_freetuptable(tt); MemoryContextSwitchTo(old); MemoryContextReset(seg);
    }
    MemoryContextDelete(seg);
    SPI_finish(); PG_RETURN_NULL();
}
/* laplace_fills(keys): every path above the lowest key that holds any of them, with the times each key is followed by the
 * next vertex of that path: what fills the gap after it. */
static Kept fills_kept = { "laplace_fills",
    "SELECT p.entity, t.id, t.times, p.tier FROM physicality p, laplace_path_times(p.path, $1) t "
    "WHERE p.tier > (SELECT min(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path && $1", 1, NULL };
PG_FUNCTION_INFO_V1(laplace_fills);
Datum laplace_fills(PG_FUNCTION_ARGS){ TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_fills", &td); set_run(fcinfo, &fills_kept, ts, td); PG_RETURN_NULL(); }
/* laplace_paths(ids): the paths of a set of entities: one DAG level per call. */
static Kept paths_kept = { "laplace_paths", "SELECT entity, path FROM physicality WHERE entity = ANY($1)", 1, NULL };
PG_FUNCTION_INFO_V1(laplace_paths);
Datum laplace_paths(PG_FUNCTION_ARGS){ TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_paths", &td); set_run(fcinfo, &paths_kept, ts, td); PG_RETURN_NULL(); }
/* laplace_attested(claims): who attested each of a set of claims, with the position given and the witness's trust. */
/* A claim witnessed on its own is a ledger row; one witnessed within a record is a member of the record's path, and the
 * record is the ledger row: both are found, the record's rows through the path index. The records that hold a claim
 * are found by the ID they hold; that they are records is checked on the few rows found, never searched for by itself
 * (every record carries that bit). */
static Kept attested_kept = { "laplace_attested",
    "SELECT u.id, a.witness, a.position, w.trust FROM unnest($1) u(id) JOIN attestation a ON a.claim = u.id JOIN witness w ON w.id = a.witness "
    "UNION ALL SELECT u.id, a.witness, a.position, w.trust FROM unnest($1) u(id) "
    "CROSS JOIN LATERAL (SELECT p.entity FROM physicality p WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = u.id) AND p.path @> ARRAY[u.id] AND laplace_mask_has(p.mask, 1::smallint)) k "
    "JOIN attestation a ON a.claim = k.entity JOIN witness w ON w.id = a.witness", 1, NULL };
PG_FUNCTION_INFO_V1(laplace_attested);
Datum laplace_attested(PG_FUNCTION_ARGS){ TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_attested", &td); set_run(fcinfo, &attested_kept, ts, td); PG_RETURN_NULL(); }

/* ---------------------------------------------------------------- the highway: the types, in place
 * A type is a slot of a list; its content's ID is the record's. laplace_type(list, text) gives the slot of the type
 * whose content is that text (the text taken as one composition of its codepoints, as laplace_text_id does), -1 when
 * the list has none; laplace_type_id(list, slot) the content's ID; laplace_type_edges(a, slot, b) the slots of list b
 * a slot of list a maps to. */
static lp_ref trunk_of(text *t, lp_ref *parts, size_t cap, size_t *np);
PG_FUNCTION_INFO_V1(laplace_type);
Datum laplace_type(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); char *ln = text_to_cstring(PG_GETARG_TEXT_PP(0)); const lp_list *l = lp_highway_list(h, ln);
    if (!l) ereport(ERROR, (errmsg("laplace: the highway has no list named \"%s\"", ln)));
    text *t = PG_GETARG_TEXT_PP(1); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_INT32(-1);
    lp_ref r = trunk_of(t, NULL, 0, NULL);
    PG_RETURN_INT32((int32)lp_highway_slot(h, l, &r.id));
}
PG_FUNCTION_INFO_V1(laplace_type_key);
Datum laplace_type_key(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); char *ln = text_to_cstring(PG_GETARG_TEXT_PP(0)); const lp_list *l = lp_highway_list(h, ln);
    if (!l) ereport(ERROR, (errmsg("laplace: the highway has no list named \"%s\"", ln)));
    PG_RETURN_INT32((int32)lp_highway_key(h, l, text_to_cstring(PG_GETARG_TEXT_PP(1))));
}
PG_FUNCTION_INFO_V1(laplace_type_id);
Datum laplace_type_id(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); char *ln = text_to_cstring(PG_GETARG_TEXT_PP(0)); const lp_list *l = lp_highway_list(h, ln);
    if (!l) ereport(ERROR, (errmsg("laplace: the highway has no list named \"%s\"", ln)));
    int32 slot = PG_GETARG_INT32(1); const lp_tier0_record *r = slot >= 0 ? lp_highway_at(h, l, (uint32)slot) : NULL;
    if (!r) PG_RETURN_NULL();
    return id_datum(&r->id);
}
PG_FUNCTION_INFO_V1(laplace_type_edges);
Datum laplace_type_edges(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); char *a = text_to_cstring(PG_GETARG_TEXT_PP(0)), *b = text_to_cstring(PG_GETARG_TEXT_PP(2)); int32 slot = PG_GETARG_INT32(1);
    const lp_edge *e; size_t n = slot >= 0 ? lp_highway_edges(h, a, (uint32)slot, b, &e) : 0;
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1)); for (size_t i = 0; i < n; i++) d[i] = Int32GetDatum((int32)e[i].to);
    PG_RETURN_ARRAYTYPE_P(construct_array(d, (int)n, INT4OID, 4, true, TYPALIGN_INT));
}
PG_FUNCTION_INFO_V1(laplace_highway_fingerprint);
Datum laplace_highway_fingerprint(PG_FUNCTION_ARGS){
    uint8_t fp[32]; lp_highway_fingerprint(highway(), fp); char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", fp[i]);
    PG_RETURN_TEXT_P(cstring_to_text(hex));
}

/* ---------------------------------------------------------------- observability */
PG_FUNCTION_INFO_V1(laplace_isa);
Datum laplace_isa(PG_FUNCTION_ARGS){
    char buf[256];
    snprintf(buf, sizeof buf, "cpu: %s; dispatch: %s", lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()));
    PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/* ---------------------------------------------------------------- GIN over path geometry
 * The keys of a path are the distinct IDs packed in its vertices. A path holds a set of IDs exactly when every key is
 * present, so containment and overlap are answered by the index alone: no recheck, no path decoded twice. */
#include "access/gin.h"
#include "access/stratnum.h"
#define LP_STRAT_CONTAINS 7        /* path @> blake3[] */
#define LP_STRAT_OVERLAPS 3        /* path && blake3[] */

static Datum *path_keys(Datum path, int32 *n){
    Geo g = geo_of(path); lp_id *ids = palloc(sizeof(lp_id) * (g.n ? g.n : 1)); int m = 0;
    for (uint32 i = 0; i < g.n; i++) lp_xyz_to_id(g.xyzm + 4 * i, &ids[i]);
    qsort(ids, g.n, 16, cmp_id);
    for (uint32 i = 0; i < g.n; i++) if (m == 0 || memcmp(&ids[i], &ids[m - 1], 16)) ids[m++] = ids[i];
    Datum *k = palloc(sizeof(Datum) * (m ? m : 1)); for (int i = 0; i < m; i++) k[i] = id_datum(&ids[i]);
    *n = m; return k;
}
PG_FUNCTION_INFO_V1(laplace_gin_extract_value);
Datum laplace_gin_extract_value(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); PG_RETURN_POINTER(path_keys(PG_GETARG_DATUM(0), n));
}
PG_FUNCTION_INFO_V1(laplace_gin_extract_query);
Datum laplace_gin_extract_query(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); StrategyNumber st = PG_GETARG_UINT16(2); int32 *mode = (int32 *)PG_GETARG_POINTER(6);
    int k; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &k);
    Datum *d = palloc(sizeof(Datum) * (k ? k : 1)); for (int i = 0; i < k; i++) d[i] = id_datum(&ids[i]);
    *n = k; if (k == 0) *mode = st == LP_STRAT_CONTAINS ? GIN_SEARCH_MODE_ALL : GIN_SEARCH_MODE_DEFAULT;
    PG_RETURN_POINTER(d);
}
PG_FUNCTION_INFO_V1(laplace_gin_consistent);
Datum laplace_gin_consistent(PG_FUNCTION_ARGS){
    bool *check = (bool *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    bool *recheck = (bool *)PG_GETARG_POINTER(5); *recheck = false;                /* keys are the exact IDs */
    if (st == LP_STRAT_CONTAINS) { for (int i = 0; i < n; i++) if (!check[i]) PG_RETURN_BOOL(false); PG_RETURN_BOOL(true); }
    for (int i = 0; i < n; i++) if (check[i]) PG_RETURN_BOOL(true);
    PG_RETURN_BOOL(false);
}
PG_FUNCTION_INFO_V1(laplace_gin_triconsistent);
Datum laplace_gin_triconsistent(PG_FUNCTION_ARGS){
    GinTernaryValue *check = (GinTernaryValue *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    if (st == LP_STRAT_CONTAINS) {
        GinTernaryValue r = GIN_TRUE;
        for (int i = 0; i < n; i++) { if (check[i] == GIN_FALSE) PG_RETURN_GIN_TERNARY_VALUE(GIN_FALSE); if (check[i] == GIN_MAYBE) r = GIN_MAYBE; }
        PG_RETURN_GIN_TERNARY_VALUE(r);
    }
    GinTernaryValue r = GIN_FALSE;
    for (int i = 0; i < n; i++) { if (check[i] == GIN_TRUE) PG_RETURN_GIN_TERNARY_VALUE(GIN_TRUE); if (check[i] == GIN_MAYBE) r = GIN_MAYBE; }
    PG_RETURN_GIN_TERNARY_VALUE(r);
}
/* ---------------------------------------------------------------- GIN over the mask
 * A row's mask (Semantics: Claims, Masks) is 256 bits: what the row is (a claim, a record, a tuple, a file), and the
 * types it holds, by the highway's layout. Its keys are the positions of its set bits, so "claims that hold X" is
 * the intersection of X's posting list with the claim bit's, in the one index over (path, mask). */
#include "utils/varbit.h"
#define LP_STRAT_HASBIT  1        /* mask ? int2 */
#define LP_STRAT_HASALL  2        /* mask ?& int2[] */
#define LP_STRAT_HASANY  3        /* mask ?| int2[] */
static Datum *mask_keys(VarBit *m, int32 *n){
    int len = VARBITLEN(m); const bits8 *b = VARBITS(m); Datum *k = palloc(sizeof(Datum) * (len ? len : 1)); int c = 0;
    for (int i = 0; i < len; i++) if (b[i >> 3] & (0x80 >> (i & 7))) k[c++] = Int16GetDatum((int16)i);
    *n = c; return k;
}
PG_FUNCTION_INFO_V1(laplace_mask_extract_value);
Datum laplace_mask_extract_value(PG_FUNCTION_ARGS){ int32 *n = (int32 *)PG_GETARG_POINTER(1); PG_RETURN_POINTER(mask_keys(PG_GETARG_VARBIT_P(0), n)); }
PG_FUNCTION_INFO_V1(laplace_mask_extract_query);
Datum laplace_mask_extract_query(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); StrategyNumber st = PG_GETARG_UINT16(2); int32 *mode = (int32 *)PG_GETARG_POINTER(6);
    if (st == LP_STRAT_HASBIT) { Datum *k = palloc(sizeof(Datum)); k[0] = Int16GetDatum(PG_GETARG_INT16(0)); *n = 1; PG_RETURN_POINTER(k); }
    ArrayType *a = PG_GETARG_ARRAYTYPE_P(0); Datum *v; bool *nulls; int c; deconstruct_array(a, INT2OID, 2, true, TYPALIGN_SHORT, &v, &nulls, &c);
    *n = c; if (c == 0) *mode = st == LP_STRAT_HASALL ? GIN_SEARCH_MODE_ALL : GIN_SEARCH_MODE_DEFAULT;
    PG_RETURN_POINTER(v);
}
PG_FUNCTION_INFO_V1(laplace_mask_consistent);
Datum laplace_mask_consistent(PG_FUNCTION_ARGS){
    bool *check = (bool *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3); bool *recheck = (bool *)PG_GETARG_POINTER(5); *recheck = false;
    if (st == LP_STRAT_HASANY) { for (int i = 0; i < n; i++) if (check[i]) PG_RETURN_BOOL(true); PG_RETURN_BOOL(false); }
    for (int i = 0; i < n; i++) if (!check[i]) PG_RETURN_BOOL(false);
    PG_RETURN_BOOL(true);
}
PG_FUNCTION_INFO_V1(laplace_mask_triconsistent);
Datum laplace_mask_triconsistent(PG_FUNCTION_ARGS){
    GinTernaryValue *check = (GinTernaryValue *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    if (st == LP_STRAT_HASANY) { GinTernaryValue r = GIN_FALSE; for (int i = 0; i < n; i++) { if (check[i] == GIN_TRUE) PG_RETURN_GIN_TERNARY_VALUE(GIN_TRUE); if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } PG_RETURN_GIN_TERNARY_VALUE(r); }
    GinTernaryValue r = GIN_TRUE; for (int i = 0; i < n; i++) { if (check[i] == GIN_FALSE) PG_RETURN_GIN_TERNARY_VALUE(GIN_FALSE); if (check[i] == GIN_MAYBE) r = GIN_MAYBE; }
    PG_RETURN_GIN_TERNARY_VALUE(r);
}
static bool mask_bit(VarBit *m, int16 i){ return i >= 0 && i < VARBITLEN(m) && (VARBITS(m)[i >> 3] & (0x80 >> (i & 7))); }
PG_FUNCTION_INFO_V1(laplace_mask_has);
Datum laplace_mask_has(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(mask_bit(PG_GETARG_VARBIT_P(0), PG_GETARG_INT16(1))); }
static bool mask_has_array(VarBit *m, ArrayType *a, bool all){
    Datum *v; bool *nulls; int c; deconstruct_array(a, INT2OID, 2, true, TYPALIGN_SHORT, &v, &nulls, &c);
    for (int i = 0; i < c; i++) { bool has = mask_bit(m, DatumGetInt16(v[i])); if (all && !has) return false; if (!all && has) return true; }
    return all;
}
PG_FUNCTION_INFO_V1(laplace_mask_has_all);
Datum laplace_mask_has_all(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(mask_has_array(PG_GETARG_VARBIT_P(0), PG_GETARG_ARRAYTYPE_P(1), true)); }
PG_FUNCTION_INFO_V1(laplace_mask_has_any);
Datum laplace_mask_has_any(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(mask_has_array(PG_GETARG_VARBIT_P(0), PG_GETARG_ARRAYTYPE_P(1), false)); }
/* The mask bit of a type, by its content: the highway's field for its list, plus its slot; -1 when it is no type. */
PG_FUNCTION_INFO_V1(laplace_mask_bit);
Datum laplace_mask_bit(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_INT16(-1);
    lp_ref r = trunk_of(t, NULL, 0, NULL); PG_RETURN_INT16((int16)lp_highway_mask_bit(highway(), &r.id));
}
PG_FUNCTION_INFO_V1(laplace_mask_bit_of);
Datum laplace_mask_bit_of(PG_FUNCTION_ARGS){ PG_RETURN_INT16((int16)lp_highway_mask_bit(highway(), (const lp_id *)PG_GETARG_POINTER(0))); }

/* The operators themselves, for plans that do not use the index. */
static bool path_has(Datum path, ArrayType *q, bool all){           /* the path decoded once, then binary search */
    Geo g = geo_of(path); int k; lp_id *ids = ids_of(q, &k);
    lp_id *v = palloc(sizeof(lp_id) * (g.n ? g.n : 1));
    for (uint32 i = 0; i < g.n; i++) lp_xyz_to_id(g.xyzm + 4 * i, &v[i]);
    qsort(v, g.n, 16, cmp_id);
    for (int j = 0; j < k; j++) {
        bool found = bsearch(&ids[j], v, g.n, 16, cmp_id) != NULL;
        if (all && !found) { pfree(v); return false; }
        if (!all && found) { pfree(v); return true; }
    }
    pfree(v); return all;
}
PG_FUNCTION_INFO_V1(laplace_path_contains);
Datum laplace_path_contains(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(path_has(PG_GETARG_DATUM(0), PG_GETARG_ARRAYTYPE_P(1), true)); }
PG_FUNCTION_INFO_V1(laplace_path_overlaps);
Datum laplace_path_overlaps(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(path_has(PG_GETARG_DATUM(0), PG_GETARG_ARRAYTYPE_P(1), false)); }

/* How many times a path holds each of the given IDs, runs included: one pass over the path, a binary search per vertex.
 * Returns only the IDs it holds. */
#include "funcapi.h"
PG_FUNCTION_INFO_V1(laplace_path_times);
Datum laplace_path_times(PG_FUNCTION_ARGS){
    FuncCallContext *fx;
    typedef struct { lp_id *id; int64 *times; int n, i; } State;
    if (SRF_IS_FIRSTCALL()) {
        fx = SRF_FIRSTCALL_INIT(); MemoryContext old = MemoryContextSwitchTo(fx->multi_call_memory_ctx);
        Geo g = geo_of(PG_GETARG_DATUM(0)); int k; lp_id *q = ids_of(PG_GETARG_ARRAYTYPE_P(1), &k);
        qsort(q, k, 16, cmp_id); int m = 0; for (int i = 0; i < k; i++) if (!m || memcmp(&q[i], &q[m - 1], 16)) q[m++] = q[i];
        int64 *t = palloc0(sizeof(int64) * (m ? m : 1)); lp_id v;
        for (uint32 i = 0; i < g.n; i++) {
            lp_xyz_to_id(g.xyzm + 4 * i, &v); lp_id *hit = bsearch(&v, q, m, 16, cmp_id);
            if (hit) { uint32 run = lp_m_run(g.xyzm[4 * i + 3]); t[hit - q] += run < 1 ? 1 : (int64)run; }
        }
        State *s = palloc(sizeof(State)); s->id = q; s->times = t; s->n = m; s->i = 0; fx->user_fctx = s;
        TupleDesc td; if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("laplace_path_times: composite result expected")));
        fx->tuple_desc = BlessTupleDesc(td);
        MemoryContextSwitchTo(old);
    }
    fx = SRF_PERCALL_SETUP(); State *s = fx->user_fctx;
    while (s->i < s->n && !s->times[s->i]) s->i++;
    if (s->i >= s->n) SRF_RETURN_DONE(fx);
    Datum v[2]; bool nl[2] = { false, false }; v[0] = id_datum(&s->id[s->i]); v[1] = Int64GetDatum(s->times[s->i]); s->i++;
    SRF_RETURN_NEXT(fx, HeapTupleGetDatum(heap_form_tuple(fx->tuple_desc, v, nl)));
}
