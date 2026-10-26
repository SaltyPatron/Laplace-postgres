/* Shared fmgr adapters; Native owns identity, geometry and text. */
#ifndef LAPLACE_PG_CORE_H
#define LAPLACE_PG_CORE_H
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "laplace/laplace.h"
#include "laplace/tier0.h"

const lp_tier0_record *lp_pg_tier0(void);
lp_text *lp_pg_text_context(void);

/* Complete ID-addressed path frontier. Native range routing and primary
 * indexes retain ascending ID order without sorting geometry in SQL. */
typedef void (*lp_pg_path_visit)(const lp_id *id,Datum path,Datum mask,void *context);
void lp_pg_lookup_paths(const lp_id *ids,size_t count,lp_pg_path_visit visit,void *context);

/* The live parameter buffers and the actual PostgreSQL array boundary size
 * every indexed frontier transfer. This is a window, never an input limit. */
static inline size_t frontier_window_ids(void)
{
    uint64 bytes = (uint64)work_mem * 1024;
    size_t overhead = ARR_OVERHEAD_NONULLS(1);
    size_t per_id = Max(sizeof(Datum) + sizeof(lp_id), 2 * sizeof(lp_id));
    size_t array_ids = (MaxAllocSize - overhead) / sizeof(lp_id);
    size_t budget_ids = bytes > overhead ? (size_t)((bytes - overhead) / per_id) : 0;
    return Max((size_t)1, Min(budget_ids, array_ids));
}

static inline Datum id_datum(const lp_id *id){ uint8 *u = palloc(16); memcpy(u, id->b, 16); return PointerGetDatum(u); }

static inline ArrayType *id_array_of(Oid el, const lp_id *ids, int n){
    Datum *d = palloc(sizeof(Datum) * (n ? n : 1));
    /* construct_array copies each fixed-width ID before returning. Borrow the
     * caller's immutable input instead of allocating and freeing every ID. */
    for (int i = 0; i < n; i++) d[i] = PointerGetDatum(ids + i);
    ArrayType *array = construct_array(d, n, el, 16, false, TYPALIGN_CHAR);
    pfree(d);
    return array;
}

static inline lp_id *ids_of(ArrayType *a, int *n){
    Datum *d; bool *nulls; deconstruct_array(a, ARR_ELEMTYPE(a), 16, false, TYPALIGN_CHAR, &d, &nulls, n);
    lp_id *ids = palloc(sizeof(lp_id) * (*n ? *n : 1));
    for (int i = 0; i < *n; i++) {
        if (nulls[i]) ereport(ERROR, (errmsg("laplace: IDs cannot be null")));
        memcpy(ids[i].b, DatumGetPointer(d[i]), 16);
    }
    if (d) pfree(d);
    if (nulls) pfree(nulls);
    return ids;
}

static inline Tuplestorestate *set_begin(FunctionCallInfo fcinfo, const char *name, TupleDesc *td){
    ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
    if (!rsi || !IsA(rsi, ReturnSetInfo) || !(rsi->allowedModes & SFRM_Materialize)) ereport(ERROR, (errmsg("%s: set-valued function called in a context that cannot accept a set", name)));
    if (get_call_result_type(fcinfo, NULL, td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("%s: composite result expected", name)));
    MemoryContext old = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    Tuplestorestate *ts = tuplestore_begin_heap(true, false, work_mem); *td = CreateTupleDescCopy(*td); MemoryContextSwitchTo(old);
    rsi->returnMode = SFRM_Materialize; rsi->setResult = ts; rsi->setDesc = *td;
    return ts;
}

static inline void set_put(FunctionCallInfo fcinfo, Tuplestorestate *ts, TupleDesc td, Datum *v, bool *nl){
    MemoryContext m = MemoryContextSwitchTo(((ReturnSetInfo *)fcinfo->resultinfo)->econtext->ecxt_per_query_memory);
    tuplestore_putvalues(ts, td, v, nl); MemoryContextSwitchTo(m);
}
#endif
