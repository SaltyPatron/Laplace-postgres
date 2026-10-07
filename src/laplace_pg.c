/* Laplace-postgres: Laplace's 4D expansion of PostgreSQL and PostGIS.
 *
 * Standard geometry types stay as they are; these functions add what Laplace needs next to what PostGIS provides.
 * They read PostGIS's serialized geometry directly (a LINESTRING ZM's vertices are 32-byte X, Y, Z, M blocks, the same
 * block a Laplace-Native path is), and call Laplace-Native for everything: every reading of a path, every map, every
 * centroid. What is here is only the fmgr glue and the set access through SPI.
 *
 * An ID is a BLAKE3 hash, 128 bits. It has a type of its own, blake3: 16 fixed bytes, written as 32 hexadecimal
 * digits, ordered and compared as bytes. */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/gin.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/varbit.h"
#include "varatt.h"
#include "laplace/laplace.h"

PG_MODULE_MAGIC_EXT(.name = "laplace", .version = "1.11");

/* ---------------------------------------------------------------- PostGIS serialized geometry (version 2) */
#include "geo.h"

/* ---------------------------------------------------------------- IDs as Datums */
static Datum id_datum(const lp_id *id){ uint8 *u = palloc(16); memcpy(u, id->b, 16); return PointerGetDatum(u); }
/* An array of IDs of a given element type. */
static ArrayType *id_array_of(Oid el, const lp_id *ids, int n){
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1));
    for (int i = 0; i < n; i++) d[i] = id_datum(&ids[i]);
    return construct_array(d, n, el, 16, false, TYPALIGN_CHAR);
}
/* An array of IDs, of the type the function is declared to return. */
static ArrayType *id_array(FunctionCallInfo fcinfo, const lp_id *ids, int n){
    Oid el = get_element_type(get_fn_expr_rettype(fcinfo->flinfo));
    if (!OidIsValid(el)) ereport(ERROR, (errmsg("laplace: the function does not return an array of IDs")));
    return id_array_of(el, ids, n);
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
static bytea *bytes_datum(const uint8 *b, size_t n){ bytea *r = palloc(VARHDRSZ + n); SET_VARSIZE(r, VARHDRSZ + n); memcpy(VARDATA(r), b, n); return r; }
static bytea *coord_ewkb(const lp_coord *c){ uint8 e[37]; lp_ewkb_coord(c, e, sizeof e); return bytes_datum(e, sizeof e); }
static text *fingerprint_text(const uint8 fp[32]){ char hex[65]; lp_hex(fp, 32, hex); return cstring_to_text(hex); }

/* ---------------------------------------------------------------- the type of an ID: blake3 */
PG_FUNCTION_INFO_V1(blake3_in);
Datum blake3_in(PG_FUNCTION_ARGS){
    const char *t = PG_GETARG_CSTRING(0); lp_id id;
    if (strlen(t) != 32 || !lp_id_unhex(t, &id)) ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION), errmsg("a blake3 ID is 32 hexadecimal digits: \"%s\"", t)));
    return id_datum(&id);
}
PG_FUNCTION_INFO_V1(blake3_out);
Datum blake3_out(PG_FUNCTION_ARGS){ char *t = palloc(33); lp_id_hex((const lp_id *)PG_GETARG_POINTER(0), t); PG_RETURN_CSTRING(t); }
PG_FUNCTION_INFO_V1(blake3_recv);
Datum blake3_recv(PG_FUNCTION_ARGS){ StringInfo b = (StringInfo)PG_GETARG_POINTER(0); uint8 *u = palloc(16); memcpy(u, pq_getmsgbytes(b, 16), 16); PG_RETURN_POINTER(u); }
PG_FUNCTION_INFO_V1(blake3_send);
Datum blake3_send(PG_FUNCTION_ARGS){ StringInfoData b; pq_begintypsend(&b); pq_sendbytes(&b, PG_GETARG_POINTER(0), 16); PG_RETURN_BYTEA_P(pq_endtypsend(&b)); }
#define ID_CMP(name, test) PG_FUNCTION_INFO_V1(name); Datum name(PG_FUNCTION_ARGS){ int c = lp_id_cmp(PG_GETARG_POINTER(0), PG_GETARG_POINTER(1)); PG_RETURN_BOOL(test); }
ID_CMP(blake3_eq, c == 0) ID_CMP(blake3_ne, c != 0) ID_CMP(blake3_lt, c < 0) ID_CMP(blake3_le, c <= 0) ID_CMP(blake3_gt, c > 0) ID_CMP(blake3_ge, c >= 0)
PG_FUNCTION_INFO_V1(blake3_cmp);
Datum blake3_cmp(PG_FUNCTION_ARGS){ int c = lp_id_cmp(PG_GETARG_POINTER(0), PG_GETARG_POINTER(1)); PG_RETURN_INT32(c < 0 ? -1 : c > 0); }
/* A hash is already evenly spread: its own bytes are the hash a hash index or a hash join asks for. */
PG_FUNCTION_INFO_V1(blake3_hash);
Datum blake3_hash(PG_FUNCTION_ARGS){ uint32 h; memcpy(&h, PG_GETARG_POINTER(0), 4); PG_RETURN_UINT32(h); }
PG_FUNCTION_INFO_V1(blake3_hash_extended);
Datum blake3_hash_extended(PG_FUNCTION_ARGS){ return hash_any_extended((const unsigned char *)PG_GETARG_POINTER(0), 16, PG_GETARG_INT64(1)); }

/* ---------------------------------------------------------------- the perf-caches, mapped in this backend */
static char *tier0_path = NULL, *flags_path = NULL, *highway_path = NULL;
static const lp_tier0_record *tier0(void){
    const lp_tier0_record *t = lp_tier0_map(tier0_path);
    if (!t) ereport(ERROR, (errmsg("laplace: cannot map tier 0 at \"%s\" (laplace.tier0)", tier0_path)));
    return t;
}
static const lp_layout *flags(void){
    const lp_layout *l = lp_flags_map(flags_path);
    if (!l) ereport(ERROR, (errmsg("laplace: cannot map the flags at \"%s\" (laplace.flags)", flags_path ? flags_path : lp_flags_path())));
    return l;
}
static const lp_highway *highway(void){
    const lp_highway *h = lp_highway_map(highway_path);
    if (!h) ereport(ERROR, (errmsg("laplace: cannot map the highway at \"%s\" (laplace.highway; generate it with: laplace highway)", highway_path)));
    return h;
}
/* Native's working memory (its maps, arrays, buffers) comes from the transaction's memory: what a call holds is freed by
 * the call, and whatever an error leaves behind goes with the transaction. */
static void *tx_grow(void *p, size_t n){ return p ? repalloc_huge(p, n) : MemoryContextAllocHuge(TopTransactionContext, n); }
static void tx_release(void *p){ pfree(p); }
void _PG_init(void);
void _PG_init(void){
    lp_allocator(tx_grow, tx_release);
    DefineCustomStringVariable("laplace.tier0", "Path of the tier-0 perf-cache (1,114,112 64-byte records).", NULL, &tier0_path,
                               lp_tier0_path(), PGC_SUSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("laplace.flags", "Path of the flags that go with tier 0 (1,114,112 256-bit records; their layout beside them).", NULL, &flags_path,
                               lp_flags_path(), PGC_SUSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("laplace.highway", "Path of the highway perf-cache (the types the resources list, and the mappings between them; its layout beside it).", NULL, &highway_path,
                               lp_highway_path(), PGC_SUSET, 0, NULL, NULL, NULL);
}

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

/* The distinct IDs a path holds: the keys of the container index. */
static lp_id *path_keys(const Geo *g, int *n){ lp_id *k = palloc(sizeof(lp_id) * (g->p.n ? g->p.n : 1)); *n = (int)lp_path_keys(g->p, k); return k; }
PG_FUNCTION_INFO_V1(laplace_vertex_ids);
Datum laplace_vertex_ids(PG_FUNCTION_ARGS){ Geo g = geo_of(PG_GETARG_DATUM(0)); int m; lp_id *k = path_keys(&g, &m); PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, k, m)); }

/* laplace_middle_any(path, ids): whether a path holds any of the IDs between its first part and its last, the place a
 * claim's predicate takes, runs counted as the parts they repeat. The claim reads take out the strands a firmware
 * refuses with it, before their fan: a restriction is applied before the sort (Sequence 15.5). */
PG_FUNCTION_INFO_V1(laplace_middle_any);
Datum laplace_middle_any(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0)); int nr; lp_id *refuse = ids_of(PG_GETARG_ARRAYTYPE_P(1), &nr);
    PG_RETURN_BOOL(lp_path_middle_any(g.p, refuse, (size_t)nr));
}

/* Continuations: the ID after every run of the phrase inside the path. */
PG_FUNCTION_INFO_V1(laplace_follows);
Datum laplace_follows(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0)); int np; lp_id *phrase = ids_of(PG_GETARG_ARRAYTYPE_P(1), &np);
    size_t cap = g.p.n * 4 + 16; lp_id *out = palloc(sizeof(lp_id) * cap);
    size_t k = lp_path_follows(g.p, phrase, (size_t)np, out, cap);
    if (k == 0) PG_RETURN_NULL();
    PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, out, (int)(k < cap ? k : cap)));
}

/* ---------------------------------------------------------------- 4D geometry on real coordinates */
PG_FUNCTION_INFO_V1(laplace_distance4d);
Datum laplace_distance4d(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1));
    if (a.type != 1 || b.type != 1) ereport(ERROR, (errmsg("laplace_distance4d: expected two POINT ZM")));
    PG_RETURN_FLOAT8(lp_distance4(xyzm(&a), xyzm(&b)));
}

PG_FUNCTION_INFO_V1(laplace_frechet4d);
Datum laplace_frechet4d(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1));
    PG_RETURN_FLOAT8(lp_frechet4(xyzm(&a), a.p.n, xyzm(&b), b.p.n));
}

/* A POINT ZM's coordinate on the fixed-point grid, each axis truncated toward zero onto it. */
static lp_coord coord_of(const Geo *g){
    if (g->type != 1 || g->p.n != 1) ereport(ERROR, (errmsg("laplace: expected a POINT ZM")));
    lp_coord c; lp_coord_trunc(xyzm(g), &c); return c;
}

PG_FUNCTION_INFO_V1(laplace_hilbert4);
Datum laplace_hilbert4(PG_FUNCTION_ARGS){ Geo g = geo_of(PG_GETARG_DATUM(0)); lp_coord c = coord_of(&g); PG_RETURN_INT64((int64)lp_hilbert4(&c)); }

PG_FUNCTION_INFO_V1(laplace_inside);
Datum laplace_inside(PG_FUNCTION_ARGS){ Geo g = geo_of(PG_GETARG_DATUM(0)); lp_coord c = coord_of(&g); PG_RETURN_BOOL(lp_coord_inside(&c)); }

/* Exact 4D centroid aggregate: Native's one centroid, its sums the aggregate's state, one division at the end. */
PG_FUNCTION_INFO_V1(laplace_centroid4d_step);
Datum laplace_centroid4d_step(PG_FUNCTION_ARGS){
    MemoryContext agg; if (!AggCheckCallContext(fcinfo, &agg)) ereport(ERROR, (errmsg("laplace_centroid4d_step called outside an aggregate")));
    lp_coord_sum *st = PG_ARGISNULL(0) ? MemoryContextAllocZero(agg, sizeof *st) : (lp_coord_sum *)PG_GETARG_POINTER(0);
    if (!PG_ARGISNULL(1)) { Geo g = geo_of(PG_GETARG_DATUM(1)); lp_coord c = coord_of(&g); lp_coord_add(st, c.m); }
    PG_RETURN_POINTER(st);
}

PG_FUNCTION_INFO_V1(laplace_centroid4d_final);
Datum laplace_centroid4d_final(PG_FUNCTION_ARGS){
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    const lp_coord_sum *st = (const lp_coord_sum *)PG_GETARG_POINTER(0);
    if (st->n == 0) PG_RETURN_NULL();
    lp_coord c; lp_coord_mean(st, &c); PG_RETURN_BYTEA_P(coord_ewkb(&c));
}

/* ---------------------------------------------------------------- the tier-0 perf-cache: coordinates computed in place */
/* The coordinate of a text taken as one composition of its codepoints: the exact centroid of their tier-0 points. */
static bool text_coord(text *t, lp_coord *out){
    bool ok; lp_ref r = lp_ref_codepoints(tier0(), (const uint8_t *)VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &ok);
    *out = r.c; return ok;
}

PG_FUNCTION_INFO_V1(laplace_text_coord_ewkb);
Datum laplace_text_coord_ewkb(PG_FUNCTION_ARGS){ lp_coord c; if (!text_coord(PG_GETARG_TEXT_PP(0), &c)) PG_RETURN_NULL(); PG_RETURN_BYTEA_P(coord_ewkb(&c)); }

/* The stored Hilbert key: the Hilbert value with its top bit flipped, so bigint order is Hilbert order. */
PG_FUNCTION_INFO_V1(laplace_text_hilbert);
Datum laplace_text_hilbert(PG_FUNCTION_ARGS){ lp_coord c; if (!text_coord(PG_GETARG_TEXT_PP(0), &c)) PG_RETURN_NULL(); PG_RETURN_INT64(lp_hilbert_key(lp_hilbert4(&c))); }

PG_FUNCTION_INFO_V1(laplace_cp_coord_ewkb);
Datum laplace_cp_coord_ewkb(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); if (cp < 0 || (uint32)cp >= LP_NCP) PG_RETURN_NULL();
    lp_ref a = lp_ref_atom(tier0(), (uint32)cp); PG_RETURN_BYTEA_P(coord_ewkb(&a.c));
}

/* ---------------------------------------------------------------- the flags that go with tier 0
 * What the Unicode Standard says of a codepoint, from the memory-mapped flags: by the property's name and the value's
 * name as the standard writes them, short or as they are said. Nothing is read from a table. */
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
    PG_RETURN_BYTEA_P(bytes_datum(flags()->flags[cp].b, 32));
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
#define TEXT_ARG(t) text *t = PG_GETARG_TEXT_PP(0); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_NULL()

PG_FUNCTION_INFO_V1(laplace_id);
Datum laplace_id(PG_FUNCTION_ARGS){ TEXT_ARG(t); lp_ref r = trunk_of(t, NULL, 0, NULL); return id_datum(&r.id); }

PG_FUNCTION_INFO_V1(laplace_tier);
Datum laplace_tier(PG_FUNCTION_ARGS){ TEXT_ARG(t); PG_RETURN_INT16((int16)trunk_of(t, NULL, 0, NULL).tier); }

PG_FUNCTION_INFO_V1(laplace_coord_ewkb);
Datum laplace_coord_ewkb(PG_FUNCTION_ARGS){ TEXT_ARG(t); lp_ref r = trunk_of(t, NULL, 0, NULL); PG_RETURN_BYTEA_P(coord_ewkb(&r.c)); }

/* The stored Hilbert key of a text's entity. */
PG_FUNCTION_INFO_V1(laplace_hilbert);
Datum laplace_hilbert(PG_FUNCTION_ARGS){ TEXT_ARG(t); lp_ref r = trunk_of(t, NULL, 0, NULL); PG_RETURN_INT64(lp_hilbert_key(lp_hilbert4(&r.c))); }

/* The constituents of a text's entity, in order, repeats included: the phrase to look for inside paths. */
PG_FUNCTION_INFO_V1(laplace_parts);
Datum laplace_parts(PG_FUNCTION_ARGS){
    TEXT_ARG(t);
    size_t cap = VARSIZE_ANY_EXHDR(t) + 1, n; lp_ref *parts = palloc(sizeof(lp_ref) * cap);
    trunk_of(t, parts, cap, &n); if (n > cap) n = cap;
    lp_id *ids = palloc(sizeof(lp_id) * n); for (size_t i = 0; i < n; i++) ids[i] = parts[i].id;
    PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, ids, (int)n));
}

/* Which tier 0 this database computes with. */
PG_FUNCTION_INFO_V1(laplace_fingerprint);
Datum laplace_fingerprint(PG_FUNCTION_ARGS){ uint8 h[32]; lp_tier0_fingerprint(tier0(), h); PG_RETURN_TEXT_P(fingerprint_text(h)); }

/* ---------------------------------------------------------------- shape measures on real coordinates */
PG_FUNCTION_INFO_V1(laplace_frechet4d_outliers);
Datum laplace_frechet4d_outliers(PG_FUNCTION_ARGS){
    Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1)); int32 k = PG_GETARG_INT32(2);
    if (k < 0 || k > 8) ereport(ERROR, (errmsg("laplace_frechet4d: between 0 and 8 vertices can be skipped")));
    PG_RETURN_FLOAT8(lp_frechet4_outliers(xyzm(&a), a.p.n, xyzm(&b), b.p.n, (unsigned)k));
}
PG_FUNCTION_INFO_V1(laplace_dtw4d);
Datum laplace_dtw4d(PG_FUNCTION_ARGS){ Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1)); PG_RETURN_FLOAT8(lp_dtw4(xyzm(&a), a.p.n, xyzm(&b), b.p.n, NULL)); }
PG_FUNCTION_INFO_V1(laplace_edr4d);
Datum laplace_edr4d(PG_FUNCTION_ARGS){ Geo a = geo_of(PG_GETARG_DATUM(0)), b = geo_of(PG_GETARG_DATUM(1)); PG_RETURN_INT64((int64)lp_edr4(xyzm(&a), a.p.n, xyzm(&b), b.p.n, PG_GETARG_FLOAT8(2))); }

/* How hard a strand tugs back, from a standing. */
PG_FUNCTION_INFO_V1(laplace_confidence);
Datum laplace_confidence(PG_FUNCTION_ARGS){
    lp_rating r = { PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), LP_GLICKO_VOLATILITY };
    PG_RETURN_FLOAT8(lp_confidence(&r, PG_GETARG_FLOAT8(2)));
}

/* ---------------------------------------------------------------- set access: statements planned once, run over sets
 * Every read of the web is a statement planned once per backend and kept, run as one executor pass over a set, its
 * rows returned through a tuplestore. */
typedef struct { const char *name; const char *sql; int nargs; SPIPlanPtr plan; } Kept;
#define KEPT_ARGS 8
/* The statement's plan, made the first time it is run, with these argument types. Inside SPI. */
static SPIPlanPtr kept_plan(Kept *k, const Oid *types){
    if (!k->plan) {
        k->plan = SPI_prepare(k->sql, k->nargs, (Oid *)types);
        if (!k->plan) ereport(ERROR, (errmsg("%s: SPI_prepare: %s", k->name, SPI_result_code_string(SPI_result))));
        SPI_keepplan(k->plan);
    }
    return k->plan;
}
static void kept_run(Kept *k, const Oid *types, Datum *args){
    if (SPI_execute_plan(kept_plan(k, types), args, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("%s: %s", k->name, SPI_result_code_string(SPI_result))));
}
static Tuplestorestate *set_begin(FunctionCallInfo fcinfo, const char *name, TupleDesc *td){
    ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
    if (!rsi || !IsA(rsi, ReturnSetInfo) || !(rsi->allowedModes & SFRM_Materialize)) ereport(ERROR, (errmsg("%s: set-valued function called in a context that cannot accept a set", name)));
    if (get_call_result_type(fcinfo, NULL, td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("%s: composite result expected", name)));
    MemoryContext old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    Tuplestorestate *ts = tuplestore_begin_heap(true, false, 65536); *td = CreateTupleDescCopy(*td); MemoryContextSwitchTo(old);
    rsi->returnMode = SFRM_Materialize; rsi->setResult = ts; rsi->setDesc = *td;
    return ts;
}
/* A row into the set, in the query's memory (the tuplestore's), whatever memory the caller is in. */
static void set_put(FunctionCallInfo fcinfo, Tuplestorestate *ts, TupleDesc td, Datum *v, bool *nl){
    MemoryContext m = MemoryContextSwitchTo(((ReturnSetInfo *)fcinfo->resultinfo)->econtext->ecxt_per_query_memory);
    tuplestore_putvalues(ts, td, v, nl); MemoryContextSwitchTo(m);
}
/* Run a kept statement over the call's own arguments, its argument types the call's, and hand every row through. */
static void set_run(FunctionCallInfo fcinfo, Kept *k){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, k->name, &td);
    if (k->nargs > KEPT_ARGS || td->natts > KEPT_ARGS) ereport(ERROR, (errmsg("%s: more than %d arguments or columns", k->name, KEPT_ARGS)));
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("%s: SPI_connect", k->name)));
    Oid t[KEPT_ARGS]; Datum a[KEPT_ARGS];
    for (int i = 0; i < k->nargs; i++) { t[i] = get_fn_expr_argtype(fcinfo->flinfo, i); a[i] = PG_GETARG_DATUM(i); }
    kept_run(k, t, a);
    int nc = SPI_tuptable->tupdesc->natts;
    for (uint64 j = 0; j < SPI_processed; j++) {
        Datum v[KEPT_ARGS]; bool nl[KEPT_ARGS];
        for (int c = 0; c < nc; c++) v[c] = SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, c + 1, &nl[c]);
        tuplestore_putvalues(ts, td, v, nl);
    }
    SPI_finish();
}
#define SET_FN(fn, kept) PG_FUNCTION_INFO_V1(fn); Datum fn(PG_FUNCTION_ARGS){ set_run(fcinfo, &kept); PG_RETURN_NULL(); }

/* ---------------------------------------------------------------- recomposition: an entity back to its text
 * The composition is read one DAG level per statement: every entity a level names that is not an atom and not yet
 * read is fetched in one set, from the trunk down, and the text is then written out of memory in path order. */
typedef struct { uint32 *kid; uint32 nkid; bool have; } TNode;     /* kid: the node's children, by their places in the map, one per occurrence */
static uint32 tnode(lp_idmap *t, const lp_id *id){
    bool fresh; size_t i = lp_idmap_put(t, id, &fresh);
    if (fresh) ((TNode *)lp_idmap_at(t, i))->have = lp_tier0_codepoint(tier0(), id) >= 0;     /* an atom has nothing to read */
    return (uint32)i;
}
static Kept level_kept = { "laplace_text", "SELECT entity, st_asewkb(path) FROM physicality WHERE entity = ANY($1)", 1, NULL };
static void read_levels(lp_idmap *t, Oid idarr, MemoryContext keep){
    for (int depth = 0; ; depth++) {
        if (depth > 64) ereport(ERROR, (errmsg("laplace_text: composition deeper than 64 tiers")));
        lp_vec(lp_id) want = { 0 };
        for (size_t i = 0; i < lp_idmap_count(t); i++) if (!((TNode *)lp_idmap_at(t, i))->have) lp_push(&want, *lp_idmap_key(t, i));
        if (!want.n) return;
        Datum arg = PointerGetDatum(id_array_of(get_element_type(idarr), want.v, (int)want.n));
        kept_run(&level_kept, &idarr, &arg);
        if (SPI_processed < want.n) ereport(ERROR, (errmsg("laplace_text: no physicality for an entity")));
        for (uint64 j = 0; j < SPI_processed; j++) {
            bool nl; lp_id id; memcpy(&id, DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 1, &nl)), 16);
            bytea *e = DatumGetByteaP(SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, 2, &nl));
            lp_path p = lp_path_of((const uint8_t *)VARDATA_ANY(e), VARSIZE_ANY_EXHDR(e)); size_t n = lp_path_len(p);
            uint32 *kid = MemoryContextAlloc(keep, sizeof(uint32) * (n ? n : 1)), k = 0;
            for (size_t i = 0; i < p.n; i++) { lp_id cid = lp_path_id(p, i); uint32 c = tnode(t, &cid); for (uint32 r = 0; r < lp_path_run(p, i); r++) kid[k++] = c; }
            TNode *me = lp_idmap_at(t, tnode(t, &id)); me->kid = kid; me->nkid = (uint32)n; me->have = true;
        }
        SPI_freetuptable(SPI_tuptable); lp_vec_free(&want);
    }
}
static void render(lp_idmap *t, uint32 i, StringInfo out){
    const TNode *x = lp_idmap_at(t, i); int64 cp = x->nkid ? -1 : lp_tier0_codepoint(tier0(), lp_idmap_key(t, i));
    if (cp >= 0) { uint8_t u[4]; appendBinaryStringInfo(out, (const char *)u, (int)lp_utf8_put((uint32)cp, u)); return; }
    for (uint32 k = 0; k < x->nkid; k++) render(t, ((const TNode *)lp_idmap_at(t, i))->kid[k], out);
}
PG_FUNCTION_INFO_V1(laplace_text);
Datum laplace_text(PG_FUNCTION_ARGS){
    StringInfoData out; initStringInfo(&out); MemoryContext keep = CurrentMemoryContext;
    lp_idmap *t = lp_idmap_sized(sizeof(TNode)); uint32 root = tnode(t, (const lp_id *)PG_GETARG_POINTER(0));
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_text: SPI")));
    read_levels(t, get_array_type(get_fn_expr_argtype(fcinfo->flinfo, 0)), keep);
    SPI_finish();
    render(t, root, &out); lp_idmap_free(t);
    PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}

/* ---------------------------------------------------------------- the web: what tugs back when a strand is pulled
 * A claim is content like any other, hashed over its parts. A tier is a floor: what holds an entity sits above it, at
 * whatever tier its own recipe composed it, so the claims that hold an entity are found by the ID they hold, among the
 * paths above the entity's tier (only the tiers at and below are pruned), that the consensus knows. */
/* laplace_claims(parts, fan, bits, refuse): the claims holding every one of the parts, none refused, with the consensus
 * on each: at most fan of them, the first fan by ID (a LIMIT alone keeps whatever the scan met first, which differs
 * between two installs of the same content). A claim sits above its highest part, at no fixed distance: every tier above is read, by
 * the IDs held. */
static Kept claims_kept = { "laplace_claims",
    "SELECT p.entity, p.path, s.rating, s.deviation, s.volatility, s.matches FROM physicality p JOIN consensus s ON s.claim = p.entity "
    "WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path @> $1 AND p.mask ?& $3 AND NOT laplace_middle_any(p.path, $4) ORDER BY p.entity LIMIT $2", 4, NULL };
SET_FN(laplace_claims, claims_kept)
/* laplace_claims_each(ids, fan, bits, refuse): for each of a set of entities, the claims holding it, none refused: one
 * call for a whole level of a walk. At most fan an entity, whichever the scan meets first: a caller asks for one more
 * than its fan and treats an entity that returns more than the fan as a hub, reached and never crossed, so what it
 * uses is every claim of the entity or none of them. i is the entity's place in the set. The entity is a rare key (an observation, a
 * claim), so the path alone is the index condition and the bits are a check on the rows found: the kind bit's posting
 * list is every claim. */
static Kept each_kept = { "laplace_claims_each",
    "SELECT u.i, c.entity, c.path, c.rating, c.deviation, c.volatility, c.matches FROM unnest($1) WITH ORDINALITY AS u(id, i) JOIN entity e ON e.id = u.id "
    "CROSS JOIN LATERAL (SELECT p.entity, p.path, s.rating, s.deviation, s.volatility, s.matches FROM physicality p JOIN consensus s ON s.claim = p.entity "
    "WHERE p.tier > e.tier AND p.path @> ARRAY[u.id] AND laplace_mask_has_all(p.mask, $3) AND NOT laplace_middle_any(p.path, $4) LIMIT $2) c", 4, NULL };
SET_FN(laplace_claims_each, each_kept)
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
Datum laplace_containers(PG_FUNCTION_ARGS){
    ArrayType *bits = PG_GETARG_ARRAYTYPE_P(1);
    set_run(fcinfo, ArrayGetNItems(ARR_NDIM(bits), ARR_DIMS(bits)) ? &containers_kept : &containers_any_kept); PG_RETURN_NULL();
}
/* laplace_fills(keys): every path above the lowest key that holds any of them, with the times each key is followed by
 * the next vertex of that path: what fills the gap after it. */
static Kept fills_kept = { "laplace_fills",
    "SELECT p.entity, t.id, t.times, p.tier FROM physicality p, laplace_path_times(p.path, $1) t "
    "WHERE p.tier > (SELECT min(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path && $1", 1, NULL };
SET_FN(laplace_fills, fills_kept)
/* laplace_paths(ids): the paths of a set of entities: one DAG level per call. */
static Kept paths_kept = { "laplace_paths", "SELECT entity, path FROM physicality WHERE entity = ANY($1)", 1, NULL };
SET_FN(laplace_paths, paths_kept)

/* laplace_forward(ids, fan): the forward pass over every contiguous segment of a prompt at once. For each segment
 * [i..j] of the prompt's constituents: the observations holding all of its parts (claims left out), how many hold it as
 * a run, and what follows the run in each, counted. A segment held by more than fan observations is a hub, reached and
 * not crossed: its row says paths fan + 1 (more than the fan), runs 0, and nothing follows it. Counting over the first
 * fan the index met would count a different set on another install of the same content. One row per continuation (next, times);
 * a segment held by nothing, or held where nothing follows the run, has a row with next null. SQL only fetches the
 * paths that hold a segment, through the container index (Architecture: SQL fetches and writes records); the runs are
 * matched and what follows them is counted here, by Laplace-Native: precedes, contains and co-occurrence from the
 * trajectories, no softmax, no window. */
static Kept forward_kept = { "laplace_forward",
    "SELECT p.path, p.tier FROM physicality p WHERE p.tier > (SELECT max(e.tier) FROM entity e WHERE e.id = ANY($1)) AND p.path @> $1 "
    "AND NOT laplace_mask_has(p.mask, 0::smallint) LIMIT $2", 2, NULL };
static Kept tiers_kept = { "laplace_forward tiers", "SELECT u.i, e.tier FROM unnest($1) WITH ORDINALITY u(id, i) JOIN entity e ON e.id = u.id", 1, NULL };
/* A segment's observations: each path as it lies, and its tier. */
typedef struct { Datum path; int16 tier; } Held;
typedef lp_vec(Held) Helds;
PG_FUNCTION_INFO_V1(laplace_forward);
Datum laplace_forward(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_forward", &td);
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0); Oid el = ARR_ELEMTYPE(arr), at = get_fn_expr_argtype(fcinfo->flinfo, 0), types[2] = { at, INT8OID };
    int n; lp_id *ids = ids_of(arr, &n); int64 fan = PG_GETARG_INT64(1); if (n > 256) n = 256;
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_forward: SPI_connect")));
    /* each constituent's highest recorded tier (-1: not recorded): which are compositions (words and above), and the
     * floor a segment's holders sit above. A segment of atoms alone (a space, a letter) is a hub and says nothing of the
     * prompt, and a single constituent is the constituents' own step, not a segment */
    int16 *tier = palloc(sizeof(int16) * (size_t)n); for (int i = 0; i < n; i++) tier[i] = -1;
    { Datum a0[1] = { PointerGetDatum(arr) }; kept_run(&tiers_kept, &at, a0);
      for (uint64 r = 0; r < SPI_processed; r++) { bool nl; int64 i = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)); int16 t = DatumGetInt16(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl));
          if (i >= 1 && i <= n && t > tier[i - 1]) tier[i - 1] = t; } }
    /* What holds [i..j] holds [i..j-1]: while the shorter segment's holders were all of them (fewer than the fan), the
     * longer one's are those of them that hold its new part too and sit above its highest tier, found here, not asked
     * for again. A segment whose shorter one reached the fan is asked of the index. */
    lp_idmap *followed = lp_idmap_sized(sizeof(int64)); lp_vec(lp_id) out = { 0 };
    /* the paths a segment starting at i was answered with live until the next i: what is derived from them points at them */
    MemoryContext rowctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward rows", ALLOCSET_DEFAULT_SIZES),
                  keepctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward held", ALLOCSET_DEFAULT_SIZES);
    Helds held[2] = { { 0 }, { 0 } };
    for (int i = 0; i < n; i++) {
        MemoryContextReset(keepctx);
        int cur = 0, have = 0, floor = tier[i], words = tier[i] > 0;                   /* have: held[cur] is every holder of [i..j-1] */
        for (int j = i + 1; j < n; j++) {
            words += tier[j] > 0; if (tier[j] > floor) floor = tier[j];
            int next = cur ^ 1, np = j - i + 1; held[next].n = 0;
            if (!words) { have = 0; continue; }
            if (!have) {
                MemoryContext old = MemoryContextSwitchTo(rowctx);
                Datum a[2] = { PointerGetDatum(id_array_of(el, ids + i, np)), Int64GetDatum(fan + 1) };
                kept_run(&forward_kept, types, a);
                MemoryContextSwitchTo(keepctx);
                for (uint64 r = 0; r < SPI_processed; r++) { bool nl; Datum d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl);     /* path is NOT NULL */
                    int16 t = DatumGetInt16(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl));
                    lp_push(&held[next], (Held){ PointerGetDatum(PG_DETOAST_DATUM_COPY(d)), t }); }
                SPI_freetuptable(SPI_tuptable); MemoryContextSwitchTo(old); MemoryContextReset(rowctx);
                have = held[next].n <= (size_t)fan;                                    /* no more than the fan: every holder there is */
                if (!have) {                                                           /* a hub: reached, not crossed */
                    Datum v[6]; bool nl[6] = { false, false, false, false, true, false };
                    v[0] = Int32GetDatum(i + 1); v[1] = Int32GetDatum(j + 1); v[2] = Int64GetDatum(fan + 1); v[3] = Int64GetDatum(0); v[4] = (Datum)0; v[5] = Int64GetDatum(0);
                    set_put(fcinfo, ts, td, v, nl); cur = next; continue;
                }
            } else
                for (size_t r = 0; r < held[cur].n; r++) { const Held *h = &held[cur].v[r]; Geo g = geo_of(h->path);
                    if (h->tier > floor && lp_path_holds(g.p, &ids[j], 1, true)) lp_push(&held[next], *h); }
            /* what follows the run in each path, counted by ID */
            uint64 paths = held[next].n; int64 runs = 0; bool unfollowed = paths == 0;
            lp_idmap_clear(followed);
            for (size_t r = 0; r < held[next].n; r++) {
                Geo g = geo_of(held[next].v[r].path); size_t cap = g.p.n * 4 + 16; lp_vec_reserve(&out, cap);
                size_t found = lp_path_follows(g.p, ids + i, (size_t)np, out.v, cap);
                if (!found) { unfollowed = true; continue; }                     /* it holds the parts, and not as a run that anything follows */
                runs++; if (found > cap) found = cap;
                for (size_t x = 0; x < found; x++) (*(int64 *)lp_idmap_get(followed, &out.v[x], NULL))++;
            }
            Datum v[6]; bool nl[6] = { false, false, false, false, false, false };
            v[0] = Int32GetDatum(i + 1); v[1] = Int32GetDatum(j + 1); v[2] = Int64GetDatum((int64)paths); v[3] = Int64GetDatum(runs);
            for (size_t x = 0; x < lp_idmap_count(followed); x++) { v[4] = id_datum(lp_idmap_key(followed, x)); v[5] = Int64GetDatum(*(int64 *)lp_idmap_at(followed, x)); set_put(fcinfo, ts, td, v, nl); }
            if (unfollowed) { v[4] = (Datum)0; nl[4] = true; v[5] = Int64GetDatum(0); set_put(fcinfo, ts, td, v, nl); }
            cur = next;
        }
    }
    MemoryContextDelete(rowctx); MemoryContextDelete(keepctx);
    lp_idmap_free(followed); lp_vec_free(&out); lp_vec_free(&held[0]); lp_vec_free(&held[1]);
    SPI_finish(); PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- the highway: the types, in place
 * A type is a slot of a list; its content's ID is the record's. laplace_type(list, text) gives the slot of the type
 * whose content is that text (the text taken as one composition of its codepoints, as laplace_text_id does), -1 when
 * the list has none; laplace_type_id(list, slot) the content's ID; laplace_type_edges(a, slot, b) the slots of list b
 * a slot of list a maps to. */
static const lp_list *list_of(const lp_highway *h, text *name){
    char *ln = text_to_cstring(name); const lp_list *l = lp_highway_list(h, ln);
    if (!l) ereport(ERROR, (errmsg("laplace: the highway has no list named \"%s\"", ln)));
    return l;
}
PG_FUNCTION_INFO_V1(laplace_type);
Datum laplace_type(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); const lp_list *l = list_of(h, PG_GETARG_TEXT_PP(0));
    text *t = PG_GETARG_TEXT_PP(1); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_INT32(-1);
    lp_ref r = trunk_of(t, NULL, 0, NULL);
    PG_RETURN_INT32((int32)lp_highway_slot(h, l, &r.id));
}
PG_FUNCTION_INFO_V1(laplace_type_key);
Datum laplace_type_key(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); const lp_list *l = list_of(h, PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32((int32)lp_highway_key(h, l, text_to_cstring(PG_GETARG_TEXT_PP(1))));
}
PG_FUNCTION_INFO_V1(laplace_type_id);
Datum laplace_type_id(PG_FUNCTION_ARGS){
    const lp_highway *h = highway(); const lp_list *l = list_of(h, PG_GETARG_TEXT_PP(0));
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
Datum laplace_highway_fingerprint(PG_FUNCTION_ARGS){ uint8_t fp[32]; lp_highway_fingerprint(highway(), fp); PG_RETURN_TEXT_P(fingerprint_text(fp)); }

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
#define LP_STRAT_CONTAINS 7        /* path @> blake3[] */
#define LP_STRAT_OVERLAPS 3        /* path && blake3[] */

/* Whether every check is true (all) or any is; GIN's maybe stays maybe. */
static GinTernaryValue tri_all(const GinTernaryValue *check, int32 n){ GinTernaryValue r = GIN_TRUE; for (int i = 0; i < n; i++) { if (check[i] == GIN_FALSE) return GIN_FALSE; if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } return r; }
static GinTernaryValue tri_any(const GinTernaryValue *check, int32 n){ GinTernaryValue r = GIN_FALSE; for (int i = 0; i < n; i++) { if (check[i] == GIN_TRUE) return GIN_TRUE; if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } return r; }
static bool bool_all(const bool *check, int32 n){ for (int i = 0; i < n; i++) if (!check[i]) return false; return true; }
static bool bool_any(const bool *check, int32 n){ for (int i = 0; i < n; i++) if (check[i]) return true; return false; }

PG_FUNCTION_INFO_V1(laplace_gin_extract_value);
Datum laplace_gin_extract_value(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); Geo g = geo_of(PG_GETARG_DATUM(0)); int m; lp_id *k = path_keys(&g, &m);
    Datum *d = palloc(sizeof(Datum) * (m ? m : 1)); for (int i = 0; i < m; i++) d[i] = PointerGetDatum(&k[i]);
    *n = m; PG_RETURN_POINTER(d);
}
PG_FUNCTION_INFO_V1(laplace_gin_extract_query);
Datum laplace_gin_extract_query(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); StrategyNumber st = PG_GETARG_UINT16(2); int32 *mode = (int32 *)PG_GETARG_POINTER(6);
    int k; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &k);
    Datum *d = palloc(sizeof(Datum) * (k ? k : 1)); for (int i = 0; i < k; i++) d[i] = PointerGetDatum(&ids[i]);
    *n = k; if (k == 0) *mode = st == LP_STRAT_CONTAINS ? GIN_SEARCH_MODE_ALL : GIN_SEARCH_MODE_DEFAULT;
    PG_RETURN_POINTER(d);
}
PG_FUNCTION_INFO_V1(laplace_gin_consistent);
Datum laplace_gin_consistent(PG_FUNCTION_ARGS){
    bool *check = (bool *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    *(bool *)PG_GETARG_POINTER(5) = false;                                   /* keys are the exact IDs: no recheck */
    PG_RETURN_BOOL(st == LP_STRAT_CONTAINS ? bool_all(check, n) : bool_any(check, n));
}
PG_FUNCTION_INFO_V1(laplace_gin_triconsistent);
Datum laplace_gin_triconsistent(PG_FUNCTION_ARGS){
    const GinTernaryValue *check = (const GinTernaryValue *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    PG_RETURN_GIN_TERNARY_VALUE(st == LP_STRAT_CONTAINS ? tri_all(check, n) : tri_any(check, n));
}

/* The operators themselves, for plans that do not use the index. */
static bool path_has(Datum path, ArrayType *q, bool all){ Geo g = geo_of(path); int k; lp_id *ids = ids_of(q, &k); return lp_path_holds(g.p, ids, (size_t)k, all); }
PG_FUNCTION_INFO_V1(laplace_path_contains);
Datum laplace_path_contains(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(path_has(PG_GETARG_DATUM(0), PG_GETARG_ARRAYTYPE_P(1), true)); }
PG_FUNCTION_INFO_V1(laplace_path_overlaps);
Datum laplace_path_overlaps(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(path_has(PG_GETARG_DATUM(0), PG_GETARG_ARRAYTYPE_P(1), false)); }

/* How many times a path holds each of the given IDs, runs included: one pass over the path, a binary search per vertex.
 * Returns only the IDs it holds. */
PG_FUNCTION_INFO_V1(laplace_path_times);
Datum laplace_path_times(PG_FUNCTION_ARGS){
    FuncCallContext *fx;
    typedef struct { lp_id *id; uint64 *times; int n, i; } State;
    if (SRF_IS_FIRSTCALL()) {
        fx = SRF_FIRSTCALL_INIT(); MemoryContext old = MemoryContextSwitchTo(fx->multi_call_memory_ctx);
        Geo g = geo_of(PG_GETARG_DATUM(0)); int k; lp_id *q = ids_of(PG_GETARG_ARRAYTYPE_P(1), &k);
        State *s = palloc(sizeof(State)); s->id = q; s->n = (int)lp_ids_unique(q, (size_t)k); s->i = 0;
        s->times = palloc(sizeof(uint64) * (s->n ? s->n : 1)); lp_path_times(g.p, q, (size_t)s->n, s->times); fx->user_fctx = s;
        TupleDesc td; if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("laplace_path_times: composite result expected")));
        fx->tuple_desc = BlessTupleDesc(td);
        MemoryContextSwitchTo(old);
    }
    fx = SRF_PERCALL_SETUP(); State *s = fx->user_fctx;
    while (s->i < s->n && !s->times[s->i]) s->i++;
    if (s->i >= s->n) SRF_RETURN_DONE(fx);
    Datum v[2]; bool nl[2] = { false, false }; v[0] = id_datum(&s->id[s->i]); v[1] = Int64GetDatum((int64)s->times[s->i]); s->i++;
    SRF_RETURN_NEXT(fx, HeapTupleGetDatum(heap_form_tuple(fx->tuple_desc, v, nl)));
}

/* ---------------------------------------------------------------- GIN over the mask
 * A row's mask (Semantics: Claims, Masks) is 256 bits: what the row is (a claim, a record, a tuple, a file), and the
 * types it holds, by the highway's layout. Its keys are the positions of its set bits, so "claims that hold X" is
 * the intersection of X's posting list with the claim bit's, in the one index over (path, mask). */
#define LP_STRAT_HASBIT  1        /* mask ? int2 */
#define LP_STRAT_HASALL  2        /* mask ?& int2[] */
#define LP_STRAT_HASANY  3        /* mask ?| int2[] */
PG_FUNCTION_INFO_V1(laplace_mask_extract_value);
Datum laplace_mask_extract_value(PG_FUNCTION_ARGS){
    VarBit *m = PG_GETARG_VARBIT_P(0); int32 *n = (int32 *)PG_GETARG_POINTER(1);
    int len = VARBITLEN(m); const bits8 *b = VARBITS(m); Datum *k = palloc(sizeof(Datum) * (len ? len : 1)); int c = 0;
    for (int i = 0; i < len; i++) if (b[i >> 3] & (0x80 >> (i & 7))) k[c++] = Int16GetDatum((int16)i);
    *n = c; PG_RETURN_POINTER(k);
}
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
    bool *check = (bool *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    *(bool *)PG_GETARG_POINTER(5) = false;
    PG_RETURN_BOOL(st == LP_STRAT_HASANY ? bool_any(check, n) : bool_all(check, n));
}
PG_FUNCTION_INFO_V1(laplace_mask_triconsistent);
Datum laplace_mask_triconsistent(PG_FUNCTION_ARGS){
    const GinTernaryValue *check = (const GinTernaryValue *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    PG_RETURN_GIN_TERNARY_VALUE(st == LP_STRAT_HASANY ? tri_any(check, n) : tri_all(check, n));
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
/* A value's bit in a bank (manifest/banks.tsv): its frozen slot in the bank's list, by its content as text; for the
 * row's own bank, kind, the kinds by name. -1 when the bank does not hold it. */
PG_FUNCTION_INFO_V1(laplace_bank_bit);
Datum laplace_bank_bit(PG_FUNCTION_ARGS){
    char *bank = text_to_cstring(PG_GETARG_TEXT_PP(0)); text *t = PG_GETARG_TEXT_PP(1);
    const lp_bank *b = lp_highway_bank(highway(), bank); if (!b) ereport(ERROR, (errmsg("laplace: no bank \"%s\" (manifest/banks.tsv)", bank)));
    if (!b->list) { char *v = text_to_cstring(t); static const char *kinds[] = { "claim", "record", "tuple", "file" };
        for (int k = 0; k < 4; k++) if (!strcmp(v, kinds[k])) PG_RETURN_INT16(k); PG_RETURN_INT16(-1); }
    if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_INT16(-1);
    lp_ref r = trunk_of(t, NULL, 0, NULL); int64 slot = lp_highway_slot(highway(), b->list, &r.id);
    PG_RETURN_INT16(slot >= 0 && slot < b->width ? (int16)slot : -1);
}
/* The bank a type is a value of, and its bit there, by the type's ID; no row when it is a value of no bank. */
PG_FUNCTION_INFO_V1(laplace_bank_of);
Datum laplace_bank_of(PG_FUNCTION_ARGS){
    int32_t bit; const lp_bank *b = lp_highway_bank_of(highway(), (const lp_id *)PG_GETARG_POINTER(0), &bit);
    TupleDesc td; if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("laplace_bank_of: a record is returned")));
    if (!b) PG_RETURN_NULL();
    Datum v[4] = { CStringGetTextDatum(b->name), CStringGetTextDatum(b->group), CStringGetTextDatum(b->carrier), Int16GetDatum((int16)bit) }; bool nl[4] = { 0 };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(td), v, nl)));
}

/* ---------------------------------------------------------------- COUPLE, one coarse native operator
 * laplace_couple(occ, fan, refuse, shape, shape_n, keep): the coupling field of an admitted observation
 * (Sequence 20.2), in one call: bounded indexed set access through SPI, everything else native.
 *   strand       every claim holding an occurrence, refusals out before the fan (15.5): the claim's other end
 *   containment  every observation holding an occurrence (19.3), at most the fan an occurrence
 *   shape        the observed curves nearest the observation's own (19.6, 19.7): nominated by the GIN (what holds the
 *                occurrences) and the GiST (what lies nearest its centroid), each realized from its children's real
 *                coordinates and measured natively under the firmware's shape; the keep nearest, with their vertices
 * One row a response, its route kept apart (the field is typed state, not one scalar): route 0 strand, 1 containment,
 * 2 shape. occ: the occurrence it answers (1-based), 0 for shape. */
enum { ROUTE_STRAND = 0, ROUTE_CONTAIN = 1, ROUTE_SHAPE = 2 };
typedef struct { FunctionCallInfo fcinfo; Tuplestorestate *ts; TupleDesc td; Oid el; } Field;
static void emit(Field *f, const lp_id *id, int occ, int route, const lp_rating *r, const lp_id *via, const lp_id *rel, int tier, double distance, const lp_id *vert, int nvert){
    Datum o[11]; bool nl[11] = { 0 };
    o[0] = id_datum(id); o[1] = Int32GetDatum(occ); o[2] = Int16GetDatum(route);
    if (route == ROUTE_STRAND) { o[3] = Float8GetDatum(r->rating); o[4] = Float8GetDatum(r->deviation); o[5] = Float8GetDatum(r->volatility); o[6] = id_datum(via); o[7] = id_datum(rel); }
    else for (int k = 3; k <= 7; k++) { nl[k] = true; o[k] = 0; }
    o[8] = Int16GetDatum(tier);
    if (route == ROUTE_SHAPE) o[9] = Float8GetDatum(distance); else { nl[9] = true; o[9] = 0; }
    if (vert) o[10] = PointerGetDatum(id_array_of(f->el, vert, nvert)); else { nl[10] = true; o[10] = 0; }
    set_put(f->fcinfo, f->ts, f->td, o, nl);
}
/* Every ID's real coordinate, in one read: a map from ID to its four doubles, those with none left out. */
typedef struct { double x[4]; bool has; } Coord4;
static void coords_spi(Oid idarr, lp_idmap *at){
    size_t n = lp_idmap_count(at); if (!n) return;
    Datum a[1] = { PointerGetDatum(id_array_of(get_element_type(idarr), lp_idmap_keys(at), (int)n)) }; Oid t[1] = { idarr };
    if (SPI_execute_with_args("SELECT e.id, e.coord FROM entity e WHERE e.id = ANY($1)", 1, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: coordinates")));
    for (uint64 r = 0; r < SPI_processed; r++) {
        bool nl; Datum g = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl); if (nl) continue;
        Coord4 *c = lp_idmap_lookup(at, (const lp_id *)DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)));
        Geo gg = geo_of(g); if (!c || gg.p.n < 1) continue;
        memcpy(c->x, xyzm(&gg), 32); c->has = true;
    }
}
/* An observed curve: the entity, its constituents, and its distance from the observation's own. */
typedef struct { lp_id id; lp_id *v; int nv; double d; } Curve;
static int by_distance(const void *a, const void *b){ double x = ((const Curve *)a)->d, y = ((const Curve *)b)->d; return x < y ? -1 : x > y; }
typedef lp_vec(Curve) Curves;
/* A nominated curve, once: its constituents, when there are between 2 and 1024 of them. */
static void nominate(Curves *cv, lp_idmap *seen, const lp_id *id, Datum path){
    bool fresh; lp_idmap_put(seen, id, &fresh); if (!fresh) return;
    Geo g = geo_of(path); size_t n = lp_path_len(g.p); if (n < 2 || n > 1024) return;
    Curve *c = lp_vec_add(cv); c->id = *id; c->v = palloc(sizeof(lp_id) * n); c->nv = (int)lp_path_expand(g.p, c->v, n);
}
static double measure(int shape, double shape_n, const double *a, size_t na, const double *b, size_t nb){
    switch (shape) {
        case 1: return lp_frechet4_outliers(a, na, b, nb, (unsigned)shape_n);
        case 2: { size_t s; double d = lp_dtw4(a, na, b, nb, &s); return s ? d / (double)s : d; }
        case 3: return (double)lp_edr4(a, na, b, nb, shape_n);
        default: return lp_frechet4(a, na, b, nb);
    }
}
PG_FUNCTION_INFO_V1(laplace_couple);
Datum laplace_couple(PG_FUNCTION_ARGS){
    Field f = { fcinfo, NULL, NULL, InvalidOid }; f.ts = set_begin(fcinfo, "laplace_couple", &f.td);
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0); f.el = get_element_type(idarr);
    ArrayType *occa = PG_GETARG_ARRAYTYPE_P(0); int nocc; lp_id *occ = ids_of(occa, &nocc);
    int64 fan = PG_GETARG_INT64(1); int16 shape = PG_GETARG_INT16(3); double shape_n = PG_GETARG_FLOAT8(4); int32 keep = PG_GETARG_INT32(5);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_couple: SPI_connect")));
    Datum fanp1 = Int64GetDatum(fan + 1), fand = Int64GetDatum(fan);
    /* strands: every claim holding an occurrence, refusals out before the fan; the strand answers with its other end */
    { Datum a[3] = { PointerGetDatum(occa), fanp1, PG_GETARG_DATUM(2) }; Oid t[3] = { idarr, INT8OID, idarr };
      if (SPI_execute_with_args("SELECT u.i, c.path, c.rating, c.deviation, c.volatility FROM unnest($1) WITH ORDINALITY AS u(id, i) JOIN entity e ON e.id = u.id "
            "CROSS JOIN LATERAL (SELECT p.path, s.rating, s.deviation, s.volatility FROM physicality p JOIN consensus s ON s.claim = p.entity WHERE p.tier > e.tier AND p.path @> ARRAY[u.id] "
            "AND p.mask ? 0::smallint AND NOT laplace_middle_any(p.path, $3) LIMIT $2) c", 3, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: strands")));
      int *held = palloc0(sizeof(int) * (size_t)(nocc + 1));                /* an occurrence more than the fan holds is a hub: what the scan met first is not its strands */
      for (uint64 r = 0; r < SPI_processed; r++) { bool nl; int i = (int)DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)) - 1; if (i >= 0 && i < nocc) held[i]++; }
      for (uint64 r = 0; r < SPI_processed; r++) { HeapTuple tup = SPI_tuptable->vals[r]; TupleDesc d = SPI_tuptable->tupdesc; bool nl;
          int i = (int)DatumGetInt64(SPI_getbinval(tup, d, 1, &nl)) - 1; if (i < 0 || i >= nocc || held[i] > fan) continue;
          Geo g = geo_of(SPI_getbinval(tup, d, 2, &nl)); lp_id part[64]; size_t np = lp_path_expand(g.p, part, 64); if (np < 2 || np > 64) continue;
          int other = lp_tuple_other(part, np, &occ[i]); if (other < 0) continue;
          lp_rating rt = { DatumGetFloat8(SPI_getbinval(tup, d, 3, &nl)), DatumGetFloat8(SPI_getbinval(tup, d, 4, &nl)), DatumGetFloat8(SPI_getbinval(tup, d, 5, &nl)) };
          emit(&f, &part[other], i + 1, ROUTE_STRAND, &rt, &occ[i], np >= 3 ? &part[1] : &occ[i], 0, 0.0, NULL, 0); } }
    /* containment: what holds each occurrence, the paths kept as the shape's GIN nominations */
    lp_idmap *seen = lp_idmap_new(); Curves cv = { 0 };
    { Datum a[2] = { PointerGetDatum(occa), fanp1 }; Oid t[2] = { idarr, INT8OID };
      if (SPI_execute_with_args("SELECT u.i, c.entity, c.tier, c.path FROM unnest($1) WITH ORDINALITY AS u(id, i) JOIN entity e ON e.id = u.id "
            "CROSS JOIN LATERAL (SELECT p.entity, p.tier, p.path FROM physicality p WHERE p.tier > e.tier AND p.path @> ARRAY[u.id] AND NOT (p.mask ? 0::smallint) LIMIT $2) c", 2, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: containment")));
      int *held = palloc0(sizeof(int) * (size_t)(nocc + 1));                /* an occurrence more than the fan holds is a hub: reached, never crossed */
      for (uint64 r = 0; r < SPI_processed; r++) { bool nl; int i = (int)DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)) - 1; if (i >= 0 && i < nocc) held[i]++; }
      for (uint64 r = 0; r < SPI_processed; r++) { HeapTuple tup = SPI_tuptable->vals[r]; TupleDesc d = SPI_tuptable->tupdesc; bool nl;
          int i = (int)DatumGetInt64(SPI_getbinval(tup, d, 1, &nl)) - 1; if (i < 0 || i >= nocc || held[i] > fan) continue;
          lp_id id; memcpy(id.b, DatumGetPointer(SPI_getbinval(tup, d, 2, &nl)), 16);
          emit(&f, &id, i + 1, ROUTE_CONTAIN, NULL, NULL, NULL, DatumGetInt16(SPI_getbinval(tup, d, 3, &nl)), 0.0, NULL, 0);
          if (shape >= 0) nominate(&cv, seen, &id, SPI_getbinval(tup, d, 4, &nl)); } }
    if (shape >= 0 && keep > 0) {
        /* the observation's own curve, and its centroid for the GiST */
        lp_idmap *oc = lp_idmap_sized(sizeof(Coord4)); for (int i = 0; i < nocc; i++) lp_idmap_put(oc, &occ[i], NULL); coords_spi(idarr, oc);
        double *pa = palloc(sizeof(double) * 4 * (size_t)nocc), cen[4] = { 0 }; int npa = 0;
        for (int i = 0; i < nocc; i++) { const Coord4 *c = lp_idmap_lookup(oc, &occ[i]); if (c && c->has) memcpy(pa + 4 * npa++, c->x, 32); }
        lp_idmap_free(oc);
        for (int i = 0; i < npa; i++) for (int k = 0; k < 4; k++) cen[k] += pa[4 * i + k] / npa;
        if (npa >= 2) {
            /* the GiST's nominations: what lies nearest the centroid, at a tier that composes */
            uint8 pt[64]; size_t pl = lp_ewkb_point4(cen, pt, sizeof pt);
            Datum a[2] = { PointerGetDatum(bytes_datum(pt, pl)), fand }; Oid t[2] = { BYTEAOID, INT8OID };
            if (SPI_execute_with_args("SELECT e.id, p.path FROM (SELECT e.id, e.tier FROM entity e WHERE e.tier >= 3 ORDER BY e.coord <~> ST_GeomFromEWKB($1) LIMIT $2) e JOIN physicality p ON p.entity = e.id AND p.tier = e.tier", 2, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: nearest")));
            for (uint64 r = 0; r < SPI_processed; r++) { bool nl; lp_id id; memcpy(id.b, DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)), 16);
                nominate(&cv, seen, &id, SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl)); }
            /* every child's real coordinate, once; each curve realized and measured */
            lp_idmap *kc = lp_idmap_sized(sizeof(Coord4)); for (size_t i = 0; i < cv.n; i++) for (int k = 0; k < cv.v[i].nv; k++) lp_idmap_put(kc, &cv.v[i].v[k], NULL);
            coords_spi(idarr, kc);
            double *b = palloc(sizeof(double) * 4 * 1024);
            for (size_t i = 0; i < cv.n; i++) { int nb = 0;
                for (int k = 0; k < cv.v[i].nv; k++) { const Coord4 *c = lp_idmap_lookup(kc, &cv.v[i].v[k]); if (c && c->has) memcpy(b + 4 * nb++, c->x, 32); }
                cv.v[i].d = nb < 2 ? INFINITY : measure(shape, shape_n, pa, (size_t)npa, b, (size_t)nb); }
            lp_idmap_free(kc);
            lp_sort(cv.v, cv.n, sizeof(Curve), by_distance);
            for (size_t i = 0; i < cv.n && (int)i < keep; i++) if (isfinite(cv.v[i].d)) emit(&f, &cv.v[i].id, 0, ROUTE_SHAPE, NULL, NULL, NULL, 0, cv.v[i].d, cv.v[i].v, cv.v[i].nv);
        }
    }
    lp_idmap_free(seen); lp_vec_free(&cv);
    SPI_finish();
    PG_RETURN_NULL();
}
