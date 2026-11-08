/* GIN over path geometry: membership and endpoint postings for 128-bit BLAKE3
 * constituents packed in 4D ZM vertices, plus streamed partial-match overlap. */
#include "pg_core.h"
#include "geo.h"
#include "access/gin.h"
#include "access/htup_details.h"
#include "access/stratnum.h"

#define LP_STRAT_CONTAINS 7        /* path @> blake3[] */
#define LP_STRAT_OVERLAPS 3        /* path && blake3[] */
#define LP_STRAT_FIRST 8           /* path ^@ blake3 */
#define LP_STRAT_LAST 9            /* path @^ blake3 */
enum { PATH_MEMBER, PATH_FIRST, PATH_LAST };

/* Roles are edge metadata, never entity identity. This predicate deliberately
 * reads the stored M values rather than inferring roles from a child's mask. */
bool lp_pg_path_has_roles(Datum path,uint8 roles){
    Geo geometry=geo_of(path);
    for(size_t i=0;i<geometry.p.n;i++){
        double m;memcpy(&m,geometry.p.v+(4*i+3)*sizeof(double),sizeof m);
        if(roles&(1u<<lp_m_said(m)))return true;
    }
    return false;
}

PG_FUNCTION_INFO_V1(laplace_path_has_roles);
Datum laplace_path_has_roles(PG_FUNCTION_ARGS){
    int16 roles=PG_GETARG_INT16(1);
    if(roles<0 || roles>UINT8_MAX)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native path role selection must be an 8-bit mask")));
    PG_RETURN_BOOL(lp_pg_path_has_roles(PG_GETARG_DATUM(0),(uint8)roles));
}

/* The role byte belongs to an index key, never to an entity's identity. Keeping
 * all 128 ID bits avoids collisions between membership and endpoint postings. */
static Datum path_key(const lp_id *id, uint8 role){
    uint8 bytes[17]; bytes[0] = role; memcpy(bytes + 1, id->b, 16);
    return PointerGetDatum(bytes_datum(bytes, sizeof bytes));
}

/* A large overlap query is one exact set match over the membership-key range,
 * rather than one entry-tree seek per input ID in every partition. The GIN
 * access method still owns pending lists, posting trees and concurrency.
 * https://www.postgresql.org/docs/18/gin.html#GIN-PARTIAL-MATCH */
typedef struct PathOverlapRegistry PathOverlapRegistry;
typedef struct {
    lp_idmap *ids;lp_id first,last;ArrayType *operand;size_t readers;
    PathOverlapRegistry *owner;
} PathOverlapSet;
struct PathOverlapRegistry { PathOverlapSet *last;MemoryContextCallback cleanup;uint64 builds; };
typedef struct { PathOverlapSet *set;MemoryContextCallback cleanup; } PathOverlapLease;
static PathOverlapRegistry *path_overlap_registry;

static void path_overlap_registry_reset(void *context){
    /* Native working allocations and operand copies belong to this same
     * transaction. Context teardown frees them, including after errors. */
    if(path_overlap_registry==context)path_overlap_registry=NULL;
}
static void path_overlap_set_free(PathOverlapSet *set){
    if(set->owner->last==set)set->owner->last=NULL;
    lp_idmap_free(set->ids);pfree(set->operand);pfree(set);
}
static void path_overlap_lease_release(void *context){
    PathOverlapSet *set=((PathOverlapLease *)context)->set;
    Assert(set->readers>0);set->readers--;
    if(!set->readers && set->owner->last!=set)path_overlap_set_free(set);
}
static PathOverlapSet *path_overlap_acquire(ArrayType *array){
    PathOverlapRegistry *registry=path_overlap_registry;
    if(!registry){
        registry=MemoryContextAllocZero(TopTransactionContext,sizeof *registry);
        registry->cleanup=(MemoryContextCallback){.func=path_overlap_registry_reset,.arg=registry};
        MemoryContextRegisterResetCallback(TopTransactionContext,&registry->cleanup);
        path_overlap_registry=registry;
    }
    PathOverlapSet *set=registry->last;
    /* Reuse input operands only, never rows or snapshot-dependent answers.
     * Exact owned bytes prove reuse across the partition scans. One idle set
     * is retained; overlapping scans hold leases until their key context ends. */
    if(!set || VARSIZE(set->operand)!=VARSIZE(array) || memcmp(set->operand,array,VARSIZE(array))){
        int count;lp_id *ids=ids_of(array,&count);
        if(set && !set->readers)path_overlap_set_free(set);
        set=MemoryContextAllocZero(TopTransactionContext,sizeof *set);set->owner=registry;
        set->operand=MemoryContextAlloc(TopTransactionContext,VARSIZE(array));memcpy(set->operand,array,VARSIZE(array));
        set->ids=lp_idmap_new();if(count)set->first=set->last=ids[0];
        for(int i=0;i<count;i++){
            lp_idmap_put(set->ids,ids+i,NULL);
            if(lp_id_cmp(ids+i,&set->first)<0)set->first=ids[i];
            if(lp_id_cmp(ids+i,&set->last)>0)set->last=ids[i];
        }
        pfree(ids);registry->last=set;registry->builds++;
        elog(DEBUG1,"Native GIN operand set: %d IDs; build " UINT64_FORMAT,count,registry->builds);
    }
    PathOverlapLease *lease=palloc(sizeof *lease);lease->set=set;set->readers++;
    lease->cleanup=(MemoryContextCallback){.func=path_overlap_lease_release,.arg=lease};
    MemoryContextRegisterResetCallback(CurrentMemoryContext,&lease->cleanup);
    return set;
}

PG_FUNCTION_INFO_V1(laplace_gin_compare_partial);
Datum laplace_gin_compare_partial(PG_FUNCTION_ARGS){
    PathOverlapSet *set=(PathOverlapSet *)PG_GETARG_POINTER(3);
    if(PG_GETARG_UINT16(2)!=LP_STRAT_OVERLAPS || !set)
        ereport(ERROR,(errmsg("laplace: invalid path GIN set match")));
    bytea *key=PG_GETARG_BYTEA_PP(1);
    if(VARSIZE_ANY_EXHDR(key)!=17)
        ereport(ERROR,(errcode(ERRCODE_INDEX_CORRUPTED),errmsg("laplace: invalid path GIN key")));
    const uint8 *bytes=(const uint8 *)VARDATA_ANY(key);
    if(bytes[0]>PATH_MEMBER)PG_RETURN_INT32(1);
    const lp_id *id=(const lp_id *)(bytes+1);
    if(lp_id_cmp(id,&set->last)>0)PG_RETURN_INT32(1);
    PG_RETURN_INT32(lp_idmap_find(set->ids,id)>=0?0:-1);
}

/* Whether every check is true (all) or any is; GIN's maybe stays maybe. */
static GinTernaryValue tri_all(const GinTernaryValue *check, int32 n){ GinTernaryValue r = GIN_TRUE; for (int i = 0; i < n; i++) { if (check[i] == GIN_FALSE) return GIN_FALSE; if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } return r; }
static GinTernaryValue tri_any(const GinTernaryValue *check, int32 n){ GinTernaryValue r = GIN_FALSE; for (int i = 0; i < n; i++) { if (check[i] == GIN_TRUE) return GIN_TRUE; if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } return r; }
static bool bool_all(const bool *check, int32 n){ for (int i = 0; i < n; i++) if (!check[i]) return false; return true; }
static bool bool_any(const bool *check, int32 n){ for (int i = 0; i < n; i++) if (check[i]) return true; return false; }

PG_FUNCTION_INFO_V1(laplace_gin_extract_value);
typedef struct { lp_id *ids; size_t count, capacity; lp_pg_path_ends ends; } GinPathKeys;
static bool gin_path_span(void *context, const lp_ewkb_span *span){
    GinPathKeys *keys = context;
    if (span->count > keys->capacity - keys->count) return false;
    for (size_t i = 0; i < span->count; i++) {
        double vertex[4]; lp_ewkb_vertex(span, i, vertex);
        lp_id *id = keys->ids + keys->count++;
        lp_xyz_to_id(vertex, id);
        if (!keys->ends.found) { keys->ends.first = *id; keys->ends.found = true; }
        keys->ends.last = *id;
    }
    return true;
}
Datum laplace_gin_extract_value(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1);
    bytea *ewkb = lp_pg_path_export(PG_GETARG_DATUM(0));
    /* Every admitted ZM vertex occupies 32 bytes. The serialized byte bound
     * allocates once without a counting walk; Native validates the complete
     * geometry while collecting membership and endpoints in source order. */
    size_t capacity = VARSIZE_ANY_EXHDR(ewkb) / 32;
    if (capacity > MaxAllocSize / sizeof(lp_id) || capacity > INT_MAX - 2)
        ereport(ERROR, (errmsg("laplace: geometry has too many vertices")));
    GinPathKeys keys = {.ids = palloc(sizeof(lp_id) * (capacity ? capacity : 1)), .capacity = capacity};
    if (!lp_ewkb_visit((const uint8 *)VARDATA_ANY(ewkb), VARSIZE_ANY_EXHDR(ewkb), gin_path_span, &keys, NULL))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: expected a complete ZM geometry")));
    int m = (int)lp_ids_unique(keys.ids, keys.count); lp_id *k = keys.ids;
    lp_pg_path_ends ends = keys.ends;
    Datum *d = palloc(sizeof(Datum) * (m + 2));
    for (int i = 0; i < m; i++) d[i] = path_key(&k[i], PATH_MEMBER);
    if (ends.found) { d[m++] = path_key(&ends.first, PATH_FIRST); d[m++] = path_key(&ends.last, PATH_LAST); }
    pfree(keys.ids); pfree(ewkb);
    *n = m; PG_RETURN_POINTER(d);
}

PG_FUNCTION_INFO_V1(laplace_gin_extract_query);
Datum laplace_gin_extract_query(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); StrategyNumber st = PG_GETARG_UINT16(2); int32 *mode = (int32 *)PG_GETARG_POINTER(6);
    if (st == LP_STRAT_FIRST || st == LP_STRAT_LAST) {
        Datum *d = palloc(sizeof(Datum));
        d[0] = path_key((const lp_id *)PG_GETARG_POINTER(0), st == LP_STRAT_FIRST ? PATH_FIRST : PATH_LAST);
        *n = 1; PG_RETURN_POINTER(d);
    }
    bool endpoint_set=st==LP_PG_GIN_FIRST_ANY || st==LP_PG_GIN_LAST_ANY || st==LP_PG_GIN_BOTH_ANY;
    if (st != LP_STRAT_CONTAINS && st != LP_STRAT_OVERLAPS && st != LP_PG_GIN_EXACT_OVERLAPS && !endpoint_set) ereport(ERROR, (errmsg("laplace: unknown path GIN strategy %u", st)));
    ArrayType *array=PG_GETARG_ARRAYTYPE_P(0);
    /* A page of identity operands selects a streamed key range. This changes
     * access mechanics only; all input IDs and exact endpoint roles remain. */
    if(st==LP_STRAT_OVERLAPS && (size_t)ArrayGetNItems(ARR_NDIM(array),ARR_DIMS(array))>=LP_PG_GIN_RANGE_IDS){
        PathOverlapSet *set=path_overlap_acquire(array);
        if(!lp_idmap_count(set->ids)){*n=0;PG_RETURN_POINTER(NULL);}
        bool **partial=(bool **)PG_GETARG_POINTER(3);*partial=palloc(sizeof(bool));(*partial)[0]=true;
        Pointer **extra=(Pointer **)PG_GETARG_POINTER(4);*extra=palloc(sizeof(Pointer));(*extra)[0]=(Pointer)set;
        Datum *d=palloc(sizeof(Datum));d[0]=path_key(&set->first,PATH_MEMBER);*n=1;
        PG_RETURN_POINTER(d);
    }
    int k;lp_id *ids=ids_of(array,&k);
    if(st==LP_PG_GIN_BOTH_ANY){
        uint32 nf=(uint32)k,nl=(uint32)k;const lp_id *first_ids=ids,*last_ids=ids;
        if(k>=1 && memcmp(ids[0].b,"\xff\xff\xff\xff\xff\xff\xff\xff",8)==0){
            memcpy(&nf,ids[0].b+8,4);memcpy(&nl,ids[0].b+12,4);
            first_ids=ids+1;last_ids=ids+1+nf;
        }
        int total=(int)(nf+nl);Datum *d=palloc(sizeof(Datum)*(total?total:1));
        for(uint32 i=0;i<nf;i++)d[i]=path_key(&first_ids[i],PATH_FIRST);
        for(uint32 i=0;i<nl;i++)d[nf+i]=path_key(&last_ids[i],PATH_LAST);
        pfree(ids);*n=total;if(!total)*mode=GIN_SEARCH_MODE_DEFAULT;
        PG_RETURN_POINTER(d);
    }
    uint8 role=st==LP_PG_GIN_FIRST_ANY?PATH_FIRST:st==LP_PG_GIN_LAST_ANY?PATH_LAST:PATH_MEMBER;
    Datum *d = palloc(sizeof(Datum) * (k ? k : 1)); for (int i = 0; i < k; i++) d[i] = path_key(&ids[i], role);
    pfree(ids);
    *n = k; if (k == 0) *mode = st == LP_STRAT_CONTAINS ? GIN_SEARCH_MODE_ALL : GIN_SEARCH_MODE_DEFAULT;
    PG_RETURN_POINTER(d);
}

PG_FUNCTION_INFO_V1(laplace_gin_consistent);
Datum laplace_gin_consistent(PG_FUNCTION_ARGS){
    bool *check = (bool *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    *(bool *)PG_GETARG_POINTER(5) = false; /* Exact key postings; page-level bitmap candidates still require rechecks. */
    PG_RETURN_BOOL(st == LP_STRAT_OVERLAPS || st >= LP_PG_GIN_EXACT_OVERLAPS ? bool_any(check, n) : bool_all(check, n));
}

PG_FUNCTION_INFO_V1(laplace_gin_triconsistent);
Datum laplace_gin_triconsistent(PG_FUNCTION_ARGS){
    const GinTernaryValue *check = (const GinTernaryValue *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    PG_RETURN_GIN_TERNARY_VALUE(st == LP_STRAT_OVERLAPS || st >= LP_PG_GIN_EXACT_OVERLAPS ? tri_any(check, n) : tri_all(check, n));
}

/* A scan can test millions of short paths against one large frontier. Build
 * its canonical Native ID set once, then search it for each path vertex.
 * Only Const/external Param arguments are query-stable; row-varying arrays
 * must be rebuilt. PostgreSQL owns the cache and its complete lifetime. */
typedef struct { lp_id *ids; size_t count; ArrayType *array; } PathQuery;
typedef struct PathScanQuery {
    struct PathScanQuery *next;
    MemoryContext context;
    int parameter;
    Oid type;
    PathQuery *query;
    MemoryContextCallback cleanup;
} PathScanQuery;
static PathScanQuery *path_scan_queries;
static void path_scan_query_reset(void *argument){
    PathScanQuery *query = argument;
    PathScanQuery **at = &path_scan_queries;
    while (*at && *at != query) at = &(*at)->next;
    if (*at) *at = query->next;
}
static PathScanQuery *path_scan_query(FunctionCallInfo fcinfo){
    Node *expr = fcinfo->flinfo->fn_expr;
    List *args;
    if (IsA(expr, OpExpr)) args = ((OpExpr *)expr)->args;
    else if (IsA(expr, FuncExpr)) args = ((FuncExpr *)expr)->args;
    else return NULL;
    if (!IsA(linitial(args), Var) || !IsA(lsecond(args), Param)) return NULL;
    Param *parameter = lsecond(args);
    if (parameter->paramkind != PARAM_EXTERN) return NULL;
    MemoryContext context = fcinfo->flinfo->fn_mcxt;
    for (PathScanQuery *query = path_scan_queries; query; query = query->next)
        if (query->context == context && query->parameter == parameter->paramid && query->type == parameter->paramtype)
            return query;
    PathScanQuery *query = MemoryContextAllocZero(context, sizeof *query);
    query->context = context; query->parameter = parameter->paramid; query->type = parameter->paramtype;
    query->cleanup.func = path_scan_query_reset; query->cleanup.arg = query;
    MemoryContextRegisterResetCallback(context, &query->cleanup);
    query->next = path_scan_queries; path_scan_queries = query;
    return query;
}
static bool path_query_scan(FunctionCallInfo fcinfo){
    Node *expr = fcinfo->flinfo->fn_expr;
    List *args;
    if (IsA(expr, OpExpr)) args = ((OpExpr *)expr)->args;
    else if (IsA(expr, FuncExpr)) args = ((FuncExpr *)expr)->args;
    else return false;
    /* A table scan owns its FmgrInfo for one executor lifetime. Standalone
     * procedural expressions can reuse theirs with changing external Params;
     * those still compare the complete original array below. */
    return IsA(lsecond(args), Const) || IsA(linitial(args), Var);
}
static bool path_has(FunctionCallInfo fcinfo, bool all){
    bool stable = get_fn_expr_arg_stable(fcinfo->flinfo, 1);
    bool scan_stable = stable && path_query_scan(fcinfo);
    PathQuery *query = stable ? fcinfo->flinfo->fn_extra : NULL;
    /* Partition scans share one external parameter and executor context.
     * Prepare its IDs once per executor, rather than retaining a separate
     * complete copy for every partition's expression. */
    PathScanQuery *shared = !query && scan_stable ? path_scan_query(fcinfo) : NULL;
    if (shared) query = shared->query;
    ArrayType *array = PG_GETARG_ARRAYTYPE_P(1);
    /* PL/pgSQL can retain FmgrInfo across executions with new parameters.
     * Compare the original array, not its sorted/deduplicated projection. */
    if (!query || (!scan_stable && (VARSIZE(array) != VARSIZE(query->array)
        || memcmp(array, query->array, VARSIZE(array))))) {
        MemoryContext previous = CurrentMemoryContext;
        if (stable) MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
        int count;
        lp_id *ids = ids_of(array, &count);
        size_t distinct = lp_ids_unique(ids, (size_t)count);
        ArrayType *copy = stable ? DatumGetArrayTypePCopy(PointerGetDatum(array)) : NULL;
        /* Publish only a complete validated replacement: a caught null-ID
         * error must leave an expression's previous cached set usable. */
        if (!query) query = palloc0(sizeof *query);
        else { pfree(query->ids); pfree(query->array); }
        *query = (PathQuery){ .ids=ids,.count=distinct,.array=copy };
        MemoryContextSwitchTo(previous);
    }
    if (stable) fcinfo->flinfo->fn_extra = query;
    if (shared) shared->query = query;
    PG_FREE_IF_COPY(array, 1);
    bytea *ewkb = lp_pg_path_ewkb(PG_GETARG_DATUM(0));
    int count; lp_id *keys = lp_pg_path_keys(ewkb, &count);
    size_t matched = 0;
    if (!all || query->count <= (size_t)count) {
        for (int i = 0; i < count; i++) {
            if (bsearch(keys + i, query->ids, query->count, sizeof(lp_id), lp_id_cmp)) {
                matched++;
                if (!all) break;
            }
        }
    }
    bool result = all ? matched == query->count : matched != 0;
    pfree(keys); pfree(ewkb);
    if (!stable) { pfree(query->ids); pfree(query); }
    return result;
}

PG_FUNCTION_INFO_V1(laplace_path_starts);
Datum laplace_path_starts(PG_FUNCTION_ARGS){ lp_pg_path_ends ends = lp_pg_path_endpoints(lp_pg_path_ewkb(PG_GETARG_DATUM(0))); PG_RETURN_BOOL(ends.found && lp_id_eq(&ends.first, (const lp_id *)PG_GETARG_POINTER(1))); }
PG_FUNCTION_INFO_V1(laplace_path_ends);
Datum laplace_path_ends(PG_FUNCTION_ARGS){ lp_pg_path_ends ends = lp_pg_path_endpoints(lp_pg_path_ewkb(PG_GETARG_DATUM(0))); PG_RETURN_BOOL(ends.found && lp_id_eq(&ends.last, (const lp_id *)PG_GETARG_POINTER(1))); }
PG_FUNCTION_INFO_V1(laplace_path_contains);
Datum laplace_path_contains(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(path_has(fcinfo, true)); }
PG_FUNCTION_INFO_V1(laplace_path_overlaps);
Datum laplace_path_overlaps(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(path_has(fcinfo, false)); }

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
