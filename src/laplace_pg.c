/* Laplace-postgres: Laplace's 4D expansion of PostgreSQL and PostGIS.
 *
 * Standard geometry types stay as they are; these functions add what Laplace needs next to what PostGIS provides.
 * They read PostGIS's serialized geometry directly (a LINESTRING ZM's vertices are 32-byte X, Y, Z, M blocks, the same
 * block a Laplace-Native path is), and call Laplace-Native for everything: every reading of a path, every map, every
 * centroid. This module owns fmgr glue, blake3 identity, perf-cache mapping, and scalar 4D/highway functions.
 *
 * An ID is a BLAKE3 hash, 128 bits. It has a type of its own, blake3: 16 fixed bytes, written as 32 hexadecimal
 * digits, ordered and compared as bytes. */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "varatt.h"
#include "laplace/laplace.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include "laplace/geometry.h"
#include "laplace/consensus.h"
#include "pg_core.h"
#include "geo.h"

#define LP_STRING_(value) #value
#define LP_STRING(value) LP_STRING_(value)
#define LP_PG_VERSION LP_STRING(LP_PG_VERSION_MAJOR) "." LP_STRING(LP_PG_VERSION_MINOR)
PG_MODULE_MAGIC_EXT(.name = "laplace", .version = LP_PG_VERSION);

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
PG_FUNCTION_INFO_V1(blake3_from_bytea);
Datum blake3_from_bytea(PG_FUNCTION_ARGS){
    bytea *b = PG_GETARG_BYTEA_PP(0);
    if (VARSIZE_ANY_EXHDR(b) != 16) ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION), errmsg("a blake3 ID is 16 bytes (got %zu)", (size_t)VARSIZE_ANY_EXHDR(b))));
    uint8 *u = palloc(16); memcpy(u, VARDATA_ANY(b), 16); PG_RETURN_POINTER(u);
}
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
static char *tier0_profile = NULL, *highway_profile = NULL;
const lp_tier0_record *lp_pg_tier0(void){
    const char *path=GetConfigOption("laplace.tier0",false,false);
    const char *prof=GetConfigOption("laplace.tier0_profile",true,false);
    const lp_tier0_record *t = (prof && *prof)
        ? lp_tier0_map_branches(path, lp_tier0_parse_branches(prof))
        : lp_tier0_map(path);
    if (!t) ereport(ERROR, (errmsg("laplace: cannot map tier 0 at \"%s\" (laplace.tier0)", path)));
    return t;
}
lp_text *lp_pg_text_context(void){
    static lp_text *context;
    if (!context) {
        context = lp_text_new(lp_pg_tier0());
        if (!context) ereport(ERROR, (errmsg("laplace: cannot open ICU's break iterators")));
    }
    return context;
}
const lp_layout *lp_pg_flags(void){
    const char *path=GetConfigOption("laplace.flags",false,false);
    const char *prof=GetConfigOption("laplace.tier0_profile",true,false);
    const lp_layout *l = (prof && *prof)
        ? lp_flags_map_branches(path, lp_tier0_parse_branches(prof))
        : lp_flags_map(path);
    if (!l) ereport(ERROR, (errmsg("laplace: cannot map the flags at \"%s\" (laplace.flags)", path)));
    return l;
}
const lp_highway *lp_pg_highway(void){
    const char *path=GetConfigOption("laplace.highway",false,false);
    const char *prof=GetConfigOption("laplace.highway_profile",true,false);
    const lp_highway *h = (prof && *prof)
        ? lp_highway_map_branches(path, lp_highway_parse_branches(prof))
        : lp_highway_map(path);
    if (!h) ereport(ERROR, (errmsg("laplace: cannot map the highway at \"%s\" (laplace.highway; generate it with: laplace highway)", path)));
    return h;
}
/* Native's working memory (its maps, arrays, buffers) comes from the transaction's memory: what a call holds is freed by
 * the call, and whatever an error leaves behind goes with the transaction. */
static void *tx_grow(void *p, size_t n){ return p ? repalloc_huge(p, n) : MemoryContextAllocHuge(TopTransactionContext, n); }
static void tx_release(void *p){ pfree(p); }
void _PG_init(void);
static void cache_setting(const char *name,const char *description,char **value,const char *initial){
    /* Configuration belongs to the backend. Multiple loaded generations read
     * that owner instead of redefining a setting or retaining a private copy. */
    if(!GetConfigOption(name,true,false) || (GetConfigOptionFlags(name,true)&GUC_CUSTOM_PLACEHOLDER))
        DefineCustomStringVariable(name,description,NULL,value,initial,PGC_SUSET,GUC_ALLOW_IN_PARALLEL,NULL,NULL,NULL);
}
void _PG_init(void){
    lp_allocator(tx_grow, tx_release);
    cache_setting("laplace.tier0", "Path of the tier-0 perf-cache (1,114,112 codepoint records, 64 bytes each).", &tier0_path,lp_tier0_path());
    cache_setting("laplace.tier0_profile", "Tier-0 modular branch profile (ascii, utf, cjk, bmp, full; empty uses full or LAPLACE_TIER0_PROFILE).", &tier0_profile, "");
    cache_setting("laplace.flags", "Path of the flags that go with tier 0 (1,114,112 256-bit records; their layout beside them).", &flags_path,lp_flags_path());
    cache_setting("laplace.highway", "Path of the highway perf-cache (the types the resources list, and the mappings between them; its layout beside it).", &highway_path,lp_highway_path());
    cache_setting("laplace.highway_profile", "Highway domain profile (core, wordnet, frames, lexical, full; empty uses full or LAPLACE_HIGHWAY_PROFILE).", &highway_profile, "");
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
    if (!lp_text_id(lp_pg_text_context(), VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &id)) PG_RETURN_NULL();
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

/* PostGIS owns its private serialization. Ask its qualified EWKB exporter once
 * per value, then use Native's validated reader for every geometry family. */
bytea *lp_pg_path_export(Datum geometry){
    static Oid exporter;static uint64 generation;
    uint64 current=lp_pg_binding_generation();
    if (!OidIsValid(exporter) || generation!=current) {
        Oid schema = get_extension_schema(get_extension_oid("postgis", false));
        Oid type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("geometry"), ObjectIdGetDatum(schema));
        exporter = LookupFuncName(list_make2(makeString(get_namespace_name(schema)), makeString("st_asewkb")), 1, &type, false);
        generation=current;
    }
    return DatumGetByteaP(OidFunctionCall1(exporter, geometry));
}
bytea *lp_pg_path_ewkb(Datum geometry){
    bytea *ewkb = lp_pg_path_export(geometry);
    if (!lp_ewkb_visit((const uint8 *)VARDATA_ANY(ewkb), VARSIZE_ANY_EXHDR(ewkb), NULL, NULL, NULL))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: expected a complete ZM geometry")));
    return ewkb;
}
lp_id *lp_pg_path_keys(bytea *ewkb, int *n){
    size_t count;
    if (!lp_ewkb_keys((const uint8 *)VARDATA_ANY(ewkb), VARSIZE_ANY_EXHDR(ewkb), NULL, 0, &count)
        || count > MaxAllocSize / sizeof(lp_id) || count > INT_MAX - 2)
        ereport(ERROR, (errmsg("laplace: geometry has too many vertices")));
    lp_id *keys = palloc(sizeof(lp_id) * (count ? count : 1));
    if (!lp_ewkb_keys((const uint8 *)VARDATA_ANY(ewkb), VARSIZE_ANY_EXHDR(ewkb), keys, count, &count))
        ereport(ERROR, (errmsg("laplace: cannot read geometry vertices")));
    *n = (int)count; return keys;
}
PG_FUNCTION_INFO_V1(laplace_path_enrich_ewkb);
Datum laplace_path_enrich_ewkb(PG_FUNCTION_ARGS){
    bytea *a=lp_pg_path_ewkb(PG_GETARG_DATUM(0)), *b=lp_pg_path_ewkb(PG_GETARG_DATUM(1));
    lp_path recorded=lp_path_of((const uint8 *)VARDATA_ANY(a),VARSIZE_ANY_EXHDR(a));
    lp_path incoming=lp_path_of((const uint8 *)VARDATA_ANY(b),VARSIZE_ANY_EXHDR(b));
    size_t size=lp_ewkb_enrich_path(recorded,incoming,NULL,0);
    if (!size) PG_RETURN_NULL();
    if (size>MaxAllocSize-VARHDRSZ) ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("laplace: enriched path exceeds the geometry allocation limit")));
    bytea *out=palloc(VARHDRSZ+size); SET_VARSIZE(out,VARHDRSZ+size);
    lp_ewkb_enrich_path(recorded,incoming,(uint8 *)VARDATA(out),size);
    PG_RETURN_BYTEA_P(out);
}
bool lp_pg_path_end_span(void *context, const lp_ewkb_span *span){
    lp_pg_path_ends *ends = context; double vertex[4];
    if (!ends->found) { lp_ewkb_vertex(span, 0, vertex); lp_xyz_to_id(vertex, &ends->first); }
    lp_ewkb_vertex(span, span->count - 1, vertex); lp_xyz_to_id(vertex, &ends->last); ends->found = true;
    return true;
}
lp_pg_path_ends lp_pg_path_endpoints(bytea *ewkb){
    lp_pg_path_ends ends = {0};
    if (!lp_ewkb_visit((const uint8 *)VARDATA_ANY(ewkb), VARSIZE_ANY_EXHDR(ewkb), lp_pg_path_end_span, &ends, NULL))
        ereport(ERROR, (errmsg("laplace: cannot read geometry endpoints")));
    return ends;
}
PG_FUNCTION_INFO_V1(laplace_vertex_ids);
Datum laplace_vertex_ids(PG_FUNCTION_ARGS){ bytea *ewkb = lp_pg_path_ewkb(PG_GETARG_DATUM(0)); int m; lp_id *k = lp_pg_path_keys(ewkb, &m); PG_RETURN_ARRAYTYPE_P(id_array(fcinfo, k, m)); }

PG_FUNCTION_INFO_V1(laplace_path_length);
Datum laplace_path_length(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0)); size_t n = lp_path_len(g.p);
    if (n > PG_INT64_MAX) ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("laplace_path_length: run count exceeds bigint")));
    PG_RETURN_INT64((int64)n);
}
PG_FUNCTION_INFO_V1(laplace_path_parts);
Datum laplace_path_parts(PG_FUNCTION_ARGS){
    Geo g=geo_of(PG_GETARG_DATUM(0));size_t n=lp_path_len(g.p);
    if(n>INT_MAX || n>MaxAllocSize/sizeof(lp_id))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("laplace_parts: expanded path exceeds array capacity")));
    lp_id *ids=palloc(sizeof(*ids)*Max(n,(size_t)1));
    if(lp_path_expand(g.p,ids,n)!=n)ereport(ERROR,(errmsg("laplace_parts: incomplete path expansion")));
    ArrayType *result=id_array(fcinfo,ids,(int)n);pfree(ids);PG_RETURN_ARRAYTYPE_P(result);
}

/* One Native solve for a complete claim, not sequential updates of its previous
 * standing. Scores here are mean outcomes; containment's summed wins are
 * divided by its games by the SQL orchestrator. Native owns all rating math. */
PG_FUNCTION_INFO_V1(laplace_series);
Datum laplace_series(PG_FUNCTION_ARGS){
    ArrayType *arrays[4]; Datum *values[4]; bool *nulls[4]; int counts[4];
    const Oid types[4] = { InvalidOid, INT8OID, FLOAT8OID, FLOAT8OID };
    const int16 sizes[4] = { 16, 8, 8, 8 };
    const char alignments[4] = { TYPALIGN_CHAR, TYPALIGN_DOUBLE, TYPALIGN_DOUBLE, TYPALIGN_DOUBLE };
    for (int k = 0; k < 4; k++) {
        arrays[k] = PG_GETARG_ARRAYTYPE_P(k);
        if (ARR_NDIM(arrays[k]) > 1)
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("laplace_series: one-dimensional arrays required")));
        deconstruct_array(arrays[k], k ? types[k] : ARR_ELEMTYPE(arrays[k]), sizes[k], k != 0,
                          alignments[k], &values[k], &nulls[k], &counts[k]);
        if (k && counts[k] != counts[0])
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("laplace_series: array lengths must agree")));
    }
    if ((size_t)counts[0] > MaxAllocSize / sizeof(lp_series_entry))
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_series: too many observations")));
    lp_series_entry *entries = palloc(sizeof(*entries) * (counts[0] ? counts[0] : 1));
    size_t used = 0;
    for (int i = 0; i < counts[0]; i++) {
        for (int k = 0; k < 4; k++) if (nulls[k][i])
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_series: null observation or cyclic witness lineage")));
        int64 games = DatumGetInt64(values[1][i]);
        double score = DatumGetFloat8(values[2][i]), trust = DatumGetFloat8(values[3][i]);
        if (games == 0) continue;                                   /* an observation of no games says nothing */
        lp_id root; memcpy(root.b, DatumGetPointer(values[0][i]), sizeof root.b);
        /* the one law for an attestation row, signed trust included (lp_series_attested) */
        if (!isfinite(score) || score < 0 || score > 1
            || !lp_series_attested(root, (double)games, (double)games * score, trust, &entries[used]))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_series: invalid games, mean score or trust")));
        used++;
    }
    lp_standing standing;
    if (!lp_series(entries, used, &standing) || standing.matches > PG_INT64_MAX || standing.witnesses > PG_INT64_MAX)
        ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("laplace_series: invalid or non-convergent rooted series")));
    pfree(entries);
    TupleDesc td;
    if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_series: composite result expected")));
    Datum result[5] = { Float8GetDatum(standing.rating), Float8GetDatum(standing.deviation), Float8GetDatum(standing.volatility),
                        Int64GetDatum((int64)standing.matches), Int64GetDatum((int64)standing.witnesses) };
    bool missing[5] = {0};
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(td), result, missing)));
}

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
/* Identity, coordinate and Hilbert placement belong to the same UAX #29 trunk.
 * Flattening codepoints here changes the weights of words and sentences. */
static bool text_coord(text *t, lp_coord *out){
    const char *bytes=VARDATA_ANY(t); int length=VARSIZE_ANY_EXHDR(t);
    if (!length || !pg_verify_mbstr(PG_UTF8,bytes,length,true)) return false;
    lp_ref trunk=lp_text_decompose(lp_pg_text_context(),(const uint8_t *)bytes,(size_t)length,NULL,NULL);
    *out=trunk.c; return true;
}

PG_FUNCTION_INFO_V1(laplace_text_coord_ewkb);
Datum laplace_text_coord_ewkb(PG_FUNCTION_ARGS){ lp_coord c; if (!text_coord(PG_GETARG_TEXT_PP(0), &c)) PG_RETURN_NULL(); PG_RETURN_BYTEA_P(coord_ewkb(&c)); }

/* The stored Hilbert key: the Hilbert value with its top bit flipped, so bigint order is Hilbert order. */
PG_FUNCTION_INFO_V1(laplace_text_hilbert);
Datum laplace_text_hilbert(PG_FUNCTION_ARGS){ lp_coord c; if (!text_coord(PG_GETARG_TEXT_PP(0), &c)) PG_RETURN_NULL(); PG_RETURN_INT64(lp_hilbert_key(lp_hilbert4(&c))); }

PG_FUNCTION_INFO_V1(laplace_cp_coord_ewkb);
Datum laplace_cp_coord_ewkb(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); if (cp < 0 || (uint32)cp >= LP_NCP) PG_RETURN_NULL();
    lp_ref a = lp_ref_atom(lp_pg_tier0(), (uint32)cp); PG_RETURN_BYTEA_P(coord_ewkb(&a.c));
}

/* ---------------------------------------------------------------- the flags that go with tier 0
 * What the Unicode Standard says of a codepoint, from the memory-mapped flags: by the property's name and the value's
 * name as the standard writes them, short or as they are said. Nothing is read from a table. */
static const lp_field *field_of(text *t){
    char *n = text_to_cstring(t); const lp_field *f = lp_flags_field(lp_pg_flags(), n);
    if (!f) ereport(ERROR, (errmsg("laplace: the standard lists no property \"%s\"", n)));
    return f;
}
static int64 cp_of_id(const uint8 *u){ return lp_tier0_codepoint(lp_pg_tier0(), (const lp_id *)u); }
static Datum said(int64 cp, text *property){
    if (cp < 0 || cp >= LP_NCP) return (Datum)0;
    const lp_layout *l = lp_pg_flags(); const lp_field *f = field_of(property); uint32 v = lp_flags_get(l, (uint32)cp, f);
    return PointerGetDatum(cstring_to_text(f->nvalues ? l->value[f->first + v].say : v ? "Yes" : "No"));
}
static uint32 value_of(const lp_layout *l, const lp_field *f, text *value){
    char *vn = text_to_cstring(value); int32 want = lp_flags_value(l, f, vn);
    if (want < 0) ereport(ERROR, (errmsg("laplace: the standard lists no value \"%s\" for %s", vn, f->say)));
    return (uint32)want;
}
static int is(int64 cp, text *property, text *value){
    if (cp < 0 || cp >= LP_NCP) return -1;
    const lp_layout *l = lp_pg_flags(); const lp_field *f = field_of(property);
    return lp_flags_get(l, (uint32)cp, f) == value_of(l, f, value);
}
PG_FUNCTION_INFO_V1(laplace_cp_flags);
Datum laplace_cp_flags(PG_FUNCTION_ARGS){
    int32 cp = PG_GETARG_INT32(0); if (cp < 0 || (uint32)cp >= LP_NCP) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(bytes_datum(lp_pg_flags()->flags[cp].b, 32));
}
PG_FUNCTION_INFO_V1(laplace_cp_said);
Datum laplace_cp_said(PG_FUNCTION_ARGS){ Datum d = said(PG_GETARG_INT32(0), PG_GETARG_TEXT_PP(1)); if (!d) PG_RETURN_NULL(); return d; }
PG_FUNCTION_INFO_V1(laplace_said);
Datum laplace_said(PG_FUNCTION_ARGS){ Datum d = said(cp_of_id((const uint8 *)PG_GETARG_POINTER(0)), PG_GETARG_TEXT_PP(1)); if (!d) PG_RETURN_NULL(); return d; }
PG_FUNCTION_INFO_V1(laplace_cp_is);
Datum laplace_cp_is(PG_FUNCTION_ARGS){ int r = is(PG_GETARG_INT32(0), PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)); if (r < 0) PG_RETURN_NULL(); PG_RETURN_BOOL(r); }
PG_FUNCTION_INFO_V1(laplace_is);
Datum laplace_is(PG_FUNCTION_ARGS){ int r = is(cp_of_id((const uint8 *)PG_GETARG_POINTER(0)), PG_GETARG_TEXT_PP(1), PG_GETARG_TEXT_PP(2)); if (r < 0) PG_RETURN_NULL(); PG_RETURN_BOOL(r); }
/* Whether every codepoint of a text has the value: laplace_text_is(s, 'White_Space', 'Yes') is a text of white space
 * only. NULL for empty text, as for laplace_text_id. */
PG_FUNCTION_INFO_V1(laplace_text_is);
Datum laplace_text_is(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); const uint8 *s = (const uint8 *)VARDATA_ANY(t); size_t n = VARSIZE_ANY_EXHDR(t), i = 0;
    if (n == 0) PG_RETURN_NULL();
    const lp_layout *l = lp_pg_flags(); const lp_field *f = field_of(PG_GETARG_TEXT_PP(1)); uint32 want = value_of(l, f, PG_GETARG_TEXT_PP(2));
    while (i < n) {
        uint32_t cp; if (!lp_utf8_next(s, n, &i, &cp) || cp >= LP_NCP) PG_RETURN_NULL();
        if (lp_flags_get(l, cp, f) != want) PG_RETURN_BOOL(false);
    }
    PG_RETURN_BOOL(true);
}
PG_FUNCTION_INFO_V1(laplace_codepoint);
Datum laplace_codepoint(PG_FUNCTION_ARGS){ int64 cp = cp_of_id((const uint8 *)PG_GETARG_POINTER(0)); if (cp < 0) PG_RETURN_NULL(); PG_RETURN_INT32((int32)cp); }

/* ---------------------------------------------------------------- a text's entity, computed in place
 * The one decomposition of text (UAX #29, Laplace-Native), composed without recording: the ID, tier, coordinate and
 * constituents the engine gives the same text. A constant argument folds at plan time, so the query becomes an index
 * lookup on the result. */
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
Datum laplace_fingerprint(PG_FUNCTION_ARGS){ uint8 h[32]; lp_tier0_fingerprint(lp_pg_tier0(), h); PG_RETURN_TEXT_P(fingerprint_text(h)); }

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
    const lp_highway *h = lp_pg_highway(); const lp_list *l = list_of(h, PG_GETARG_TEXT_PP(0));
    text *t = PG_GETARG_TEXT_PP(1); if (VARSIZE_ANY_EXHDR(t) == 0) PG_RETURN_INT32(-1);
    lp_ref r = trunk_of(t, NULL, 0, NULL);
    PG_RETURN_INT32((int32)lp_highway_slot(h, l, &r.id));
}
PG_FUNCTION_INFO_V1(laplace_type_key);
Datum laplace_type_key(PG_FUNCTION_ARGS){
    const lp_highway *h = lp_pg_highway(); const lp_list *l = list_of(h, PG_GETARG_TEXT_PP(0));
    PG_RETURN_INT32((int32)lp_highway_key(h, l, text_to_cstring(PG_GETARG_TEXT_PP(1))));
}
PG_FUNCTION_INFO_V1(laplace_type_id);
Datum laplace_type_id(PG_FUNCTION_ARGS){
    const lp_highway *h = lp_pg_highway(); const lp_list *l = list_of(h, PG_GETARG_TEXT_PP(0));
    int32 slot = PG_GETARG_INT32(1); const lp_tier0_record *r = slot >= 0 ? lp_highway_at(h, l, (uint32)slot) : NULL;
    if (!r) PG_RETURN_NULL();
    return id_datum(&r->id);
}
PG_FUNCTION_INFO_V1(laplace_type_edges);
Datum laplace_type_edges(PG_FUNCTION_ARGS){
    const lp_highway *h = lp_pg_highway(); char *a = text_to_cstring(PG_GETARG_TEXT_PP(0)), *b = text_to_cstring(PG_GETARG_TEXT_PP(2)); int32 slot = PG_GETARG_INT32(1);
    const lp_edge *e; size_t n = slot >= 0 ? lp_highway_edges(h, a, (uint32)slot, b, &e) : 0;
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1)); for (size_t i = 0; i < n; i++) d[i] = Int32GetDatum((int32)e[i].to);
    PG_RETURN_ARRAYTYPE_P(construct_array(d, (int)n, INT4OID, 4, true, TYPALIGN_INT));
}
PG_FUNCTION_INFO_V1(laplace_highway_fingerprint);
Datum laplace_highway_fingerprint(PG_FUNCTION_ARGS){ uint8_t fp[32]; lp_highway_fingerprint(lp_pg_highway(), fp); PG_RETURN_TEXT_P(fingerprint_text(fp)); }

/* ---------------------------------------------------------------- observability */
PG_FUNCTION_INFO_V1(laplace_isa);
Datum laplace_isa(PG_FUNCTION_ARGS){
    char buf[256];
    snprintf(buf, sizeof buf, "cpu: %s; dispatch: %s", lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()));
    PG_RETURN_TEXT_P(cstring_to_text(buf));
}
