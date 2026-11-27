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
#include <sys/stat.h>
#include "laplace/laplace.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include "laplace/perfcache.h"
#include "laplace/geometry.h"
#include "laplace/consensus.h"
#include "pg_core.h"
#include "geo.h"
#include "access/genam.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_am_d.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_index.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "nodes/supportnodes.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

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
/* The chess perf-cache (laplace.chess). Not required: NULL when the setting is empty or no file is there. A file that is
 * there and does not load, is not a chess module, or was composed from another tier 0 is an error, never half used. */
static char *chess_path = NULL;
const lp_perfcache *lp_pg_chess(bool required){
    const char *path=GetConfigOption("laplace.chess",true,false);
    if (!path || !*path) {
        if (required) ereport(ERROR, (errcode(ERRCODE_CONFIG_FILE_ERROR), errmsg("laplace: no chess perf-cache is configured (laplace.chess)")));
        return NULL;
    }
    if (!required) { struct stat st; if (stat(path, &st) != 0) return NULL; }
    const lp_perfcache *c = lp_perfcache_map(path);
    if (!c) ereport(ERROR, (errcode(ERRCODE_CONFIG_FILE_ERROR), errmsg("laplace: cannot map the chess perf-cache at \"%s\" (laplace.chess; generate it with: laplace chess)", path)));
    if (strcmp(c->module, "chess")) ereport(ERROR, (errcode(ERRCODE_CONFIG_FILE_ERROR), errmsg("laplace: \"%s\" (laplace.chess) is a %s perf-cache module, not chess", path, c->module)));
    if (!lp_perfcache_bound(c, lp_pg_tier0())) ereport(ERROR, (errcode(ERRCODE_CONFIG_FILE_ERROR), errmsg("laplace: the chess perf-cache at \"%s\" was composed from another tier 0 (laplace.tier0): it is stale; generate it again with: laplace chess", path)));
    return c;
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
    static char chess_default[4096];
    cache_setting("laplace.chess", "Path of the chess perf-cache (chess's vocabulary and the admitted positions and moves; laplace chess writes it).", &chess_path,
                  lp_perfcache_beside_tier0("LAPLACE_CHESS", ".chess", chess_default, sizeof chess_default));
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
Datum lp_pg_geom_from_ewkb(const uint8 *ewkb, size_t len, int32 srid){
    static Oid importer, setsrid;
    static uint64 generation;
    uint64 current = lp_pg_binding_generation();
    if (!OidIsValid(importer) || generation != current) {
        Oid schema = get_extension_schema(get_extension_oid("postgis", false));
        char *nsp = get_namespace_name(schema);
        Oid bytea_type = BYTEAOID, int4_type = INT4OID;
        Oid geom_type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("geometry"), ObjectIdGetDatum(schema));
        importer = LookupFuncName(list_make2(makeString(nsp), makeString("st_geomfromewkb")), 1, &bytea_type, false);
        Oid set_args[2] = {geom_type, int4_type};
        setsrid = LookupFuncName(list_make2(makeString(nsp), makeString("st_setsrid")), 2, set_args, false);
        generation = current;
    }
    bytea *b = bytes_datum(ewkb, len);
    Datum g = OidFunctionCall1(importer, PointerGetDatum(b));
    pfree(b);
    if (srid != 0) g = OidFunctionCall2(setsrid, g, Int32GetDatum(srid));
    return g;
}
static int32 geom_srid_of(Datum geometry){
    static Oid getter;
    static uint64 generation;
    uint64 current = lp_pg_binding_generation();
    if (!OidIsValid(getter) || generation != current) {
        Oid schema = get_extension_schema(get_extension_oid("postgis", false));
        Oid geom_type = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("geometry"), ObjectIdGetDatum(schema));
        getter = LookupFuncName(list_make2(makeString(get_namespace_name(schema)), makeString("st_srid")), 1, &geom_type, false);
        generation = current;
    }
    return DatumGetInt32(OidFunctionCall1(getter, geometry));
}
static bytea *enrich_path_ewkb(Datum rec_geom, Datum inc_geom){
    bytea *a = lp_pg_path_ewkb(rec_geom), *b = lp_pg_path_ewkb(inc_geom);
    lp_path recorded = lp_path_of((const uint8 *)VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a));
    lp_path incoming = lp_path_of((const uint8 *)VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
    size_t size = lp_ewkb_enrich_path(recorded, incoming, NULL, 0);
    if (!size) { pfree(a); pfree(b); return NULL; }
    if (size > MaxAllocSize - VARHDRSZ)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace: enriched path exceeds the geometry allocation limit")));
    bytea *out = palloc(VARHDRSZ + size);
    SET_VARSIZE(out, VARHDRSZ + size);
    lp_ewkb_enrich_path(recorded, incoming, (uint8 *)VARDATA(out), size);
    pfree(a); pfree(b);
    return out;
}
Datum lp_pg_path_enrich_datum(Datum recorded, Datum incoming){
    if (geom_srid_of(recorded) != geom_srid_of(incoming)) return (Datum)0;
    bytea *ewkb = enrich_path_ewkb(recorded, incoming);
    if (!ewkb) return (Datum)0;
    Datum out = lp_pg_geom_from_ewkb((const uint8 *)VARDATA(ewkb), VARSIZE_ANY_EXHDR(ewkb), geom_srid_of(recorded));
    pfree(ewkb);
    return out;
}
PG_FUNCTION_INFO_V1(laplace_path_enrich_ewkb);
Datum laplace_path_enrich_ewkb(PG_FUNCTION_ARGS){
    bytea *out = enrich_path_ewkb(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1));
    if (!out) PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(out);
}
PG_FUNCTION_INFO_V1(laplace_path_enrich);
Datum laplace_path_enrich(PG_FUNCTION_ARGS){
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();
    Datum out = lp_pg_path_enrich_datum(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1));
    if (!out) PG_RETURN_NULL();
    PG_RETURN_DATUM(out);
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

typedef struct {
    lp_id start_id, end_id;
    int64 length;
    bool found;
} PathEndEntry;

static void path_end_visit(const lp_id *id, Datum path, Datum mask, void *context){
    (void)mask;
    PathEndEntry *e = lp_idmap_lookup((lp_idmap *)context, id);
    if (!e) return;
    Geo g = geo_of(path);
    if (g.p.n == 0) return;
    size_t len = lp_path_len(g.p);
    if (len > PG_INT64_MAX)
        ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("laplace_path_endpoints: run count exceeds bigint")));
    e->start_id = lp_path_id(g.p, 0);
    e->end_id = lp_path_id(g.p, g.p.n - 1);
    e->length = (int64)len;
    e->found = true;
}

PG_FUNCTION_INFO_V1(laplace_path_endpoints);
Datum laplace_path_endpoints(PG_FUNCTION_ARGS){
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_path_endpoints", &td);
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    int n = 0;
    lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (n <= 0) { pfree(ids); PG_RETURN_NULL(); }
    lp_idmap *by_id = lp_idmap_sized(sizeof(PathEndEntry));
    PG_TRY(); {
        for (int i = 0; i < n; i++) {
            bool fresh;
            PathEndEntry *e = lp_idmap_get(by_id, ids + i, &fresh);
            if (fresh) *e = (PathEndEntry){0};
        }
        lp_pg_lookup_paths(lp_idmap_keys(by_id), lp_idmap_count(by_id), false, path_end_visit, by_id);
        for (int i = 0; i < n; i++) {
            const PathEndEntry *e = lp_idmap_lookup(by_id, ids + i);
            if (!e || !e->found) continue;
            Datum v[5] = {
                Int64GetDatum((int64)i + 1),
                id_datum(ids + i),
                id_datum(&e->start_id),
                id_datum(&e->end_id),
                Int64GetDatum(e->length)
            };
            bool nl[5] = {false, false, false, false, false};
            set_put(fcinfo, ts, td, v, nl);
            pfree(DatumGetPointer(v[1]));
            pfree(DatumGetPointer(v[2]));
            pfree(DatumGetPointer(v[3]));
        }
    } PG_FINALLY(); {
        lp_idmap_free(by_id);
        pfree(ids);
    } PG_END_TRY();
    PG_RETURN_NULL();
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
/* Antecedents: the ID before every run of the phrase inside the path. */
PG_FUNCTION_INFO_V1(laplace_precedes);
Datum laplace_precedes(PG_FUNCTION_ARGS){
    Geo g = geo_of(PG_GETARG_DATUM(0)); int np; lp_id *phrase = ids_of(PG_GETARG_ARRAYTYPE_P(1), &np);
    size_t cap = g.p.n * 4 + 16; lp_id *out = palloc(sizeof(lp_id) * cap);
    size_t k = lp_path_precedes(g.p, phrase, (size_t)np, out, cap);
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

/* The decomposer's word segments of a text, in source order, whitespace and punctuation included (tailored UAX #29,
 * the segmentation laplace_id composes): each segment's surface and identity. The surfaces concatenate to the text,
 * byte for byte. Invalid or empty UTF-8 has no segments. */
typedef struct { FunctionCallInfo fcinfo; Tuplestorestate *ts; TupleDesc td; const char *base; int64 n; } SegmentSink;
static void segment_row(void *sink, size_t offset, size_t length, const lp_ref *value){
    SegmentSink *s = sink; Datum v[3]; bool nl[3] = { false, false, false };
    v[0] = Int64GetDatum(++s->n);
    v[1] = PointerGetDatum(cstring_to_text_with_len(s->base + offset, (int)length));
    v[2] = id_datum(&value->id);
    set_put(s->fcinfo, s->ts, s->td, v, nl);
}
PG_FUNCTION_INFO_V1(laplace_segments);
Datum laplace_segments(PG_FUNCTION_ARGS){
    text *t = PG_GETARG_TEXT_PP(0); SegmentSink s = { fcinfo, NULL, NULL, VARDATA_ANY(t), 0 };
    s.ts = set_begin(fcinfo, "laplace_segments", &s.td);
    int length = VARSIZE_ANY_EXHDR(t);
    if (length && pg_verify_mbstr(PG_UTF8, s.base, length, true))
        lp_text_segments(lp_pg_text_context(), (const uint8_t *)s.base, (size_t)length, segment_row, &s);
    PG_RETURN_NULL();
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

/* ---------------------------------------------------------------- the chess perf-cache, in place
 * The module laplace chess (Laplace-Engine) writes: chess's vocabulary by direct address and the admitted positions and
 * moves, each record an ID, its real coordinate, its slot and its tier (laplace/perfcache.h). Every lookup is a vector:
 * a request's IDs or slots in, a row for each that the module holds out, by its ordinal; a miss is no row and says
 * nothing about whether the content exists. The lookups resolve request values into canonical keys for an indexed
 * probe; they are never a predicate over a stored column. */
PG_FUNCTION_INFO_V1(laplace_chess_fingerprint);
Datum laplace_chess_fingerprint(PG_FUNCTION_ARGS){
    const lp_perfcache *c = lp_pg_chess(false); if (!c) PG_RETURN_NULL();
    uint8_t fp[32]; lp_perfcache_fingerprint(c, fp); PG_RETURN_TEXT_P(fingerprint_text(fp));
}
PG_FUNCTION_INFO_V1(laplace_chess_records_ewkb);
Datum laplace_chess_records_ewkb(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_chess_records", &td);
    const lp_perfcache *c = lp_pg_chess(true); int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    for (int i = 0; i < n; i++) {
        const lp_section *s; const lp_tier0_record *r = lp_perfcache_find(c, &ids[i], &s); if (!r) continue;
        lp_coord co; memcpy(co.m, r->m, sizeof co.m);
        Datum v[6] = { Int32GetDatum(i + 1), PointerGetDatum(cstring_to_text(s->name)), Int32GetDatum((int32)r->rank), Int32GetDatum((int32)r->pad),
                       PointerGetDatum(coord_ewkb(&co)), Int64GetDatum(lp_hilbert_key(r->hilbert)) };
        bool nl[6] = { 0 }; set_put(fcinfo, ts, td, v, nl);
    }
    pfree(ids); PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(laplace_chess_at);
Datum laplace_chess_at(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_chess_at", &td);
    const lp_perfcache *c = lp_pg_chess(true); char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const lp_section *s = lp_perfcache_section(c, name);
    if (!s || s->kind == LP_PERFCACHE_PAIRS) ereport(ERROR, (errmsg("laplace: the chess perf-cache has no section of records named \"%s\"", name)));
    Datum *d; bool *nulls; int n; deconstruct_array(PG_GETARG_ARRAYTYPE_P(1), INT4OID, 4, true, TYPALIGN_INT, &d, &nulls, &n);
    for (int i = 0; i < n; i++) {
        int32 slot = nulls[i] ? -1 : DatumGetInt32(d[i]); const lp_tier0_record *r = slot >= 0 ? lp_perfcache_at(c, s, (uint32)slot) : NULL;
        if (!r) continue;
        Datum v[2] = { Int32GetDatum(i + 1), id_datum(&r->id) }; bool nl[2] = { 0 }; set_put(fcinfo, ts, td, v, nl);
    }
    PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(laplace_chess_next);
Datum laplace_chess_next(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_chess_next", &td);
    const lp_perfcache *c = lp_pg_chess(true); const lp_section *s = lp_perfcache_section(c, "next");
    if (!s || s->kind != LP_PERFCACHE_PAIRS) ereport(ERROR, (errmsg("laplace: the chess perf-cache has no (position, move) -> position table")));
    int np, nm; lp_id *p = ids_of(PG_GETARG_ARRAYTYPE_P(0), &np), *m = ids_of(PG_GETARG_ARRAYTYPE_P(1), &nm);
    if (np != nm) ereport(ERROR, (errmsg("laplace_chess_next: %d positions and %d moves: one move for each position", np, nm)));
    for (int i = 0; i < np; i++) {
        const lp_pair *pair = lp_perfcache_pair(c, s, &p[i], &m[i]); if (!pair) continue;
        Datum v[2] = { Int32GetDatum(i + 1), id_datum(&pair->c) }; bool nl[2] = { 0 }; set_put(fcinfo, ts, td, v, nl);
    }
    pfree(p); pfree(m); PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- observability */
PG_FUNCTION_INFO_V1(laplace_isa);
Datum laplace_isa(PG_FUNCTION_ARGS){
    char buf[256];
    snprintf(buf, sizeof buf, "cpu: %s; dispatch: %s", lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()));
    PG_RETURN_TEXT_P(cstring_to_text(buf));
}

static void jb_key(JsonbParseState **st, const char *k) {
    JsonbValue v = { .type = jbvString };
    v.val.string.len = (int)strlen(k);
    v.val.string.val = pstrdup(k);
    pushJsonbValue(st, WJB_KEY, &v);
}

static void jb_str(JsonbParseState **st, const char *k, const char *s) {
    jb_key(st, k);
    if (!s) {
        JsonbValue n = { .type = jbvNull };
        pushJsonbValue(st, WJB_VALUE, &n);
        return;
    }
    JsonbValue v = { .type = jbvString };
    v.val.string.len = (int)strlen(s);
    v.val.string.val = pstrdup(s);
    pushJsonbValue(st, WJB_VALUE, &v);
}

static void jb_i64(JsonbParseState **st, const char *k, int64 val) {
    jb_key(st, k);
    JsonbValue v = { .type = jbvNumeric };
    v.val.numeric = int64_to_numeric(val);
    pushJsonbValue(st, WJB_VALUE, &v);
}

static void jb_bool(JsonbParseState **st, const char *k, bool val) {
    jb_key(st, k);
    JsonbValue v = { .type = jbvBool };
    v.val.boolean = val;
    pushJsonbValue(st, WJB_VALUE, &v);
}

typedef struct {
    int64 rows;
    int64 bytes;
    int64 parts;
    int64 total_indexes;
    int64 gin_indexes;
    int64 gist_indexes;
    int64 btree_indexes;
    int64 unindexed_parts;
} PartRelStats;

static void collect_canonical_stats(const char *relname, bool exact, int64 *tier_counts, PartRelStats *out) {
    memset(out, 0, sizeof(*out));
    const Oid *parts = NULL;
    int nparts = lp_pg_canonical_partitions(relname, &parts, NULL, NULL, NULL);
    out->parts = nparts;
    Snapshot snap = exact ? GetActiveSnapshot() : NULL;
    Relation pg_index = table_open(IndexRelationId, AccessShareLock);
    for (int p = 0; p < nparts; p++) {
        CHECK_FOR_INTERRUPTS();
        int32 relpages = 0;
        Oid toast_oid = InvalidOid;
        HeapTuple ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(parts[p]));
        if (HeapTupleIsValid(ctup)) {
            Form_pg_class c = (Form_pg_class)GETSTRUCT(ctup);
            relpages = c->relpages;
            toast_oid = c->reltoastrelid;
            if (!exact && c->reltuples > 0.0f)
                out->rows += (int64)c->reltuples;
            ReleaseSysCache(ctup);
        }
        int part_indexes = 0;
        ScanKeyData ikey;
        ScanKeyInit(&ikey, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(parts[p]));
        SysScanDesc iscan = systable_beginscan(pg_index, IndexIndrelidIndexId, true, NULL, 1, &ikey);
        HeapTuple idx_tup;
        while (HeapTupleIsValid(idx_tup = systable_getnext(iscan))) {
            Form_pg_index idx_form = (Form_pg_index)GETSTRUCT(idx_tup);
            if (!idx_form->indislive) continue;
            part_indexes++;
            out->total_indexes++;
            HeapTuple itup = SearchSysCache1(RELOID, ObjectIdGetDatum(idx_form->indexrelid));
            if (HeapTupleIsValid(itup)) {
                Form_pg_class ic = (Form_pg_class)GETSTRUCT(itup);
                Oid relam = ic->relam;
                if (relam == GIN_AM_OID) out->gin_indexes++;
                else if (relam == GIST_AM_OID) out->gist_indexes++;
                else if (relam == BTREE_AM_OID) out->btree_indexes++;
                if (!exact && ic->relpages > 0)
                    out->bytes += (int64)ic->relpages * BLCKSZ;
                ReleaseSysCache(itup);
            }
        }
        systable_endscan(iscan);
        if (part_indexes == 0) out->unindexed_parts++;
        if (!exact) {
            if (relpages > 0) out->bytes += (int64)relpages * BLCKSZ;
            if (OidIsValid(toast_oid)) {
                HeapTuple ttup = SearchSysCache1(RELOID, ObjectIdGetDatum(toast_oid));
                if (HeapTupleIsValid(ttup)) {
                    Form_pg_class tc = (Form_pg_class)GETSTRUCT(ttup);
                    if (tc->relpages > 0) out->bytes += (int64)tc->relpages * BLCKSZ;
                    ReleaseSysCache(ttup);
                }
            }
            continue;
        }
        out->bytes += DatumGetInt64(DirectFunctionCall1(pg_total_relation_size, ObjectIdGetDatum(parts[p])));
        Relation part = table_open(parts[p], AccessShareLock);
        if (RelationGetNumberOfBlocks(part) > 0) {
            AttrNumber a_tier = tier_counts ? get_attnum(parts[p], "tier") : InvalidAttrNumber;
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                out->rows++;
                if (tier_counts && a_tier > 0) {
                    bool isnull = true;
                    Datum td = slot_getattr(slot, a_tier, &isnull);
                    if (!isnull) {
                        int16 tier = DatumGetInt16(td);
                        if (tier >= 0 && tier <= 255) tier_counts[tier]++;
                    }
                }
                ExecClearTuple(slot);
            }
            table_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
        }
        table_close(part, NoLock);
    }
    table_close(pg_index, AccessShareLock);
}

static void count_witness_cb(const lp_id *id, const lp_id *lineage, double trust, void *ctx) {
    (void)id; (void)lineage; (void)trust;
    (*(int64 *)ctx)++;
}

PG_FUNCTION_INFO_V1(laplace_estate_summary);
Datum laplace_estate_summary(PG_FUNCTION_ARGS) {
    bool exact = PG_NARGS() > 0 && !PG_ARGISNULL(0) ? PG_GETARG_BOOL(0) : false;

    char *dbname = get_database_name(MyDatabaseId);
    bool read_only = XactReadOnly || RecoveryInProgress();

    char *extversion = NULL;
    Oid ext_oid = get_extension_oid("laplace", true);
    if (OidIsValid(ext_oid)) {
        HeapTuple etup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext_oid));
        if (HeapTupleIsValid(etup)) {
            bool isnull = true;
            Datum d = SysCacheGetAttr(EXTENSIONOID, etup, Anum_pg_extension_extversion, &isnull);
            if (!isnull) extversion = TextDatumGetCString(d);
            ReleaseSysCache(etup);
        }
    }

    char isa[256];
    snprintf(isa, sizeof isa, "cpu: %s; dispatch: %s", lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()));

    char *t0_fp = NULL, *hw_fp = NULL, *chess_fp = NULL;
    const char *t0_opt = GetConfigOption("laplace.tier0", true, false);
    if (t0_opt && *t0_opt) {
        uint8 fp[32];
        lp_tier0_fingerprint(lp_pg_tier0(), fp);
        t0_fp = text_to_cstring(fingerprint_text(fp));
    }
    const char *hw_opt = GetConfigOption("laplace.highway", true, false);
    if (hw_opt && *hw_opt) {
        uint8 fp[32];
        lp_highway_fingerprint(lp_pg_highway(), fp);
        hw_fp = text_to_cstring(fingerprint_text(fp));
    }
    const lp_perfcache *chess = lp_pg_chess(false);
    if (chess) {
        uint8 fp[32];
        lp_perfcache_fingerprint(chess, fp);
        chess_fp = text_to_cstring(fingerprint_text(fp));
    }

    int64 *tier_counts = exact ? palloc0(sizeof(int64) * 256) : NULL;
    PartRelStats ent_s, path_s, att_s, con_s;
    collect_canonical_stats("entity", exact, tier_counts, &ent_s);
    collect_canonical_stats("physicality", exact, NULL, &path_s);
    collect_canonical_stats("attestation", exact, NULL, &att_s);
    collect_canonical_stats("consensus", exact, NULL, &con_s);
    int64 db_bytes = exact
        ? DatumGetInt64(DirectFunctionCall1(pg_database_size_oid, ObjectIdGetDatum(MyDatabaseId)))
        : (ent_s.bytes + path_s.bytes + att_s.bytes + con_s.bytes);
    if (!exact && db_bytes <= 0)
        db_bytes = DatumGetInt64(DirectFunctionCall1(pg_database_size_oid, ObjectIdGetDatum(MyDatabaseId)));

    int64 witness_count = 0;
    lp_pg_scan_all_witnesses(count_witness_cb, &witness_count);

    int64 stage_tables = 0;
    Oid stage_nsp = get_namespace_oid("stage", true);
    if (OidIsValid(stage_nsp)) {
        Relation rel_class = table_open(RelationRelationId, AccessShareLock);
        ScanKeyData skey;
        ScanKeyInit(&skey, Anum_pg_class_relnamespace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(stage_nsp));
        SysScanDesc sscan = systable_beginscan(rel_class, InvalidOid, false, NULL, 1, &skey);
        HeapTuple ctup;
        while (HeapTupleIsValid(ctup = systable_getnext(sscan))) {
            Form_pg_class c = (Form_pg_class)GETSTRUCT(ctup);
            if (c->relkind == RELKIND_RELATION || c->relkind == RELKIND_PARTITIONED_TABLE)
                stage_tables++;
        }
        systable_endscan(sscan);
        table_close(rel_class, AccessShareLock);
    }

    JsonbParseState *st = NULL;
    pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
    jb_str(&st, "database", dbname);
    jb_bool(&st, "read_only", read_only);
    jb_i64(&st, "database_bytes", db_bytes);
    jb_str(&st, "count_kind", exact ? "exact" : "estimated");
    jb_str(&st, "extversion", extversion);
    jb_str(&st, "isa", isa);
    jb_str(&st, "tier0", t0_fp);
    jb_str(&st, "highway", hw_fp);
    jb_str(&st, "chess", chess_fp);
    jb_i64(&st, "entities", ent_s.rows);
    jb_i64(&st, "physicalities", path_s.rows);
    jb_i64(&st, "attestation_rows", att_s.rows);
    jb_i64(&st, "attestations", att_s.rows);
    jb_i64(&st, "standing_rows", con_s.rows);
    jb_i64(&st, "consensus", con_s.rows);
    jb_i64(&st, "registered_witness_rows", witness_count);
    jb_i64(&st, "entity_bytes", ent_s.bytes);
    jb_i64(&st, "physicality_bytes", path_s.bytes);
    jb_i64(&st, "attestation_bytes", att_s.bytes);
    jb_i64(&st, "consensus_bytes", con_s.bytes);
    jb_i64(&st, "entity_parts", ent_s.parts);
    jb_i64(&st, "physicality_parts", path_s.parts);
    jb_i64(&st, "attestation_parts", att_s.parts);
    jb_i64(&st, "consensus_parts", con_s.parts);
    jb_i64(&st, "entity_indexes", ent_s.total_indexes);
    jb_i64(&st, "entity_gist_indexes", ent_s.gist_indexes);
    jb_i64(&st, "physicality_indexes", path_s.total_indexes);
    jb_i64(&st, "physicality_gin_indexes", path_s.gin_indexes);
    jb_i64(&st, "unindexed_parts", ent_s.unindexed_parts + path_s.unindexed_parts + att_s.unindexed_parts + con_s.unindexed_parts);
    jb_i64(&st, "stage_tables", stage_tables);
    if (tier_counts) {
        jb_key(&st, "tiers");
        pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
        for (int t = 0; t <= 255; t++) {
            if (!tier_counts[t]) continue;
            char tkey[8];
            snprintf(tkey, sizeof tkey, "%d", t);
            jb_i64(&st, tkey, tier_counts[t]);
        }
        pushJsonbValue(&st, WJB_END_OBJECT, NULL);
        pfree(tier_counts);
    }
    JsonbValue *root = pushJsonbValue(&st, WJB_END_OBJECT, NULL);
    PG_RETURN_JSONB_P(JsonbValueToJsonb(root));
}

/* Planner SupportRequestRows hook so small ID arrays or page limits do not default to prorows=1000. */
PG_FUNCTION_INFO_V1(laplace_srf_support);
Datum laplace_srf_support(PG_FUNCTION_ARGS) {
    Node *rawreq = (Node *)PG_GETARG_POINTER(0);
    Node *ret = NULL;
    if (IsA(rawreq, SupportRequestRows)) {
        SupportRequestRows *req = (SupportRequestRows *)rawreq;
        if (req->node && IsA(req->node, FuncExpr)) {
            FuncExpr *fe = (FuncExpr *)req->node;
            if (list_length(fe->args) >= 1) {
                Node *arg0 = (Node *)linitial(fe->args);
                double est = 16.0;
                if (IsA(arg0, Const)) {
                    Const *c = (Const *)arg0;
                    if (c->constisnull) est = 1.0;
                    else if (c->consttype == TEXTOID) est = 8.0;
                    else if (get_element_type(c->consttype) != InvalidOid) {
                        ArrayType *a = DatumGetArrayTypeP(c->constvalue);
                        est = (double)ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
                        if (est < 1.0) est = 1.0;
                    }
                } else if (IsA(arg0, ArrayExpr)) {
                    est = (double)list_length(((ArrayExpr *)arg0)->elements);
                    if (est < 1.0) est = 1.0;
                }
                const char *fn = get_func_name(req->funcid);
                if (fn) {
                    if (strcmp(fn, "laplace_library_page") == 0 && list_length(fe->args) >= 2) {
                        Node *arg1 = (Node *)lsecond(fe->args);
                        if (IsA(arg1, Const) && !((Const *)arg1)->constisnull)
                            est = (double)DatumGetInt32(((Const *)arg1)->constvalue);
                        else
                            est = 64.0;
                    } else if (strcmp(fn, "laplace_below") == 0 || strcmp(fn, "laplace_containers") == 0 ||
                               strcmp(fn, "laplace_paths") == 0 || strcmp(fn, "laplace_contained") == 0) {
                        est *= 16.0;
                    } else if (strcmp(fn, "laplace_forward") == 0 || strcmp(fn, "laplace_forward_suffix") == 0 ||
                               strcmp(fn, "laplace_claims_each") == 0 || strcmp(fn, "laplace_fills") == 0) {
                        est *= 8.0;
                    } else if (strcmp(fn, "laplace_replay_export") == 0) {
                        est = 10000.0;
                    } else if (strcmp(fn, "laplace_hilbert_neighbors") == 0 && list_length(fe->args) >= 3) {
                        Node *arg2 = (Node *)lthird(fe->args);
                        if (IsA(arg2, Const) && !((Const *)arg2)->constisnull)
                            est = 2.0 * (double)DatumGetInt32(((Const *)arg2)->constvalue);
                        else
                            est = 32.0;
                    } else if (strcmp(fn, "laplace_constellation_sample") == 0) {
                        Node *a0 = (Node *)linitial(fe->args);
                        if (IsA(a0, Const) && !((Const *)a0)->constisnull)
                            est = (double)DatumGetInt32(((Const *)a0)->constvalue);
                        else
                            est = 64.0;
                    }
                }
                if (est < 1.0) est = 1.0;
                if (est > 1e7) est = 1e7;
                req->rows = est;
                ret = (Node *)req;
            }
        }
    }
    PG_RETURN_POINTER(ret);
}

static bool canonical_rel_nonempty(const char *relname, Snapshot snap)
{
    const Oid *parts = NULL;
    int nparts = lp_pg_canonical_partitions(relname, &parts, NULL, NULL, NULL);
    bool found = false;
    for (int p = 0; p < nparts && !found; p++) {
        CHECK_FOR_INTERRUPTS();
        Relation part = table_open(parts[p], AccessShareLock);
        TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
        TupleTableSlot *slot = table_slot_create(part, NULL);
        if (table_scan_getnextslot(scan, ForwardScanDirection, slot))
            found = true;
        ExecDropSingleTupleTableSlot(slot);
        table_endscan(scan);
        table_close(part, NoLock);
    }
    return found;
}

/* Direct-AM early-exit emptiness probe across entity and consensus partitions. */
PG_FUNCTION_INFO_V1(laplace_nonempty);
Datum laplace_nonempty(PG_FUNCTION_ARGS)
{
    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_nonempty requires composite result")));
    const char *ecols[] = { "id" }, *ccols[] = { "claim" }, *col = NULL;
    Relation ep = lp_pg_read_parent(lp_pg_relation("entity"), &col, ecols, 1);
    table_close(ep, NoLock);
    Relation cp = lp_pg_read_parent(lp_pg_relation("consensus"), &col, ccols, 1);
    table_close(cp, NoLock);

    Snapshot snap = GetActiveSnapshot();
    bool has_ent = canonical_rel_nonempty("entity", snap);
    bool has_con = canonical_rel_nonempty("consensus", snap);
    Datum vals[2] = { BoolGetDatum(has_ent), BoolGetDatum(has_con) };
    bool nulls[2] = { false, false };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), vals, nulls)));
}

/* Direct-AM uniform partition-stride tier distribution sample for status reporting. */
PG_FUNCTION_INFO_V1(laplace_entity_tier_sample);
Datum laplace_entity_tier_sample(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_entity_tier_sample", &td);
    Oid target = lp_pg_relation("entity");
    const char *cols[] = { "tier" }, *col = NULL;
    Relation ep = lp_pg_read_parent(target, &col, cols, 1);
    AttrNumber a_tier = get_attnum(target, "tier");
    table_close(ep, NoLock);

    const Oid *parts = NULL;
    int nparts = lp_pg_canonical_partitions("entity", &parts, NULL, NULL, NULL);
    double est_total = 0.0;
    for (int p = 0; p < nparts; p++) {
        HeapTuple ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(parts[p]));
        if (HeapTupleIsValid(ctup)) {
            Form_pg_class c = (Form_pg_class)GETSTRUCT(ctup);
            if (c->reltuples > 0.0f) est_total += c->reltuples;
            ReleaseSysCache(ctup);
        }
    }

    int step = (nparts >= 8 && est_total >= 100000.0) ? Max(1, nparts / 4) : 1;
    int64 counts[256] = {0};
    int sampled = 0;
    Snapshot snap = GetActiveSnapshot();
    for (int p = 0; p < nparts; p += step) {
        CHECK_FOR_INTERRUPTS();
        sampled++;
        Relation part = table_open(parts[p], AccessShareLock);
        TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
        TupleTableSlot *slot = table_slot_create(part, NULL);
        while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool isnull = true;
            Datum dt = slot_getattr(slot, a_tier, &isnull);
            if (!isnull) {
                int16 tier = DatumGetInt16(dt);
                if (tier >= 0 && tier <= 255) counts[tier]++;
            }
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);
        table_endscan(scan);
        table_close(part, NoLock);
    }

    double scale = sampled > 0 ? ((double)nparts / (double)sampled) : 1.0;
    for (int t = 0; t <= 255; t++) {
        if (!counts[t]) continue;
        Datum v[2] = { Int16GetDatum((int16)t), Int64GetDatum((int64)llround((double)counts[t] * scale)) };
        bool nl[2] = { false, false };
        set_put(fcinfo, ts, td, v, nl);
    }
    return (Datum)0;
}

static Oid find_tier_btree_index(Relation part, AttrNumber a_tier)
{
    List *idxs = RelationGetIndexList(part);
    ListCell *lc;
    Oid found = InvalidOid;
    foreach(lc, idxs) {
        Oid ioid = lfirst_oid(lc);
        Relation idx = index_open(ioid, AccessShareLock);
        if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index &&
            idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive &&
            idx->rd_index->indnkeyatts >= 1 && idx->rd_index->indkey.values[0] == a_tier) {
            found = ioid;
            index_close(idx, NoLock);
            break;
        }
        index_close(idx, NoLock);
    }
    list_free(idxs);
    return found;
}

/* Direct-AM exact entity count at target_tier using per-partition (tier, hilbert, id) B-trees. */
PG_FUNCTION_INFO_V1(laplace_entity_tier_count);
Datum laplace_entity_tier_count(PG_FUNCTION_ARGS)
{
    int16 target_tier = PG_GETARG_INT16(0);
    Oid target = lp_pg_relation("entity");
    const char *cols[] = { "tier" }, *col = NULL;
    Relation ep = lp_pg_read_parent(target, &col, cols, 1);
    AttrNumber a_tier = get_attnum(target, "tier");
    table_close(ep, NoLock);

    const Oid *parts = NULL;
    const int16 *min_t = NULL, *max_t = NULL;
    int nparts = lp_pg_canonical_partitions("entity", &parts, NULL, NULL, NULL);
    (void)lp_pg_canonical_tier_bounds("entity", &min_t, &max_t);
    Snapshot snap = GetActiveSnapshot();
    int64 total = 0;

    for (int p = 0; p < nparts; p++) {
        CHECK_FOR_INTERRUPTS();
        if (min_t && max_t && (target_tier < min_t[p] || target_tier > max_t[p]))
            continue;
        Relation part = table_open(parts[p], AccessShareLock);
        if (RelationGetNumberOfBlocks(part) == 0) {
            table_close(part, NoLock);
            continue;
        }
        AttrNumber p_tier = get_attnum(parts[p], "tier");
        Oid idx_oid = find_tier_btree_index(part, p_tier);
        TupleTableSlot *slot = table_slot_create(part, NULL);
        if (OidIsValid(idx_oid)) {
            Relation idx = index_open(idx_oid, AccessShareLock);
            ScanKeyData sk;
            ScanKeyEntryInitialize(&sk, 0, 1, BTEqualStrategyNumber, INT2OID,
                                   idx->rd_indcollation[0], F_INT2EQ, Int16GetDatum(target_tier));
            IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 1, 0);
            index_rescan(scan, &sk, 1, NULL, 0);
            while (index_getnext_slot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                total++;
                ExecClearTuple(slot);
            }
            index_endscan(scan);
            index_close(idx, NoLock);
        } else {
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool isnull = true;
                Datum dt = slot_getattr(slot, a_tier, &isnull);
                if (!isnull && DatumGetInt16(dt) == target_tier) total++;
                ExecClearTuple(slot);
            }
            table_endscan(scan);
        }
        ExecDropSingleTupleTableSlot(slot);
        table_close(part, NoLock);
    }
    PG_RETURN_INT64(total);
}
