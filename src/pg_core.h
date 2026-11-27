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
#include "executor/tuptable.h"
#include "access/tupconvert.h"
#include "laplace/laplace.h"
#include "laplace/tier0.h"
#include "laplace/highway.h"
#include "laplace/perfcache.h"
#include "laplace/geometry.h"
#include "laplace/consensus.h"
#include "varatt.h"

const lp_tier0_record *lp_pg_tier0(void);
lp_text *lp_pg_text_context(void);
const lp_layout *lp_pg_flags(void);
const lp_highway *lp_pg_highway(void);
/* The chess perf-cache laplace.chess names; required: an error rather than NULL when none is configured or there. */
const lp_perfcache *lp_pg_chess(bool required);
static inline lp_mask lp_pg_mask_native(Datum mask){ return (lp_mask)DatumGetInt64(mask); }
lp_mask lp_pg_kind_mask(const char *role);
static inline Datum lp_pg_mask_sql(lp_mask value){ return Int64GetDatum((int64)value); }
bytea *lp_pg_path_export(Datum geometry);
bytea *lp_pg_path_ewkb(Datum geometry);
Datum lp_pg_geom_from_ewkb(const uint8 *ewkb, size_t len, int32 srid);
Datum lp_pg_path_enrich_datum(Datum recorded, Datum incoming);
lp_id *lp_pg_path_keys(bytea *ewkb, int *n);

typedef struct { lp_id first, last; bool found; } lp_pg_path_ends;
bool lp_pg_path_end_span(void *context, const lp_ewkb_span *span);
lp_pg_path_ends lp_pg_path_endpoints(bytea *ewkb);

/* Bind the installed extension, never the caller's search_path. PostgreSQL
 * catalog invalidation refreshes names after schema/extension changes. */
Oid lp_pg_schema(void);
Oid lp_pg_relation(const char *name);
uint64 lp_pg_binding_generation(void);
char *lp_pg_sql(const char *sql);

/* Complete ID-addressed path frontier. Native range routing and primary
 * indexes retain ascending ID order without sorting geometry in SQL. */
typedef void (*lp_pg_path_visit)(const lp_id *id,Datum path,Datum mask,void *context);
void lp_pg_lookup_paths(const lp_id *ids,size_t count,bool masks,lp_pg_path_visit visit,void *context);
/* Witness metadata uses the same snapshot/ACL/primary-index owner. A missing
 * lineage is NULL; missing witness IDs produce no callback. */
typedef void (*lp_pg_witness_visit)(const lp_id *id,const lp_id *lineage,double trust,void *context);
void lp_pg_lookup_witnesses(const lp_id *ids,size_t count,bool lineage,lp_pg_witness_visit visit,void *context);
void lp_pg_scan_all_witnesses(lp_pg_witness_visit visit,void *context);
/* Complete direct-container frontier through canonical GIN indexes, with
 * PostgreSQL bitmap visibility/lossy rechecks and ascending parent-ID visits. */
void lp_pg_lookup_containers(const lp_id *ids,size_t count,bool masks,bool assertions,lp_pg_path_visit visit,void *context);
/* Scan all canonical physicality partitions whose mask carries every bit in `bits`
 * via the composite GIN mask index (zero SPI, bitmap heap order, flat per-tuple memory). */
void lp_pg_scan_claims_by_mask(lp_mask bits, lp_pg_path_visit visit, void *context);
/* Assertion frontiers select actual CLAIM/RECORD vertices before acquiring
 * ordinary content parents. The path remains the authority for every edge. */
bool lp_pg_path_has_roles(Datum path,uint8 roles);
/* Registered witness roots, or an explicit subset; visit each stored path once. */
typedef void (*lp_pg_stored_path_visit)(const lp_id *id,lp_path path,Datum mask,void *context);
void lp_pg_walk_witnesses(ArrayType *roots,lp_pg_stored_path_visit visit,void *context);
/* Positive classifications (for example mapped Tier-0 IDs) are retained.
 * Only false entries are probed; missing stored IDs stay false. */
void lp_pg_present(const lp_id *ids,size_t count,bool *present);

typedef struct { Oid partition; lp_id id; int64 ordinal; } PresenceKey;
typedef struct { Oid partition; ItemPointerData tuple; } CarrierLocation;
typedef struct { FunctionCallInfo fcinfo; Tuplestorestate *output; TupleDesc descriptor; } PresenceOutput;
typedef void (*NativeKeyVisit)(TupleTableSlot *slot,const PresenceKey *key,void *context);
Oid lp_pg_enrichment_partition(Oid target, struct PartitionKeyData *key, struct PartitionDescData *partitions, Datum value);
void lp_pg_indexed_keys(Relation parent,const char *column,PresenceKey *keys,size_t count,NativeKeyVisit visit,void *context);
void lp_pg_acquired_rows(Relation parent,PresenceKey *keys,size_t count,lp_idmap *carriers,NativeKeyVisit visit,void *context,const bool *done);
Relation lp_pg_read_parent(Oid target,const char **column,const char *const *selected,int columns);
Relation lp_pg_presence_parent(Oid target,const char **column,int columns);
void lp_pg_presence_keys(FunctionCallInfo fcinfo,Tuplestorestate *output,TupleDesc descriptor,Relation parent,const char *column,PresenceKey *keys,int count);

typedef struct { lp_id claim, trunk; double games, tokens, score; uint32_t position; double trust; int witness, share_bp;
                 lp_id dependence; double source_trust, fisher; uint8_t has_dependence; } PgAttrRow;
typedef lp_vec(PgAttrRow) PgAttrRows;
typedef struct {
    double trust;                 /* first member: shared provenance lookup */
    lp_id lineage, root;
    uint8_t has_lineage, state;
} PgSeriesWitness;
typedef struct { lp_mask claim; MemoryContext context; } PgClaimBinding;
typedef struct { PgClaimBinding binding; lp_idmap *front; void *dag; size_t found; } PgClaimFrontier;

void lp_pg_claim_bind(PgClaimBinding *binding);
void lp_pg_claim_frontier_row(const lp_id *id, Datum path, Datum mask, void *context);
void lp_pg_witness_metadata(const lp_id *id, const lp_id *lineage, double trust, void *context);
void lp_pg_rollup_sources(PgAttrRows *rows);
void lp_pg_compute_attested_registry(const lp_id *claims, int n, Oid idarr, int witnessed_only,
                                     int include_files, const lp_idmap *roots,
                                     const lp_idmap *known_witnesses, PgAttrRows *out, lp_idmap **registry,
                                     void (*testimony)(const void *,const void *,const uint64_t *,size_t,double,void *), void *testimony_context);
void lp_pg_compute_attested_rows(const lp_id *claims, int n, Oid idarr, int witnessed_only,
                                 int include_files, const lp_idmap *roots, const lp_idmap *known_witnesses, PgAttrRows *out);
typedef struct {
    lp_id trunk, claim, dependence_root;
    double games, tokens, score, dependence_trust;
    int32 position;
    bool has_dependence;
} PgContainedSeriesRow;
typedef lp_vec(PgContainedSeriesRow) PgContainedSeriesRows;
void lp_pg_contained_series(const lp_id *roots, int nroots, PgContainedSeriesRows *out);
void lp_pg_read_attestation_rows(const lp_id *claims, int n, const lp_idmap *roots, PgAttrRows *out, lp_idmap *witnesses);
void lp_pg_read_attestation_stage(Oid selection_relid, PgAttrRows *out, lp_idmap *witnesses);
void lp_pg_attested_unstored(const lp_id *claims, int n, Oid idarr, int include_files, PgAttrRows *rows, lp_idmap *registry);

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
/* The generic overlap operator can stream large membership ranges. Sparse
 * stored-ID frontiers need exact entry seeks instead: their random BLAKE3
 * bounds otherwise cover almost the whole GIN dictionary. Native callers select
 * the access strategy explicitly, without limiting the input identities. */
#define LP_PG_GIN_RANGE_IDS (BLCKSZ / sizeof(lp_id))
/* Native ScanKeys can request exact postings without exposing a second SQL
 * operator. The support routines recognize this access strategy; its predicate
 * and lossy recheck are the ordinary overlap operation. */
#define LP_PG_GIN_EXACT_OVERLAPS 10
#define LP_PG_GIN_FIRST_ANY 11
#define LP_PG_GIN_LAST_ANY 12
#define LP_PG_GIN_BOTH_ANY 13

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

static inline ArrayType *id_array(FunctionCallInfo fcinfo, const lp_id *ids, int n){
    Oid el = get_element_type(get_fn_expr_rettype(fcinfo->flinfo));
    if (!OidIsValid(el)) ereport(ERROR, (errmsg("laplace: the function does not return an array of IDs")));
    return id_array_of(el, ids, n);
}
static inline bytea *bytes_datum(const uint8 *b, size_t n){
    bytea *r = palloc(VARHDRSZ + n); SET_VARSIZE(r, VARHDRSZ + n); memcpy(VARDATA(r), b, n); return r;
}
static inline bytea *coord_ewkb(const lp_coord *c){
    uint8 e[37]; lp_ewkb_coord(c, e, sizeof e); return bytes_datum(e, sizeof e);
}
static inline lp_ref trunk_of(text *t, lp_ref *parts, size_t cap, size_t *np){
    lp_ref one; size_t n;
    return lp_text_parts(lp_pg_text_context(), (const uint8_t *)VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), parts ? parts : &one, parts ? cap : 1, np ? np : &n);
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

static inline TupleConversionMap *lp_pg_tuple_conversion(TupleDesc child, TupleDesc parent){
    if (child->natts == parent->natts) {
        bool same = true;
        for (int i = 0; i < parent->natts; i++) {
            Form_pg_attribute ca = TupleDescAttr(child, i), pa = TupleDescAttr(parent, i);
            if (ca->attisdropped != pa->attisdropped ||
                (!pa->attisdropped && (ca->atttypid != pa->atttypid || strcmp(NameStr(ca->attname), NameStr(pa->attname)) != 0))) {
                same = false; break;
            }
        }
        if (same) return NULL;
    }
    return convert_tuples_by_name(child, parent);
}

/* Direct-AM canonical partition/index cache & batch access primitives (zero SPI). */
int lp_pg_canonical_partitions(const char *relname, const Oid **parts, const Oid **pks, const Oid **aux, const Oid **aux2);
int lp_pg_canonical_tier_bounds(const char *relname, const int16 **min_tiers, const int16 **max_tiers);
typedef void (*lp_pg_entity_coord_visit)(int64 ordinal, const lp_id *id, int16 tier, Datum coord, int64 hilbert, void *context);
void lp_pg_lookup_entity_coords(const lp_id *ids, size_t count, lp_pg_entity_coord_visit visit, void *context);
void lp_pg_lookup_entity_tiers(const lp_id *ids, size_t count, int16 *tiers);
void lp_pg_read_attestation_pk(const lp_id *claims, int n, const lp_idmap *roots, PgAttrRows *out, lp_idmap *witnesses);

typedef struct { lp_id id; int16 tier; Datum coord; double distance; } PgKnnHit;
int lp_pg_knn4d(const double query[4], int k, bool angular, int16 min_tier, bool has_max_tier, int16 max_tier, const lp_id *exclude_id, PgKnnHit *out, MemoryContext result_ctx);

typedef struct { int64 ordinal; lp_id entity; Datum path; int16 tier; int16 kind; bool hub; } PgHolderHit;
typedef void (*lp_pg_holder_visit)(const PgHolderHit *hit, void *context);
void lp_pg_holders_direct(const lp_id *ids, int count, const lp_id *required, int nrequired,
                          bool per_part, int16 floor, int64 cap, int16 kinds,
                          lp_mask bits, int16 endpoint, lp_pg_holder_visit visit, void *context);

typedef struct {
    int64 i; lp_id entity; Datum path;
    double rating, deviation, volatility; int64 matches;
} PgClaimHit;
typedef void (*lp_pg_claim_visit)(const PgClaimHit *hit, void *context);
void lp_pg_claims_each_direct(const lp_id *ids, int count, int64 fan, lp_mask bits,
                              const lp_id *refuse, int nrefuse, int16 endpoint,
                              const lp_id *include, int ninclude, int64 arity,
                              const lp_id *opposite, int nopposite,
                              lp_pg_claim_visit visit, void *context);
void lp_pg_claims_each_scoped_direct(const lp_id *ids, int count, int64 fan, lp_mask bits,
                                     const lp_id *refuse, int nrefuse, int16 endpoint,
                                     const lp_id *include, int ninclude, int64 arity,
                                     const lp_id *opposite, int nopposite,
                                     bool has_scope, const lp_id *witnesses, int nwitnesses, Oid idarr,
                                     lp_pg_claim_visit visit, void *context);
void lp_pg_solve_scoped_standings(const lp_id *claims, int n, Oid idarr,
                                  const lp_id *witnesses, int nwitnesses, lp_idmap *out);
void lp_pg_filter_witness_entities(const lp_id *entities, size_t count,
                                   const lp_id *witnesses, int nwitnesses, bool *allowed);
void lp_pg_truncate_rel(Oid rel1, Oid rel2);

typedef struct PgSalience {
    int16 tier;
    int64 df;
    int64 total;
    double info_bits;
    double span_lift;
    double loss_contrast;
    double salience;
} PgSalience;

void lp_pg_salience(const lp_id *occ, int nocc, const lp_id *scope, int nscope, PgSalience *out);
#endif
