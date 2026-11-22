/* Fisher-information basis-point royalty attribution and rooted series solving
 * (laplace_attribution, laplace_source_attribution, laplace_source_standings,
 * laplace_series_snapshot, laplace_accumulate_series, laplace_apply_series,
 * laplace_attest_series, laplace_attest_stage, laplace_measure_witness). */
#include "pg_core.h"
#include "geo.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "laplace/consensus.h"
#include <math.h>
#include <limits.h>

typedef struct {lp_id claim;lp_standing standing;} PgSolvedStanding;
typedef lp_vec(PgSolvedStanding) PgSolvedStandings;

static void compute_attribution_rows(const lp_id *claims,int n,Oid idarr,const lp_idmap *scope,PgAttrRows *out,PgSolvedStandings *solved);

PG_FUNCTION_INFO_V1(laplace_attribution);
Datum laplace_attribution(PG_FUNCTION_ARGS)
{
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_attribution", &td);
    int n; lp_id *claims = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (!n) PG_RETURN_NULL();
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0);
    PgAttrRows rows = {0};
    compute_attribution_rows(claims,n,idarr,NULL,&rows,NULL);
    for (size_t i = 0; i < rows.n; i++) {
        const PgAttrRow *r = &rows.v[i];
        Datum v[7] = {
            id_datum(&r->claim), id_datum(&r->trunk),
            Float8GetDatum(r->games), Float8GetDatum(r->tokens), Float8GetDatum(r->score),
            Float8GetDatum(r->trust), Int32GetDatum(r->share_bp)
        };
        bool nl[7] = {0};
        set_put(fcinfo, ts, td, v, nl);
    }
    lp_vec_free(&rows);
    PG_RETURN_NULL();
}

/* A formed batch owns one Native evidence context. SQL supplies indexed sets;
 * it does not recursively resolve witness ancestry or rebuild four arrays for
 * every claim. The same context also serves stage-resident changed claim sets. */
typedef struct { lp_id claim, witness, lineage; uint64_t games; double score, trust; uint8_t has_lineage; } PgSeriesObservation;
typedef struct {
    lp_idmap *claims, *witnesses, *queried;
    lp_strmap *pending, *pairs, *trusts;
    lp_vec(PgSeriesObservation) observations;
    lp_vec(lp_series_entry) entries;
    lp_vec(size_t) ancestry;
    lp_vec(lp_id) frontier;
    PgAttrRows historical;
} PgSeriesBatch;

static void pg_series_batch_free(void *arg)
{
    PgSeriesBatch *batch = arg;
    lp_idmap_free(batch->claims); lp_idmap_free(batch->witnesses);
    lp_idmap_free(batch->queried);lp_vec_free(&batch->frontier);
    lp_strmap_free(batch->pending); lp_strmap_free(batch->pairs); lp_strmap_free(batch->trusts);
    lp_vec_free(&batch->observations); lp_vec_free(&batch->entries);
    lp_vec_free(&batch->ancestry); lp_vec_free(&batch->historical);
}

static Oid pg_series_stage(Oid id, const char *column, Oid native_id, bool local_selection)
{
    Relation stage = table_open(id, AccessShareLock);
    bool stage_input = RelationGetNamespace(stage) == get_namespace_oid("stage", false)
        && stage->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED;
    bool local_input = local_selection && isTempNamespace(RelationGetNamespace(stage))
        && stage->rd_rel->relpersistence == RELPERSISTENCE_TEMP;
    AttrNumber att = get_attnum(id, column);
    if ((!stage_input && !local_input)
        || stage->rd_rel->relkind != RELKIND_RELATION
        || att == InvalidAttrNumber
        || get_atttype(id, att) != native_id)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("laplace_series_snapshot requires an UNLOGGED stage table with Native %s IDs", column)));
    List *inputs = find_all_inheritors(id, ShareLock, NULL);
    list_free(inputs);
    table_close(stage, NoLock);
    return id;
}

static void require_stage_kind(Oid id, bool allow_temp, const char *err_msg)
{
    Relation rel = table_open(id, AccessShareLock);
    Oid ns = RelationGetNamespace(rel);
    bool stage_ok = (ns == get_namespace_oid("stage", true) && rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED);
    bool temp_ok = (allow_temp && isTempNamespace(ns) && rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP);
    bool ok = (rel->rd_rel->relkind == RELKIND_RELATION) && (stage_ok || temp_ok);
    table_close(rel, NoLock);
    if (!ok)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s", err_msg)));
}

static void require_rel_priv(Oid relid, AclMode mode)
{
    if (pg_class_aclcheck(relid, GetUserId(), mode) != ACLCHECK_OK)
        aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)), get_rel_name(relid));
}

static void pg_series_invalid(void)
{
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
        errmsg("laplace_series_snapshot: invalid cumulative series, witness metadata or claim path")));
}

static lp_id pg_series_root(PgSeriesBatch *batch, lp_id root)
{
    batch->ancestry.n = 0;
    for (;;) {
        int64_t at = lp_idmap_find(batch->witnesses, &root);
        if (at < 0) break;
        PgSeriesWitness *witness = lp_idmap_at(batch->witnesses, (size_t)at);
        if (witness->state == 2) { root = witness->root; break; }
        if (witness->state == 1)
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_series: cyclic witness lineage")));
        witness->state = 1; lp_push(&batch->ancestry, (size_t)at);
        if (!witness->has_lineage || lp_id_eq(&root, &witness->lineage)) break;
        root = witness->lineage;
        CHECK_FOR_INTERRUPTS();
    }
    for (size_t i = 0; i < batch->ancestry.n; i++) {
        PgSeriesWitness *witness = lp_idmap_at(batch->witnesses, batch->ancestry.v[i]);
        witness->root = root; witness->state = 2;
    }
    return root;
}

/* Allocate only after the selected claim/source set has been reduced, delegating
 * exact 10000bp conservation and deterministic ID tie-breaking to Laplace-Native. */
static void pg_attribution_shares(PgAttrRow *rows,size_t n)
{
    if(!n)return;
    double *weights=palloc(n*sizeof *weights);
    lp_id *ids=palloc(n*sizeof *ids);
    int32_t *bp=palloc(n*sizeof *bp);
    for(size_t i=0;i<n;i++){weights[i]=rows[i].fisher;ids[i]=rows[i].trunk;}
    if(!lp_fisher_shares_bp(weights,ids,n,bp)){
        pfree(bp);pfree(ids);pfree(weights);
        pg_series_invalid();
    }
    for(size_t i=0;i<n;i++)rows[i].share_bp=bp[i];
    pfree(bp);pfree(ids);pfree(weights);
}

/* Hydrate only the selected witnesses' missing ancestors. Symbolic roots need
 * no registry row; a visited set also makes missing parents one indexed probe. */
static void pg_attribution_ancestors(PgSeriesBatch *batch)
{
    batch->queried=lp_idmap_new();size_t scanned=0;bool first=true;
    for(;;){
        batch->frontier.n=0;size_t count=lp_idmap_count(batch->witnesses);
        if(first){
            for(size_t i=0;i<batch->historical.n;i++){
                const PgAttrRow *r=batch->historical.v+i;bool fresh;
                if(r->has_dependence&&lp_idmap_find(batch->witnesses,&r->dependence)<0){
                    lp_idmap_put(batch->queried,&r->dependence,&fresh);if(fresh)lp_push(&batch->frontier,r->dependence);
                }
            }
            first=false;
        }
        for(;scanned<count;scanned++){
            const PgSeriesWitness *w=lp_idmap_at(batch->witnesses,scanned);
            bool fresh;
            if(w->has_lineage&&lp_idmap_find(batch->witnesses,&w->lineage)<0){
                lp_idmap_put(batch->queried,&w->lineage,&fresh);if(fresh)lp_push(&batch->frontier,w->lineage);
            }
        }
        if(!batch->frontier.n)break;
        lp_pg_lookup_witnesses(batch->frontier.v,batch->frontier.n,true,lp_pg_witness_metadata,batch->witnesses);
        CHECK_FOR_INTERRUPTS();
    }
}
static int pg_information_order(const void *a,const void *b)
{
    return lp_id_cmp(a,&((const lp_series_information *)b)->dependence_root);
}

static void compute_attribution_rows(const lp_id *claims,int n,Oid idarr,const lp_idmap *scope,PgAttrRows *out,PgSolvedStandings *solved)
{
    MemoryContext caller=CurrentMemoryContext;
    MemoryContext context=AllocSetContextCreate(caller,"Native source attribution",ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);
    PgSeriesBatch *batch=palloc0(sizeof *batch);MemoryContextCallback *cleanup=palloc(sizeof *cleanup);
    *cleanup=(MemoryContextCallback){.func=pg_series_batch_free,.arg=batch};
    MemoryContextRegisterResetCallback(context,cleanup);
    batch->witnesses=lp_idmap_sized(sizeof(PgSeriesWitness));
    lp_pg_read_attestation_rows(claims,n,scope,&batch->historical,batch->witnesses);
    lp_pg_attested_unstored(claims,n,idarr,0,&batch->historical,batch->witnesses);
    PgAttrRows *rows=&batch->historical;size_t kept=0;
    for(size_t i=0;i<rows->n;i++)if(rows->v[i].witness&&(!scope||lp_idmap_find(scope,&rows->v[i].trunk)>=0))
        rows->v[kept++]=rows->v[i];
    rows->n=kept;
    pg_attribution_ancestors(batch);
    for(size_t i=0;i<rows->n;){
        CHECK_FOR_INTERRUPTS();size_t j=i+1;
        while(j<rows->n&&lp_id_eq(&rows->v[i].claim,&rows->v[j].claim))j++;
        batch->entries.n=0;
        for(size_t k=i;k<j;k++){
            const PgAttrRow *r=rows->v+k;const PgSeriesWitness *w=lp_idmap_lookup(batch->witnesses,&r->trunk);
            double trust=r->has_dependence?r->trust:w->trust;
            lp_id root=pg_series_root(batch,r->has_dependence?r->dependence:w->has_lineage?w->lineage:r->trunk);
            lp_series_entry entry;                            /* the one law for an attestation row (lp_series_attested) */
            if(!lp_series_attested(root,r->games,r->score,trust,&entry))pg_series_invalid();
            lp_push(&batch->entries,entry);
        }
        lp_series_information *information=solved?NULL:palloc((j-i)*sizeof *information);size_t written=0;lp_standing standing;
        lp_rating prior=lp_rating_stock();
        bool valid=solved?lp_series(batch->entries.v,batch->entries.n,&standing)
            :lp_series_evaluate(&prior,batch->entries.v,batch->entries.n,&standing,information,j-i,&written);
        if(!valid||standing.matches>PG_INT64_MAX){
            const lp_series_entry *e0=batch->entries.v;
            ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                errmsg("laplace attribution: invalid or non-convergent rooted series"),
                errdetail("errno=%d entries=%zu matches=%llu e0=(games=%llu wins=%.17g trust=%.17g wdev=%.17g opp=%.17g/%.17g)",
                    errno,batch->entries.n,(unsigned long long)standing.matches,
                    (unsigned long long)(batch->entries.n?e0->games:0),batch->entries.n?e0->wins:0.0,
                    batch->entries.n?e0->trust:0.0,batch->entries.n?e0->witness_deviation:0.0,
                    batch->entries.n?e0->opponent.rating:0.0,batch->entries.n?e0->opponent.deviation:0.0)));
        }
        if(solved)lp_push(solved,((PgSolvedStanding){rows->v[i].claim,standing}));
        else for(size_t k=i;k<j;k++){
            const lp_series_entry *entry=batch->entries.v+(k-i);
            const lp_series_information *root=bsearch(&entry->dependence_root,information,written,sizeof *information,pg_information_order);
            /* The Schur-complement curvature of a shared root is distributed
             * across its actual observations; it is never counted per copy. */
            rows->v[k].fisher=root?root->information*((double)entry->games/(double)root->games):0;
        }
        if(information)pfree(information);i=j;
    }
    if(solved){MemoryContextSwitchTo(caller);MemoryContextDelete(context);return;}
    lp_pg_rollup_sources(rows);
    for(size_t i=0;i<rows->n;){
        size_t j=i+1;while(j<rows->n&&lp_id_eq(&rows->v[i].claim,&rows->v[j].claim))j++;
        pg_attribution_shares(rows->v+i,j-i);i=j;
    }
    *out=*rows;memset(rows,0,sizeof *rows);
    MemoryContextSwitchTo(caller);MemoryContextDelete(context);
}

typedef struct { PgAttrRows rows;PgSolvedStandings solved;lp_idmap *scope,*sources;lp_vec(uint64_t) touched,games; } PgSourceAttribution;
static void pg_source_attribution_free(void *arg)
{
    PgSourceAttribution *batch=arg;lp_vec_free(&batch->rows);lp_idmap_free(batch->scope);
    lp_idmap_free(batch->sources);lp_vec_free(&batch->touched);lp_vec_free(&batch->games);
    lp_vec_free(&batch->solved);
}
PG_FUNCTION_INFO_V1(laplace_source_attribution);
Datum laplace_source_attribution(PG_FUNCTION_ARGS)
{
    TupleDesc td;Tuplestorestate *result=set_begin(fcinfo,"laplace_source_attribution",&td);
    MemoryContext caller=CurrentMemoryContext;
    MemoryContext context=AllocSetContextCreate(caller,"Native attribution output",ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);PgSourceAttribution *batch=palloc0(sizeof *batch);
    MemoryContextCallback *cleanup=palloc(sizeof *cleanup);
    *cleanup=(MemoryContextCallback){.func=pg_source_attribution_free,.arg=batch};MemoryContextRegisterResetCallback(context,cleanup);
    int n;lp_id *claims=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n);
    int ns;lp_id *selected=ids_of(PG_GETARG_ARRAYTYPE_P(1),&ns);
    if(ns){batch->scope=lp_idmap_new();for(int i=0;i<ns;i++)lp_idmap_put(batch->scope,selected+i,NULL);}
    PgAttrRows *rows=&batch->rows;
    compute_attribution_rows(claims,n,get_fn_expr_argtype(fcinfo->flinfo,0),batch->scope,rows,NULL);
    batch->sources=lp_idmap_sized(sizeof(PgAttrRow));
    for(size_t i=0;i<rows->n;i++){
        const PgAttrRow *r=rows->v+i;bool fresh;size_t at=lp_idmap_put(batch->sources,&r->trunk,&fresh);
        if(!isfinite(r->games)||r->games>9007199254740992.0||floor(r->games)!=r->games)pg_series_invalid();
        PgAttrRow *sum=lp_idmap_at(batch->sources,at);
        if(fresh){*sum=(PgAttrRow){.trunk=r->trunk};lp_push(&batch->touched,UINT64_C(0));lp_push(&batch->games,UINT64_C(0));}
        uint64_t games=(uint64_t)r->games;
        if(games>(uint64_t)PG_INT64_MAX-batch->games.v[at])pg_series_invalid();
        batch->games.v[at]+=games;sum->fisher+=r->fisher;batch->touched.v[at]++;
    }
    rows->n=0;
    for(size_t i=0;i<lp_idmap_count(batch->sources);i++)lp_push(rows,*(PgAttrRow *)lp_idmap_at(batch->sources,i));
    pg_attribution_shares(rows->v,rows->n);
    for(size_t i=0;i<rows->n;i++){
        PgAttrRow *r=rows->v+i;Datum values[5]={id_datum(&r->trunk),Int64GetDatum((int64)batch->touched.v[i]),
            Int64GetDatum((int64)batch->games.v[i]),Float8GetDatum(r->fisher),Int32GetDatum(r->share_bp)};
        bool missing[5]={0};set_put(fcinfo,result,td,values,missing);
    }
    MemoryContextSwitchTo(caller);MemoryContextDelete(context);PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_source_standings);
Datum laplace_source_standings(PG_FUNCTION_ARGS)
{
    TupleDesc td;Tuplestorestate *result=set_begin(fcinfo,"laplace_source_standings",&td);
    MemoryContext caller=CurrentMemoryContext;
    MemoryContext context=AllocSetContextCreate(caller,"Native scoped standings",ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);PgSourceAttribution *batch=palloc0(sizeof *batch);
    MemoryContextCallback *cleanup=palloc(sizeof *cleanup);
    *cleanup=(MemoryContextCallback){.func=pg_source_attribution_free,.arg=batch};MemoryContextRegisterResetCallback(context,cleanup);
    int n,ns;lp_id *claims=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n),*selected=ids_of(PG_GETARG_ARRAYTYPE_P(1),&ns);
    if(ns){batch->scope=lp_idmap_new();for(int i=0;i<ns;i++)lp_idmap_put(batch->scope,selected+i,NULL);}
    compute_attribution_rows(claims,n,get_fn_expr_argtype(fcinfo->flinfo,0),batch->scope,NULL,&batch->solved);
    for(size_t i=0;i<batch->solved.n;i++){
        const PgSolvedStanding *r=batch->solved.v+i;
        Datum values[5]={id_datum(&r->claim),Float8GetDatum(r->standing.rating),Float8GetDatum(r->standing.deviation),
            Float8GetDatum(r->standing.volatility),Int64GetDatum((int64)r->standing.matches)};
        bool missing[5]={0};set_put(fcinfo,result,td,values,missing);
    }
    MemoryContextSwitchTo(caller);MemoryContextDelete(context);PG_RETURN_NULL();
}

void lp_pg_solve_scoped_standings(const lp_id *claims,int n,Oid idarr,
                                  const lp_id *witnesses,int nwitnesses,lp_idmap *out)
{
    if(!out||n<=0||!claims||!witnesses||nwitnesses<=0)return;
    MemoryContext caller=CurrentMemoryContext;
    MemoryContext context=AllocSetContextCreate(caller,"Native scoped standings helper",ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);
    lp_idmap *scope=lp_idmap_new();
    for(int i=0;i<nwitnesses;i++)lp_idmap_put(scope,witnesses+i,NULL);
    PgSolvedStandings solved={0};
    compute_attribution_rows(claims,n,idarr,scope,NULL,&solved);
    lp_idmap_free(scope);
    MemoryContextSwitchTo(caller);
    for(size_t i=0;i<solved.n;i++){
        lp_standing *st=lp_idmap_get(out,&solved.v[i].claim,NULL);
        *st=solved.v[i].standing;
    }
    MemoryContextDelete(context);
}

static int pg_series_by_claim(const void *a, const void *b)
{
    return memcmp(((const PgSeriesObservation *)a)->claim.b, ((const PgSeriesObservation *)b)->claim.b, sizeof(lp_id));
}

static AttrNumber require_attnum(Oid relid, const char *col, Oid expected_type, bool *type_ok)
{
    AttrNumber att = get_attnum(relid, col);
    if (att == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("laplace: relation \"%s\" has no column \"%s\"", get_rel_name(relid), col)));
    if (OidIsValid(expected_type) && get_atttype(relid, att) != expected_type) {
        if (type_ok) *type_ok = false;
        else ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("laplace: relation \"%s\" column \"%s\" has unexpected type", get_rel_name(relid), col)));
    } else if (type_ok) {
        *type_ok = true;
    }
    return att;
}

static int64 read_int64_slot(TupleTableSlot *slot, AttrNumber att, Oid typ, bool *isnull)
{
    Datum v = slot_getattr(slot, att, isnull);
    if (*isnull) return 0;
    if (typ == INT8OID) return DatumGetInt64(v);
    if (typ == INT4OID) return (int64)DatumGetInt32(v);
    if (typ == INT2OID) return (int64)DatumGetInt16(v);
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: expected integer column")));
}

static double read_float8_slot(TupleTableSlot *slot, AttrNumber att, Oid typ, bool *isnull)
{
    Datum v = slot_getattr(slot, att, isnull);
    if (*isnull) return 0.0;
    if (typ == FLOAT8OID) return DatumGetFloat8(v);
    if (typ == FLOAT4OID) return (double)DatumGetFloat4(v);
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: expected floating-point column")));
}

static void record_series_observation(PgSeriesBatch *batch, const lp_id *claim, const lp_id *witness,
                                      bool has_lin, const lp_id *lineage, int64 games, double score, double trust)
{
    if (games <= 0 || games > INT64CONST(9007199254740992)) pg_series_invalid();
    if (!isfinite(score) || score < 0.0 || score > 1.0 || !isfinite(trust) || trust < -1.0 || trust > 1.0)
        pg_series_invalid();
    PgSeriesObservation o = {
        .claim = *claim,
        .witness = *witness,
        .games = (uint64_t)games,
        .score = score,
        .trust = trust,
        .has_lineage = has_lin ? 1 : 0
    };
    if (has_lin) o.lineage = *lineage;

    uint8_t key[49] = {0};
    memcpy(key, o.claim.b, 16);
    memcpy(key + 16, o.witness.b, 16);
    memcpy(key + 32, o.lineage.b, 16);
    key[48] = o.has_lineage;
    bool fresh = false;
    lp_strmap_put(batch->pending, key, sizeof key, &fresh);
    if (!fresh) pg_series_invalid();
    lp_strmap_put(batch->pairs, key, 32, NULL);
    double *prev_trust = lp_strmap_get(batch->trusts, key + 16, 33, &fresh);
    if (!fresh && *prev_trust != o.trust) pg_series_invalid();
    *prev_trust = o.trust;
    lp_push(&batch->observations, o);
}

static void cwl_key_pack(uint8_t key[49], const lp_id *c, const lp_id *w, bool has_lin, const lp_id *lin)
{
    memset(key, 0, 49);
    memcpy(key, c->b, 16);
    memcpy(key + 16, w->b, 16);
    if (has_lin) memcpy(key + 32, lin->b, 16);
    key[48] = has_lin ? 1 : 0;
}

static Oid find_cwl_index(Relation rel, AttrNumber ac, AttrNumber aw, AttrNumber al, bool require_unique)
{
    List *indexes = RelationGetIndexList(rel);
    Oid found = InvalidOid;
    ListCell *lc;
    foreach (lc, indexes) {
        Oid idxOid = lfirst_oid(lc);
        Relation idx = index_open(idxOid, AccessShareLock);
        bool match = idx->rd_rel->relam == BTREE_AM_OID
            && idx->rd_index->indisvalid
            && idx->rd_index->indnkeyatts == 3
            && idx->rd_index->indkey.values[0] == ac
            && idx->rd_index->indkey.values[1] == aw
            && idx->rd_index->indkey.values[2] == al
            && (!require_unique || (idx->rd_index->indisunique && idx->rd_index->indnullsnotdistinct))
            && RelationGetIndexPredicate(idx) == NIL;
        index_close(idx, AccessShareLock);
        if (match) { found = idxOid; break; }
    }
    list_free(indexes);
    return found;
}

static Oid find_claim_btree_index(Relation rel, AttrNumber ac)
{
    List *indexes = RelationGetIndexList(rel);
    Oid found = InvalidOid;
    ListCell *lc;
    foreach (lc, indexes) {
        Oid idxOid = lfirst_oid(lc);
        Relation idx = index_open(idxOid, AccessShareLock);
        bool match = idx->rd_rel->relam == BTREE_AM_OID
            && idx->rd_index->indisvalid
            && idx->rd_index->indnkeyatts >= 1
            && idx->rd_index->indkey.values[0] == ac
            && RelationGetIndexPredicate(idx) == NIL;
        index_close(idx, AccessShareLock);
        if (match) { found = idxOid; break; }
    }
    list_free(indexes);
    return found;
}

static void init_cwl_scankeys(ScanKeyData sk[3], Relation idx, Oid native_id,
                              const lp_id *claim, const lp_id *witness, bool has_lin, const lp_id *lin)
{
    RegProcedure p0 = get_opcode(get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber));
    RegProcedure p1 = get_opcode(get_opfamily_member(idx->rd_opfamily[1], native_id, native_id, BTEqualStrategyNumber));
    ScanKeyEntryInitialize(&sk[0], 0, 1, BTEqualStrategyNumber, native_id, idx->rd_indcollation[0], p0, PointerGetDatum(claim));
    ScanKeyEntryInitialize(&sk[1], 0, 2, BTEqualStrategyNumber, native_id, idx->rd_indcollation[1], p1, PointerGetDatum(witness));
    if (has_lin) {
        RegProcedure p2 = get_opcode(get_opfamily_member(idx->rd_opfamily[2], native_id, native_id, BTEqualStrategyNumber));
        ScanKeyEntryInitialize(&sk[2], 0, 3, BTEqualStrategyNumber, native_id, idx->rd_indcollation[2], p2, PointerGetDatum(lin));
    } else {
        ScanKeyEntryInitialize(&sk[2], SK_ISNULL | SK_SEARCHNULL, 3, BTEqualStrategyNumber, InvalidOid, InvalidOid, InvalidOid, (Datum)0);
    }
}

typedef struct {
    Relation rel;
    TupleDesc td;
    EState *estate;
    ResultRelInfo *rri;
    TupleTableSlot *old_slot;
    TupleTableSlot *new_slot;
    ExprState **def_states;
    CommandId cid;
    Snapshot snap;
} PgRelWriter;

static void pg_writer_open(PgRelWriter *w, Oid relid, bool prepare_defaults)
{
    memset(w, 0, sizeof *w);
    w->rel = table_open(relid, RowExclusiveLock);
    w->td = RelationGetDescr(w->rel);
    w->estate = CreateExecutorState();
    w->rri = makeNode(ResultRelInfo);
    InitResultRelInfo(w->rri, w->rel, 0, NULL, 0);
    ExecOpenIndices(w->rri, false);
    w->old_slot = table_slot_create(w->rel, NULL);
    w->new_slot = table_slot_create(w->rel, NULL);
    w->cid = GetCurrentCommandId(true);
    w->snap = GetActiveSnapshot();
    if (prepare_defaults && w->td->natts > 0) {
        w->def_states = palloc0(sizeof(ExprState *) * (size_t)w->td->natts);
        for (int c = 0; c < w->td->natts; c++) {
            Form_pg_attribute a = TupleDescAttr(w->td, c);
            if (!a->attisdropped && a->atthasdef) {
                Node *def = build_column_default(w->rel, c + 1);
                if (def) {
                    Expr *plan = expression_planner((Expr *)def);
                    w->def_states[c] = ExecPrepareExpr(plan, w->estate);
                }
            }
        }
    }
}

static void pg_writer_close(PgRelWriter *w)
{
    if (w->def_states) { pfree(w->def_states); w->def_states = NULL; }
    if (w->new_slot) { ExecDropSingleTupleTableSlot(w->new_slot); w->new_slot = NULL; }
    if (w->old_slot) { ExecDropSingleTupleTableSlot(w->old_slot); w->old_slot = NULL; }
    if (w->rri) { ExecCloseIndices(w->rri); w->rri = NULL; }
    if (w->estate) { FreeExecutorState(w->estate); w->estate = NULL; }
    if (w->rel) { table_close(w->rel, NoLock); w->rel = NULL; }
}

static void pg_writer_fill_unset(PgRelWriter *w, const bool *assigned)
{
    ExprContext *econtext = GetPerTupleExprContext(w->estate);
    ResetExprContext(econtext);
    for (int c = 0; c < w->td->natts; c++) {
        if (assigned[c]) continue;
        Form_pg_attribute a = TupleDescAttr(w->td, c);
        if (!a->attisdropped && w->def_states && w->def_states[c]) {
            bool isnull = false;
            w->new_slot->tts_values[c] = ExecEvalExprSwitchContext(w->def_states[c], econtext, &isnull);
            w->new_slot->tts_isnull[c] = isnull;
        } else {
            w->new_slot->tts_values[c] = (Datum)0;
            w->new_slot->tts_isnull[c] = true;
        }
    }
}

static void pg_writer_insert_new_slot(PgRelWriter *w)
{
    if (w->rel->rd_att->constr) ExecConstraints(w->rri, w->new_slot, w->estate);
    table_tuple_insert(w->rel, w->new_slot, w->cid, 0, NULL);
    if (w->rri->ri_NumIndices > 0) {
        List *recheck = ExecInsertIndexTuples(w->rri, w->new_slot, w->estate, false, false, NULL, NIL, false);
        list_free(recheck);
    }
}

static bool pg_writer_update_tid(PgRelWriter *w, ItemPointer tid,
                                 void (*fill_from_old)(TupleTableSlot *old_s, TupleTableSlot *new_s, void *ctx),
                                 void *ctx, bool need_old_row)
{
    if (need_old_row) {
        ExecClearTuple(w->old_slot);
        if (!table_tuple_fetch_row_version(w->rel, tid, w->snap, w->old_slot))
            return false;
    }
retry:
    if (need_old_row) slot_getallattrs(w->old_slot);
    fill_from_old(need_old_row ? w->old_slot : NULL, w->new_slot, ctx);
    if (w->rel->rd_att->constr) ExecConstraints(w->rri, w->new_slot, w->estate);
    TM_FailureData tmfd;
    LockTupleMode lockmode;
    TU_UpdateIndexes update_indexes = TU_None;
    TM_Result res = table_tuple_update(w->rel, tid, w->new_slot, w->cid, w->snap, InvalidSnapshot,
                                       true, &tmfd, &lockmode, &update_indexes);
    if (res == TM_Updated) {
        if (IsolationUsesXactSnapshot())
            ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                            errmsg("could not serialize access due to concurrent update")));
        ExecClearTuple(w->old_slot);
        if (table_tuple_lock(w->rel, tid, w->snap, w->old_slot, w->cid, lockmode, LockWaitBlock,
                             TUPLE_LOCK_FLAG_FIND_LAST_VERSION, &tmfd) == TM_Ok)
            goto retry;
        return false;
    }
    if (res == TM_Ok) {
        if (update_indexes != TU_None && w->rri->ri_NumIndices > 0) {
            List *recheck = ExecInsertIndexTuples(w->rri, w->new_slot, w->estate, true, false, NULL, NIL,
                                                  update_indexes == TU_Summarizing);
            list_free(recheck);
        }
        return true;
    }
    return false;
}

static bool pg_writer_delete_tid(PgRelWriter *w, ItemPointer tid)
{
    TM_FailureData tmfd;
retry:
    TM_Result res = table_tuple_delete(w->rel, tid, w->cid, w->snap, InvalidSnapshot, true, &tmfd, false);
    if (res == TM_Updated) {
        if (IsolationUsesXactSnapshot())
            ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                            errmsg("could not serialize access due to concurrent delete")));
        ExecClearTuple(w->old_slot);
        if (table_tuple_lock(w->rel, tid, w->snap, w->old_slot, w->cid, LockTupleExclusive, LockWaitBlock,
                             TUPLE_LOCK_FLAG_FIND_LAST_VERSION, &tmfd) == TM_Ok)
            goto retry;
        return false;
    }
    return res == TM_Ok;
}

static void collect_stage_claims(Oid relid, Oid native_id, lp_idmap *claims, uint16_t *buckets_out)
{
    Relation rel = table_open(relid, AccessShareLock);
    AttrNumber ac = require_attnum(relid, "claim", native_id, NULL);
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan(rel, snap, 0, NULL);
    TupleTableSlot *slot = table_slot_create(rel, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool missing = false;
        Datum val = slot_getattr(slot, ac, &missing);
        if (missing) pg_series_invalid();
        lp_id claim;
        memcpy(claim.b, DatumGetPointer(val), sizeof(lp_id));
        lp_idmap_put(claims, &claim, NULL);
        if (buckets_out) *buckets_out |= (uint16_t)(1u << (claim.b[0] >> 4));
        ExecClearTuple(slot);
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    table_close(rel, NoLock);
}

static void scan_pending_for_claims(Oid pending_oid, Oid native_id, PgSeriesBatch *batch)
{
    Relation pending = table_open(pending_oid, AccessShareLock);
    AttrNumber ac = require_attnum(pending_oid, "claim", native_id, NULL);
    AttrNumber aw = require_attnum(pending_oid, "witness", native_id, NULL);
    AttrNumber al = require_attnum(pending_oid, "lineage", native_id, NULL);
    AttrNumber ag = require_attnum(pending_oid, "games", InvalidOid, NULL);
    AttrNumber asc = require_attnum(pending_oid, "score", InvalidOid, NULL);
    AttrNumber at = require_attnum(pending_oid, "trust", InvalidOid, NULL);
    Oid tg = get_atttype(pending_oid, ag);
    Oid tsc = get_atttype(pending_oid, asc);
    Oid tt = get_atttype(pending_oid, at);

    Snapshot snap = GetActiveSnapshot();
    size_t count = lp_idmap_count(batch->claims);
    Oid idxOid = (count <= 4096) ? find_claim_btree_index(pending, ac) : InvalidOid;
    TupleTableSlot *slot = table_slot_create(pending, NULL);

    if (OidIsValid(idxOid)) {
        Relation idx = index_open(idxOid, AccessShareLock);
        Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
        RegProcedure eq_proc = get_opcode(eq_op);
        lp_id *uniq = palloc(sizeof(lp_id) * count);
        memcpy(uniq, lp_idmap_keys(batch->claims), sizeof(lp_id) * count);
        lp_sort(uniq, count, sizeof(lp_id), lp_id_cmp);

        IndexScanDesc iscan = index_beginscan(pending, idx, snap, NULL, 1, 0);
        size_t window = frontier_window_ids();
        for (size_t off = 0; off < count;) {
            size_t take = Min(window, count - off);
            ArrayType *arr = id_array_of(native_id, uniq + off, (int)take);
            ScanKeyData sk;
            ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, native_id,
                                   idx->rd_indcollation[0], eq_proc, PointerGetDatum(arr));
            index_rescan(iscan, &sk, 1, NULL, 0);
            while (index_getnext_slot(iscan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool mc, mw, ml, mg, msc, mt;
                Datum vc = slot_getattr(slot, ac, &mc);
                if (!mc) {
                    lp_id claim, witness, lineage = {0};
                    memcpy(claim.b, DatumGetPointer(vc), 16);
                    Datum vw = slot_getattr(slot, aw, &mw);
                    if (mw) pg_series_invalid();
                    memcpy(witness.b, DatumGetPointer(vw), 16);
                    Datum vl = slot_getattr(slot, al, &ml);
                    if (!ml) memcpy(lineage.b, DatumGetPointer(vl), 16);
                    int64 games = read_int64_slot(slot, ag, tg, &mg); if (mg) pg_series_invalid();
                    double score = read_float8_slot(slot, asc, tsc, &msc); if (msc) pg_series_invalid();
                    double trust = read_float8_slot(slot, at, tt, &mt); if (mt) pg_series_invalid();
                    record_series_observation(batch, &claim, &witness, !ml, &lineage, games, score, trust);
                }
                ExecClearTuple(slot);
            }
            pfree(arr);
            off += take;
        }
        index_endscan(iscan);
        index_close(idx, NoLock);
        pfree(uniq);
    } else {
        TableScanDesc tscan = table_beginscan(pending, snap, 0, NULL);
        while (table_scan_getnextslot(tscan, ForwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool mc, mw, ml, mg, msc, mt;
            Datum vc = slot_getattr(slot, ac, &mc);
            if (!mc) {
                lp_id claim;
                memcpy(claim.b, DatumGetPointer(vc), 16);
                if (lp_idmap_find(batch->claims, &claim) >= 0) {
                    lp_id witness, lineage = {0};
                    Datum vw = slot_getattr(slot, aw, &mw);
                    if (mw) pg_series_invalid();
                    memcpy(witness.b, DatumGetPointer(vw), 16);
                    Datum vl = slot_getattr(slot, al, &ml);
                    if (!ml) memcpy(lineage.b, DatumGetPointer(vl), 16);
                    int64 games = read_int64_slot(slot, ag, tg, &mg); if (mg) pg_series_invalid();
                    double score = read_float8_slot(slot, asc, tsc, &msc); if (msc) pg_series_invalid();
                    double trust = read_float8_slot(slot, at, tt, &mt); if (mt) pg_series_invalid();
                    record_series_observation(batch, &claim, &witness, !ml, &lineage, games, score, trust);
                }
            }
            ExecClearTuple(slot);
        }
        table_endscan(tscan);
    }

    ExecDropSingleTupleTableSlot(slot);
    table_close(pending, NoLock);
}

typedef struct {
    lp_id *claims;
    lp_standing *standings;
    bool *present;
    size_t count;
} PgEvaluatedSeries;

static void eval_series_for_claims(Oid pending_oid, Oid native_id, lp_idmap *claims,
                                   MemoryContext result_ctx, PgEvaluatedSeries *out)
{
    memset(out, 0, sizeof *out);
    size_t count = lp_idmap_count(claims);
    if (!count) return;

    MemoryContext caller = CurrentMemoryContext;
    MemoryContext context = AllocSetContextCreate(caller, "Native series batch", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);
    PgSeriesBatch *batch = palloc0(sizeof *batch);
    MemoryContextCallback *cleanup = palloc(sizeof *cleanup);
    *cleanup = (MemoryContextCallback){ .func = pg_series_batch_free, .arg = batch };
    MemoryContextRegisterResetCallback(context, cleanup);
    batch->claims = claims;
    batch->witnesses = lp_idmap_sized(sizeof(PgSeriesWitness));
    batch->pending = lp_strmap_sized(1);
    batch->pairs = lp_strmap_sized(1);
    batch->trusts = lp_strmap_sized(sizeof(double));

    PgClaimFrontier verified = {0};
    lp_pg_claim_bind(&verified.binding);
    lp_pg_lookup_paths(lp_idmap_keys(batch->claims), count, true, lp_pg_claim_frontier_row, &verified);
    MemoryContextDelete(verified.binding.context);
    if (verified.found != count) pg_series_invalid();

    if (OidIsValid(pending_oid))
        scan_pending_for_claims(pending_oid, native_id, batch);

    for (size_t i = 0; i < batch->observations.n; i++) {
        const PgSeriesObservation *o = batch->observations.v + i;
        bool fresh;
        PgSeriesWitness *w = lp_idmap_get(batch->witnesses, &o->witness, &fresh);
        if (fresh || !o->has_lineage) {
            w->trust = o->trust;
            w->has_lineage = o->has_lineage;
            if (o->has_lineage) w->lineage = o->lineage;
        }
    }

    lp_pg_read_attestation_rows(lp_idmap_keys(batch->claims), (int)count, NULL, &batch->historical, batch->witnesses);
    pg_attribution_ancestors(batch);

    for (size_t i = 0; i < batch->historical.n; i++) {
        const PgAttrRow *r = batch->historical.v + i;
        uint8_t key[32];
        memcpy(key, r->claim.b, 16);
        memcpy(key + 16, r->trunk.b, 16);
        if (lp_strmap_find(batch->pairs, key, sizeof key) >= 0 || r->games <= 0) continue;
        PgSeriesWitness *w = lp_idmap_lookup(batch->witnesses, &r->trunk);
        if (!w || r->games > 9007199254740992.0 || !isfinite(r->games)) pg_series_invalid();
        PgSeriesObservation o = {
            .claim = r->claim,
            .witness = r->trunk,
            .games = (uint64_t)r->games,
            .score = r->score / r->games,
            .trust = r->has_dependence ? r->trust : w->trust,
            .has_lineage = r->has_dependence || w->has_lineage,
            .lineage = r->has_dependence ? r->dependence : w->lineage
        };
        lp_push(&batch->observations, o);
    }

    lp_sort(batch->observations.v, batch->observations.n, sizeof(PgSeriesObservation), pg_series_by_claim);

    lp_id *ordered = MemoryContextAlloc(result_ctx, count * sizeof(lp_id));
    lp_standing *standings = MemoryContextAllocZero(result_ctx, count * sizeof(lp_standing));
    bool *present = MemoryContextAllocZero(result_ctx, count * sizeof(bool));
    memcpy(ordered, lp_idmap_keys(batch->claims), count * sizeof(lp_id));
    lp_sort(ordered, count, sizeof(lp_id), lp_id_cmp);

    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        CHECK_FOR_INTERRUPTS();
        batch->entries.n = 0;
        while (at < batch->observations.n && lp_id_eq(&batch->observations.v[at].claim, ordered + i)) {
            const PgSeriesObservation *o = batch->observations.v + at++;
            lp_id root = pg_series_root(batch, o->has_lineage ? o->lineage : o->witness);
            double wins = fmin((double)o->games, fmax(0.0, (double)o->games * o->score));
            lp_series_entry entry;
            if (!lp_series_attested(root, (double)o->games, wins, o->trust, &entry))
                pg_series_invalid();
            lp_push(&batch->entries, entry);
        }
        if (batch->entries.n) {
            lp_standing standing = {0};
            if (!lp_series(batch->entries.v, batch->entries.n, &standing) || standing.matches > PG_INT64_MAX) {
                const lp_series_entry *e0 = batch->entries.v;
                ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                                errmsg("laplace_series: invalid or non-convergent rooted series"),
                                errdetail("errno=%d entries=%zu matches=%llu e0=(games=%llu wins=%.17g trust=%.17g wdev=%.17g opp=%.17g/%.17g)",
                                          errno, batch->entries.n, (unsigned long long)standing.matches,
                                          (unsigned long long)e0->games, e0->wins, e0->trust,
                                          e0->witness_deviation, e0->opponent.rating, e0->opponent.deviation)));
            }
            standings[i] = standing;
            present[i] = true;
        }
    }

    batch->claims = NULL; /* Caller owns lifetime of claims map if separate */
    MemoryContextSwitchTo(caller);
    MemoryContextDelete(context);

    out->claims = ordered;
    out->standings = standings;
    out->present = present;
    out->count = count;
}

PG_FUNCTION_INFO_V1(laplace_series_snapshot);
Datum laplace_series_snapshot(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *result = set_begin(fcinfo, "laplace_series_snapshot", &td);
    if (PG_ARGISNULL(0)) pg_series_invalid();

    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));
    Oid pending = pg_series_stage(PG_GETARG_OID(0), "claim", native_id, false);
    Oid changed = InvalidOid;
    bool stage_selection = get_fn_expr_argtype(fcinfo->flinfo, 1) == REGCLASSOID;
    if (stage_selection && PG_ARGISNULL(1)) pg_series_invalid();
    if (stage_selection) changed = pg_series_stage(PG_GETARG_OID(1), "claim", native_id, true);

    lp_idmap *claims = lp_idmap_new();
    if (!stage_selection && !PG_ARGISNULL(1)) {
        int count;
        lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(1), &count);
        for (int i = 0; i < count; i++) lp_idmap_put(claims, ids + i, NULL);
        pfree(ids);
    } else {
        collect_stage_claims(OidIsValid(changed) ? changed : pending, native_id, claims, NULL);
    }

    PgEvaluatedSeries eval = {0};
    if (lp_idmap_count(claims) > 0)
        eval_series_for_claims(pending, native_id, claims, CurrentMemoryContext, &eval);
    lp_idmap_free(claims);

    for (size_t i = 0; i < eval.count; i++) {
        Datum values[5] = { id_datum(&eval.claims[i]), 0, 0, 0, 0 };
        bool missing[5] = { false, true, true, true, true };
        if (eval.present[i]) {
            values[1] = Float8GetDatum(eval.standings[i].rating);
            values[2] = Float8GetDatum(eval.standings[i].deviation);
            values[3] = Float8GetDatum(eval.standings[i].volatility);
            values[4] = Int64GetDatum((int64)eval.standings[i].matches);
            memset(missing, 0, sizeof missing);
        }
        set_put(fcinfo, result, td, values, missing);
        pfree(DatumGetPointer(values[0]));
    }
    if (eval.claims) pfree(eval.claims);
    if (eval.standings) pfree(eval.standings);
    if (eval.present) pfree(eval.present);
    PG_RETURN_NULL();
}

typedef struct {
    lp_id claim;
    lp_standing standing;
    bool present;
    Oid partition;
    size_t orig_idx;
} ConsensusWriteItem;

static int consensus_write_cmp(const void *a, const void *b)
{
    const ConsensusWriteItem *x = a, *y = b;
    int c = (x->partition > y->partition) - (x->partition < y->partition);
    return c ? c : lp_id_cmp(&x->claim, &y->claim);
}

typedef struct {
    const ConsensusWriteItem *item;
    AttrNumber a_claim, a_rating, a_dev, a_vol, a_matches;
} ConsensusFillCtx;

static void fill_consensus_slot(TupleTableSlot *old_s, TupleTableSlot *new_s, void *context)
{
    (void)old_s;
    const ConsensusFillCtx *c = context;
    ExecClearTuple(new_s);
    memset(new_s->tts_isnull, 0, sizeof(bool) * (size_t)new_s->tts_tupleDescriptor->natts);
    for (int col = 0; col < new_s->tts_tupleDescriptor->natts; col++)
        if (TupleDescAttr(new_s->tts_tupleDescriptor, col)->attisdropped) new_s->tts_isnull[col] = true;
    new_s->tts_values[c->a_claim - 1] = PointerGetDatum(&c->item->claim);
    new_s->tts_values[c->a_rating - 1] = Float8GetDatum(c->item->standing.rating);
    new_s->tts_values[c->a_dev - 1] = Float8GetDatum(c->item->standing.deviation);
    new_s->tts_values[c->a_vol - 1] = Float8GetDatum(c->item->standing.volatility);
    new_s->tts_values[c->a_matches - 1] = Int64GetDatum((int64)c->item->standing.matches);
    ExecStoreVirtualTuple(new_s);
}

static int64 write_consensus_items(ConsensusWriteItem *items, size_t count, Oid native_id)
{
    if (!count) return 0;

    Oid consensus = lp_pg_relation("consensus");
    require_rel_priv(consensus, ACL_SELECT | ACL_INSERT | ACL_UPDATE | ACL_DELETE);
    Relation parent = table_open(consensus, AccessShareLock);
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;
    for (size_t i = 0; i < count; i++) {
        if (!OidIsValid(items[i].partition))
            items[i].partition = lp_pg_enrichment_partition(consensus, pkey, pdesc, PointerGetDatum(&items[i].claim));
    }
    table_close(parent, NoLock);
    lp_sort(items, count, sizeof(*items), consensus_write_cmp);

    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions("consensus", &parts, &pks, NULL, NULL);
    size_t array_ids = (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id);
    int64 affected = 0;

    for (size_t begin = 0; begin < count;) {
        CHECK_FOR_INTERRUPTS();
        size_t end = begin + 1;
        while (end < count && items[end].partition == items[begin].partition) end++;
        if (end - begin > array_ids) end = begin + array_ids;
        size_t len = end - begin;

        Oid partOid = items[begin].partition;
        PgRelWriter w;
        pg_writer_open(&w, partOid, false);
        Oid pkOid = InvalidOid;
        for (int p = 0; p < nparts; p++) if (parts[p] == partOid) { pkOid = pks[p]; break; }
        if (!OidIsValid(pkOid)) pkOid = RelationGetPrimaryKeyIndex(w.rel, false);
        if (!OidIsValid(pkOid)) ereport(ERROR, (errmsg("Native consensus: partition has no primary index")));
        Relation idx = index_open(pkOid, AccessShareLock);

        AttrNumber ac = get_attnum(partOid, "claim");
        AttrNumber ar = get_attnum(partOid, "rating");
        AttrNumber ad = get_attnum(partOid, "deviation");
        AttrNumber av = get_attnum(partOid, "volatility");
        AttrNumber am = get_attnum(partOid, "matches");

        Datum *vals = palloc(sizeof(Datum) * len);
        for (size_t k = 0; k < len; k++) vals[k] = PointerGetDatum(&items[begin + k].claim);
        ArrayType *wanted = construct_array(vals, (int)len, native_id, sizeof(lp_id), false, TYPALIGN_CHAR);
        pfree(vals);

        Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
        ScanKeyData sk;
        ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, native_id,
                               idx->rd_indcollation[0], get_opcode(eq_op), PointerGetDatum(wanted));
        IndexScanDesc scan = index_beginscan(w.rel, idx, w.snap, NULL, 1, 0);
        index_rescan(scan, &sk, 1, NULL, 0);

        ItemPointerData *existing_tids = palloc0(sizeof(ItemPointerData) * len);
        bool *matched = palloc0(sizeof(bool) * len);
        while (index_getnext_slot(scan, ForwardScanDirection, w.old_slot)) {
            CHECK_FOR_INTERRUPTS();
            bool missing = false;
            Datum vc = slot_getattr(w.old_slot, ac, &missing);
            if (!missing) {
                ConsensusWriteItem key_item = { .claim = *(const lp_id *)DatumGetPointer(vc), .partition = partOid };
                ConsensusWriteItem *hit = bsearch(&key_item, items + begin, len, sizeof(*items), consensus_write_cmp);
                if (hit) {
                    size_t idx_off = (size_t)(hit - (items + begin));
                    existing_tids[idx_off] = w.old_slot->tts_tid;
                    matched[idx_off] = true;
                }
            }
            ExecClearTuple(w.old_slot);
        }
        index_endscan(scan);
        pfree(wanted);
        index_close(idx, NoLock);

        for (size_t k = 0; k < len; k++) {
            ConsensusFillCtx fctx = { &items[begin + k], ac, ar, ad, av, am };
            if (matched[k]) {
                if (items[begin + k].present) {
                    if (pg_writer_update_tid(&w, &existing_tids[k], fill_consensus_slot, &fctx, false))
                        affected++;
                } else {
                    if (pg_writer_delete_tid(&w, &existing_tids[k]))
                        affected++;
                }
            } else if (items[begin + k].present) {
                fill_consensus_slot(NULL, w.new_slot, &fctx);
                pg_writer_insert_new_slot(&w);
                affected++;
            }
        }
        pfree(existing_tids);
        pfree(matched);
        pg_writer_close(&w);
        begin = end;
    }
    if (affected > 0) CommandCounterIncrement();
    return affected;
}

static int64 attest_stage_core(Oid pending_oid, Oid native_id, lp_idmap *claims, uint16_t buckets)
{
    if (lp_idmap_count(claims) == 0) return 0;

    for (int h = 0; h < 16; h++)
        if (buckets & (uint16_t)(1u << h))
            DirectFunctionCall2(pg_advisory_xact_lock_int4, Int32GetDatum(1279348803), Int32GetDatum(h));

    int64 affected = 0;
    PgEvaluatedSeries eval = {0};
    PushActiveSnapshot(GetTransactionSnapshot());
    PG_TRY(); {
        eval_series_for_claims(pending_oid, native_id, claims, CurrentMemoryContext, &eval);
        if (eval.count > 0) {
            ConsensusWriteItem *items = palloc_extended(sizeof(*items) * eval.count, MCXT_ALLOC_HUGE);
            for (size_t i = 0; i < eval.count; i++) {
                items[i] = (ConsensusWriteItem){
                    .claim = eval.claims[i],
                    .standing = eval.standings[i],
                    .present = eval.present[i],
                    .partition = InvalidOid,
                    .orig_idx = i
                };
            }
            affected = write_consensus_items(items, eval.count, native_id);
            pfree(items);
        }
    } PG_FINALLY(); {
        PopActiveSnapshot();
    } PG_END_TRY();

    if (eval.claims) pfree(eval.claims);
    if (eval.standings) pfree(eval.standings);
    if (eval.present) pfree(eval.present);
    return affected;
}

PG_FUNCTION_INFO_V1(laplace_attest_stage);
Datum laplace_attest_stage(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_attest_stage requires an UNLOGGED changed-claim table")));
    Oid pending = PG_GETARG_OID(0);
    Oid changed = PG_GETARG_OID(1);
    require_stage_kind(changed, true, "laplace_attest_stage requires an UNLOGGED changed-claim table");
    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));
    pending = pg_series_stage(pending, "claim", native_id, false);
    changed = pg_series_stage(changed, "claim", native_id, true);

    lp_idmap *claims = lp_idmap_new();
    uint16_t buckets = 0;
    collect_stage_claims(changed, native_id, claims, &buckets);
    int64 affected = attest_stage_core(pending, native_id, claims, buckets);
    lp_idmap_free(claims);
    PG_RETURN_INT64(affected);
}

PG_FUNCTION_INFO_V1(laplace_attest_series);
Datum laplace_attest_series(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_attest_series requires an UNLOGGED stage table")));
    Oid pending = PG_GETARG_OID(0);
    require_stage_kind(pending, false, "laplace_attest_series requires an UNLOGGED stage table");
    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));
    pending = pg_series_stage(pending, "claim", native_id, false);

    lp_idmap *claims = lp_idmap_new();
    uint16_t buckets = 0;
    if (!PG_ARGISNULL(1)) {
        int count;
        lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(1), &count);
        for (int i = 0; i < count; i++) {
            lp_idmap_put(claims, ids + i, NULL);
            buckets |= (uint16_t)(1u << (ids[i].b[0] >> 4));
        }
        pfree(ids);
    } else {
        collect_stage_claims(pending, native_id, claims, &buckets);
    }
    int64 affected = attest_stage_core(pending, native_id, claims, buckets);
    lp_idmap_free(claims);
    PG_RETURN_INT64(affected);
}

typedef struct {
    lp_id claim, witness, lineage;
    int64 games, tokens;
    double wins, trust;
    int32 pos;
    bool has_lin, has_pos;
    ItemPointerData tid;
    bool found;
} AccDeltaRow;

typedef struct {
    const AccDeltaRow *row;
    AttrNumber a_claim, a_wit, a_lin, a_games, a_tok, a_score, a_pos, a_trust, a_wins;
    Oid t_games, t_tok, t_score, t_pos, t_trust, t_wins;
} AccWriteCtx;

static void write_acc_update(TupleTableSlot *old_s, TupleTableSlot *new_s, void *context)
{
    const AccWriteCtx *c = context;
    const AccDeltaRow *d = c->row;
    ExecClearTuple(new_s);
    int natts = new_s->tts_tupleDescriptor->natts;
    for (int col = 0; col < natts; col++) {
        new_s->tts_values[col] = old_s->tts_values[col];
        new_s->tts_isnull[col] = old_s->tts_isnull[col];
    }

    bool nul;
    int64 pg = read_int64_slot(old_s, c->a_games, c->t_games, &nul);
    int64 pt = read_int64_slot(old_s, c->a_tok, c->t_tok, &nul);
    double pw = read_float8_slot(old_s, c->a_wins, c->t_wins, &nul);
    bool pp_null = false;
    int32 pp = 0;
    Datum vpp = slot_getattr(old_s, c->a_pos, &pp_null);
    if (!pp_null) pp = DatumGetInt32(vpp);

    int64 ng = pg + d->games;
    int64 nt = pt + d->tokens;
    double nw = fmin((double)ng, fmax(0.0, pw + d->wins));
    double nscore = fmin(1.0, fmax(0.0, nw / (double)ng));
    bool npos_null;
    int32 npos = 0;
    if (pp_null || pp <= 0) {
        npos_null = !d->has_pos;
        npos = d->pos;
    } else if (!d->has_pos || d->pos <= 0) {
        npos_null = false;
        npos = pp;
    } else {
        npos_null = false;
        npos = Min(pp, d->pos);
    }

    new_s->tts_values[c->a_games - 1] = Int64GetDatum(ng); new_s->tts_isnull[c->a_games - 1] = false;
    new_s->tts_values[c->a_tok - 1] = Int64GetDatum(nt); new_s->tts_isnull[c->a_tok - 1] = false;
    new_s->tts_values[c->a_wins - 1] = Float8GetDatum(nw); new_s->tts_isnull[c->a_wins - 1] = false;
    new_s->tts_values[c->a_score - 1] = Float8GetDatum(nscore); new_s->tts_isnull[c->a_score - 1] = false;
    new_s->tts_values[c->a_pos - 1] = npos_null ? (Datum)0 : Int32GetDatum(npos);
    new_s->tts_isnull[c->a_pos - 1] = npos_null;
    new_s->tts_values[c->a_trust - 1] = Float8GetDatum(d->trust); new_s->tts_isnull[c->a_trust - 1] = false;
    ExecStoreVirtualTuple(new_s);
}

static int64 accumulate_series_core(Oid pending_oid, Oid delta_oid, Oid affected_oid, const char *caller_name)
{
    if (!OidIsValid(pending_oid) || !OidIsValid(delta_oid) || pending_oid == delta_oid ||
        (OidIsValid(affected_oid) && (affected_oid == pending_oid || affected_oid == delta_oid)))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("%s requires distinct UNLOGGED stage tables", caller_name)));

    require_stage_kind(pending_oid, false, "laplace_accumulate_series requires distinct UNLOGGED stage tables");
    require_stage_kind(delta_oid, false, "laplace_accumulate_series requires distinct UNLOGGED stage tables");
    require_rel_priv(pending_oid, ACL_SELECT | ACL_INSERT | ACL_UPDATE);
    require_rel_priv(delta_oid, OidIsValid(affected_oid) ? (ACL_SELECT | ACL_TRUNCATE) : ACL_SELECT);
    if (OidIsValid(affected_oid)) {
        require_stage_kind(affected_oid, true, "laplace_stage_flush_claims requires distinct UNLOGGED stage tables");
        require_rel_priv(affected_oid, ACL_SELECT | ACL_INSERT);
    }

    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));

    Relation delta = table_open(delta_oid, AccessShareLock);
    AttrNumber dc = require_attnum(delta_oid, "claim", native_id, NULL);
    AttrNumber dw = require_attnum(delta_oid, "witness", native_id, NULL);
    AttrNumber dl = require_attnum(delta_oid, "lineage", native_id, NULL);
    AttrNumber dg = require_attnum(delta_oid, "games", InvalidOid, NULL);
    AttrNumber dt = require_attnum(delta_oid, "tokens", InvalidOid, NULL);
    AttrNumber dpos = require_attnum(delta_oid, "position", INT4OID, NULL);
    AttrNumber dtr = require_attnum(delta_oid, "trust", InvalidOid, NULL);
    AttrNumber dwins = require_attnum(delta_oid, "wins", InvalidOid, NULL);
    Oid tdg = get_atttype(delta_oid, dg), tdt = get_atttype(delta_oid, dt);
    Oid tdtr = get_atttype(delta_oid, dtr), tdwins = get_atttype(delta_oid, dwins);

    lp_vec(AccDeltaRow) incoming = {0};
    lp_strmap *seen = lp_strmap_sized(1);
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc dscan = table_beginscan(delta, snap, 0, NULL);
    TupleTableSlot *dslot = table_slot_create(delta, NULL);
    while (table_scan_getnextslot(dscan, ForwardScanDirection, dslot)) {
        CHECK_FOR_INTERRUPTS();
        bool nul;
        AccDeltaRow r = {0};
        Datum v = slot_getattr(dslot, dc, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"claim\"")));
        memcpy(r.claim.b, DatumGetPointer(v), 16);
        v = slot_getattr(dslot, dw, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"witness\"")));
        memcpy(r.witness.b, DatumGetPointer(v), 16);
        v = slot_getattr(dslot, dl, &nul);
        r.has_lin = !nul;
        if (!nul) memcpy(r.lineage.b, DatumGetPointer(v), 16);
        r.games = read_int64_slot(dslot, dg, tdg, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"games\"")));
        r.tokens = read_int64_slot(dslot, dt, tdt, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"tokens\"")));
        v = slot_getattr(dslot, dpos, &nul);
        r.has_pos = !nul;
        if (!nul) r.pos = DatumGetInt32(v);
        r.trust = read_float8_slot(dslot, dtr, tdtr, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"trust\"")));
        r.wins = read_float8_slot(dslot, dwins, tdwins, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"wins\"")));

        uint8_t key[49];
        cwl_key_pack(key, &r.claim, &r.witness, r.has_lin, &r.lineage);
        bool fresh = false;
        lp_strmap_put(seen, key, sizeof key, &fresh);
        if (!fresh)
            ereport(ERROR, (errcode(ERRCODE_CARDINALITY_VIOLATION),
                            errmsg("ON CONFLICT DO UPDATE command cannot affect row a second time")));
        lp_push(&incoming, r);
        ExecClearTuple(dslot);
    }
    ExecDropSingleTupleTableSlot(dslot);
    table_endscan(dscan);
    table_close(delta, NoLock);
    lp_strmap_free(seen);

    if (!incoming.n) { lp_vec_free(&incoming); return 0; }

    PgRelWriter w;
    pg_writer_open(&w, pending_oid, true);
    AttrNumber ac = require_attnum(pending_oid, "claim", native_id, NULL);
    AttrNumber aw = require_attnum(pending_oid, "witness", native_id, NULL);
    AttrNumber al = require_attnum(pending_oid, "lineage", native_id, NULL);
    AttrNumber ag = require_attnum(pending_oid, "games", INT8OID, NULL);
    AttrNumber atok = require_attnum(pending_oid, "tokens", INT8OID, NULL);
    AttrNumber ascore = require_attnum(pending_oid, "score", FLOAT8OID, NULL);
    AttrNumber apos = require_attnum(pending_oid, "position", INT4OID, NULL);
    AttrNumber atrust = require_attnum(pending_oid, "trust", FLOAT8OID, NULL);
    AttrNumber awins = require_attnum(pending_oid, "wins", FLOAT8OID, NULL);

    Oid idxOid = find_cwl_index(w.rel, ac, aw, al, true);
    if (!OidIsValid(idxOid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                        errmsg("there is no unique or exclusion constraint matching the ON CONFLICT specification")));
    Relation idx = index_open(idxOid, AccessShareLock);
    IndexScanDesc iscan = index_beginscan(w.rel, idx, w.snap, NULL, 3, 0);
    for (size_t i = 0; i < incoming.n; i++) {
        if (!(i & 255)) CHECK_FOR_INTERRUPTS();
        AccDeltaRow *d = &incoming.v[i];
        ScanKeyData sk[3];
        init_cwl_scankeys(sk, idx, native_id, &d->claim, &d->witness, d->has_lin, &d->lineage);
        index_rescan(iscan, sk, 3, NULL, 0);
        if (index_getnext_slot(iscan, ForwardScanDirection, w.old_slot)) {
            d->tid = w.old_slot->tts_tid;
            d->found = true;
            ExecClearTuple(w.old_slot);
        }
    }
    index_endscan(iscan);
    index_close(idx, NoLock);

    bool *assigned = palloc0(sizeof(bool) * (size_t)w.td->natts);
    assigned[ac - 1] = assigned[aw - 1] = assigned[al - 1] = assigned[ag - 1] = true;
    assigned[atok - 1] = assigned[ascore - 1] = assigned[apos - 1] = assigned[atrust - 1] = assigned[awins - 1] = true;

    int64 affected = 0;
    AccWriteCtx ctx = {
        .a_claim = ac, .a_wit = aw, .a_lin = al, .a_games = ag, .a_tok = atok,
        .a_score = ascore, .a_pos = apos, .a_trust = atrust, .a_wins = awins,
        .t_games = INT8OID, .t_tok = INT8OID, .t_score = FLOAT8OID, .t_pos = INT4OID,
        .t_trust = FLOAT8OID, .t_wins = FLOAT8OID
    };
    for (size_t i = 0; i < incoming.n; i++) {
        CHECK_FOR_INTERRUPTS();
        const AccDeltaRow *d = &incoming.v[i];
        if (d->found) {
            ctx.row = d;
            if (pg_writer_update_tid(&w, &incoming.v[i].tid, write_acc_update, &ctx, true))
                affected++;
        } else {
            ExecClearTuple(w.new_slot);
            pg_writer_fill_unset(&w, assigned);
            w.new_slot->tts_values[ac - 1] = PointerGetDatum(&d->claim); w.new_slot->tts_isnull[ac - 1] = false;
            w.new_slot->tts_values[aw - 1] = PointerGetDatum(&d->witness); w.new_slot->tts_isnull[aw - 1] = false;
            w.new_slot->tts_values[al - 1] = d->has_lin ? PointerGetDatum(&d->lineage) : (Datum)0;
            w.new_slot->tts_isnull[al - 1] = !d->has_lin;
            w.new_slot->tts_values[ag - 1] = Int64GetDatum(d->games); w.new_slot->tts_isnull[ag - 1] = false;
            w.new_slot->tts_values[atok - 1] = Int64GetDatum(d->tokens); w.new_slot->tts_isnull[atok - 1] = false;
            w.new_slot->tts_values[awins - 1] = Float8GetDatum(d->wins); w.new_slot->tts_isnull[awins - 1] = false;
            w.new_slot->tts_values[ascore - 1] = Float8GetDatum(d->wins / (double)d->games); w.new_slot->tts_isnull[ascore - 1] = false;
            w.new_slot->tts_values[apos - 1] = d->has_pos ? Int32GetDatum(d->pos) : (Datum)0;
            w.new_slot->tts_isnull[apos - 1] = !d->has_pos;
            w.new_slot->tts_values[atrust - 1] = Float8GetDatum(d->trust); w.new_slot->tts_isnull[atrust - 1] = false;
            ExecStoreVirtualTuple(w.new_slot);
            pg_writer_insert_new_slot(&w);
            affected++;
        }
    }
    pfree(assigned);
    pg_writer_close(&w);

    if (OidIsValid(affected_oid)) {
        lp_idmap *uniq_claims = lp_idmap_new();
        for (size_t i = 0; i < incoming.n; i++)
            lp_idmap_put(uniq_claims, &incoming.v[i].claim, NULL);
        size_t nu = lp_idmap_count(uniq_claims);
        bool *present_aff = palloc0(nu ? nu : 1);

        PgRelWriter aw_rel;
        pg_writer_open(&aw_rel, affected_oid, true);
        AttrNumber ac_aff = require_attnum(affected_oid, "claim", native_id, NULL);
        Oid aff_idx = find_claim_btree_index(aw_rel.rel, ac_aff);
        if (OidIsValid(aff_idx)) {
            Relation aidx = index_open(aff_idx, AccessShareLock);
            Oid eq_op = get_opfamily_member(aidx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
            RegProcedure eq_proc = get_opcode(eq_op);
            IndexScanDesc ascan = index_beginscan(aw_rel.rel, aidx, aw_rel.snap, NULL, 1, 0);
            size_t array_ids = (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id);
            const lp_id *ukeys = lp_idmap_keys(uniq_claims);
            for (size_t off = 0; off < nu; off += array_ids) {
                CHECK_FOR_INTERRUPTS();
                size_t chunk = Min(array_ids, nu - off);
                Datum *vals = palloc(sizeof(Datum) * chunk);
                for (size_t k = 0; k < chunk; k++) vals[k] = PointerGetDatum(&ukeys[off + k]);
                ArrayType *wanted = construct_array(vals, (int)chunk, native_id, sizeof(lp_id), false, TYPALIGN_CHAR);
                pfree(vals);
                ScanKeyData sk;
                ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, native_id,
                                       aidx->rd_indcollation[0], eq_proc, PointerGetDatum(wanted));
                index_rescan(ascan, &sk, 1, NULL, 0);
                while (index_getnext_slot(ascan, ForwardScanDirection, aw_rel.old_slot)) {
                    bool nul = false;
                    Datum vc = slot_getattr(aw_rel.old_slot, ac_aff, &nul);
                    if (!nul) {
                        int64 pos = lp_idmap_find(uniq_claims, (const lp_id *)DatumGetPointer(vc));
                        if (pos >= 0) present_aff[pos] = true;
                    }
                    ExecClearTuple(aw_rel.old_slot);
                }
                pfree(wanted);
            }
            index_endscan(ascan);
            index_close(aidx, NoLock);
        } else {
            TableScanDesc s = table_beginscan(aw_rel.rel, aw_rel.snap, 0, NULL);
            while (table_scan_getnextslot(s, ForwardScanDirection, aw_rel.old_slot)) {
                CHECK_FOR_INTERRUPTS();
                bool nul = false;
                Datum vc = slot_getattr(aw_rel.old_slot, ac_aff, &nul);
                if (!nul) {
                    int64 pos = lp_idmap_find(uniq_claims, (const lp_id *)DatumGetPointer(vc));
                    if (pos >= 0) present_aff[pos] = true;
                }
                ExecClearTuple(aw_rel.old_slot);
            }
            table_endscan(s);
        }

        bool *aff_assigned = palloc0(sizeof(bool) * (size_t)aw_rel.td->natts);
        aff_assigned[ac_aff - 1] = true;
        for (size_t i = 0; i < nu; i++) {
            if (present_aff[i]) continue;
            CHECK_FOR_INTERRUPTS();
            const lp_id *cid = lp_idmap_key(uniq_claims, i);
            ExecClearTuple(aw_rel.new_slot);
            pg_writer_fill_unset(&aw_rel, aff_assigned);
            aw_rel.new_slot->tts_values[ac_aff - 1] = PointerGetDatum(cid);
            aw_rel.new_slot->tts_isnull[ac_aff - 1] = false;
            ExecStoreVirtualTuple(aw_rel.new_slot);
            pg_writer_insert_new_slot(&aw_rel);
        }
        pfree(aff_assigned);
        pg_writer_close(&aw_rel);
        pfree(present_aff);
        lp_idmap_free(uniq_claims);

        CommandCounterIncrement();
        lp_pg_truncate_rel(delta_oid, InvalidOid);
    }

    lp_vec_free(&incoming);
    return affected;
}

PG_FUNCTION_INFO_V1(laplace_accumulate_series);
Datum laplace_accumulate_series(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_accumulate_series requires distinct UNLOGGED stage tables")));
    PG_RETURN_INT64(accumulate_series_core(PG_GETARG_OID(0), PG_GETARG_OID(1), InvalidOid, "laplace_accumulate_series"));
}

typedef struct {
    lp_id claim, witness, lineage;
    int64 games, tokens;
    double score, trust;
    int32 pos;
    bool has_lin, has_pos, staged;
} ApplyDeltaRow;

typedef struct {
    ItemPointerData tid;
    size_t delta_idx;
} ApplyUpdateTarget;

typedef struct {
    const ApplyDeltaRow *row;
    AttrNumber a_claim, a_wit, a_lin, a_games, a_tok, a_score, a_pos, a_trust;
    bool preserve_other_cols;
} ApplyWriteCtx;

static void write_apply_slot(TupleTableSlot *old_s, TupleTableSlot *new_s, void *context)
{
    const ApplyWriteCtx *c = context;
    const ApplyDeltaRow *s = c->row;
    if (c->preserve_other_cols) {
        ExecClearTuple(new_s);
        int natts = new_s->tts_tupleDescriptor->natts;
        for (int col = 0; col < natts; col++) {
            new_s->tts_values[col] = old_s->tts_values[col];
            new_s->tts_isnull[col] = old_s->tts_isnull[col];
        }
    } else {
        new_s->tts_values[c->a_claim - 1] = PointerGetDatum(&s->claim); new_s->tts_isnull[c->a_claim - 1] = false;
        new_s->tts_values[c->a_wit - 1] = PointerGetDatum(&s->witness); new_s->tts_isnull[c->a_wit - 1] = false;
        new_s->tts_values[c->a_lin - 1] = s->has_lin ? PointerGetDatum(&s->lineage) : (Datum)0;
        new_s->tts_isnull[c->a_lin - 1] = !s->has_lin;
    }
    new_s->tts_values[c->a_games - 1] = Int64GetDatum(s->games); new_s->tts_isnull[c->a_games - 1] = false;
    new_s->tts_values[c->a_tok - 1] = Int64GetDatum(s->tokens); new_s->tts_isnull[c->a_tok - 1] = false;
    new_s->tts_values[c->a_score - 1] = Float8GetDatum(s->score); new_s->tts_isnull[c->a_score - 1] = false;
    new_s->tts_values[c->a_pos - 1] = s->has_pos ? Int32GetDatum(s->pos) : (Datum)0;
    new_s->tts_isnull[c->a_pos - 1] = !s->has_pos;
    new_s->tts_values[c->a_trust - 1] = Float8GetDatum(s->trust); new_s->tts_isnull[c->a_trust - 1] = false;
    ExecStoreVirtualTuple(new_s);
}

PG_FUNCTION_INFO_V1(laplace_apply_series);
Datum laplace_apply_series(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_apply_series requires distinct UNLOGGED stage tables")));
    Oid pending_oid = PG_GETARG_OID(0);
    Oid delta_oid = PG_GETARG_OID(1);
    if (pending_oid == delta_oid)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_apply_series requires distinct UNLOGGED stage tables")));
    require_stage_kind(pending_oid, false, "laplace_apply_series requires distinct UNLOGGED stage tables");
    require_stage_kind(delta_oid, false, "laplace_apply_series requires distinct UNLOGGED stage tables");
    require_rel_priv(pending_oid, ACL_SELECT | ACL_INSERT | ACL_UPDATE);
    require_rel_priv(delta_oid, ACL_SELECT | ACL_TRUNCATE);

    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));

    Relation delta = table_open(delta_oid, AccessExclusiveLock);
    AttrNumber dc = require_attnum(delta_oid, "claim", native_id, NULL);
    AttrNumber dw = require_attnum(delta_oid, "witness", native_id, NULL);
    AttrNumber dl = require_attnum(delta_oid, "lineage", native_id, NULL);
    AttrNumber dg = require_attnum(delta_oid, "games", InvalidOid, NULL);
    AttrNumber dtok = require_attnum(delta_oid, "tokens", InvalidOid, NULL);
    AttrNumber dsc = require_attnum(delta_oid, "score", InvalidOid, NULL);
    AttrNumber dpos = require_attnum(delta_oid, "position", INT4OID, NULL);
    AttrNumber dtr = require_attnum(delta_oid, "trust", InvalidOid, NULL);
    AttrNumber dstaged = require_attnum(delta_oid, "staged", BOOLOID, NULL);
    Oid tdg = get_atttype(delta_oid, dg), tdt = get_atttype(delta_oid, dtok);
    Oid tdsc = get_atttype(delta_oid, dsc), tdtr = get_atttype(delta_oid, dtr);

    lp_vec(ApplyDeltaRow) rows = {0};
    lp_idmap *changed_claims = lp_idmap_new();
    lp_strmap *staged_map = lp_strmap_sized(sizeof(size_t));
    uint16_t buckets = 0;

    Snapshot snap = GetActiveSnapshot();
    TableScanDesc dscan = table_beginscan(delta, snap, 0, NULL);
    TupleTableSlot *dslot = table_slot_create(delta, NULL);
    while (table_scan_getnextslot(dscan, ForwardScanDirection, dslot)) {
        CHECK_FOR_INTERRUPTS();
        bool nul;
        ApplyDeltaRow r = {0};
        Datum v = slot_getattr(dslot, dc, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"claim\"")));
        memcpy(r.claim.b, DatumGetPointer(v), 16);
        lp_idmap_put(changed_claims, &r.claim, NULL);
        buckets |= (uint16_t)(1u << (r.claim.b[0] >> 4));
        v = slot_getattr(dslot, dw, &nul);
        if (nul) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"witness\"")));
        memcpy(r.witness.b, DatumGetPointer(v), 16);
        v = slot_getattr(dslot, dl, &nul);
        r.has_lin = !nul;
        if (!nul) memcpy(r.lineage.b, DatumGetPointer(v), 16);
        r.games = read_int64_slot(dslot, dg, tdg, &nul);
        r.tokens = read_int64_slot(dslot, dtok, tdt, &nul);
        r.score = read_float8_slot(dslot, dsc, tdsc, &nul);
        v = slot_getattr(dslot, dpos, &nul);
        r.has_pos = !nul;
        if (!nul) r.pos = DatumGetInt32(v);
        r.trust = read_float8_slot(dslot, dtr, tdtr, &nul);
        v = slot_getattr(dslot, dstaged, &nul);
        r.staged = !nul && DatumGetBool(v);

        size_t idx = rows.n;
        lp_push(&rows, r);
        if (r.staged) {
            uint8_t key[49];
            cwl_key_pack(key, &r.claim, &r.witness, r.has_lin, &r.lineage);
            bool fresh = false;
            size_t *slot_idx = lp_strmap_get(staged_map, key, sizeof key, &fresh);
            if (fresh) *slot_idx = idx;
        }
        ExecClearTuple(dslot);
    }
    ExecDropSingleTupleTableSlot(dslot);
    table_endscan(dscan);
    table_close(delta, NoLock);

    if (rows.n > 0) {
        PgRelWriter w;
        pg_writer_open(&w, pending_oid, true);
        AttrNumber ac = require_attnum(pending_oid, "claim", native_id, NULL);
        AttrNumber aw = require_attnum(pending_oid, "witness", native_id, NULL);
        AttrNumber al = require_attnum(pending_oid, "lineage", native_id, NULL);
        AttrNumber ag = require_attnum(pending_oid, "games", INT8OID, NULL);
        AttrNumber atok = require_attnum(pending_oid, "tokens", INT8OID, NULL);
        AttrNumber ascore = require_attnum(pending_oid, "score", FLOAT8OID, NULL);
        AttrNumber apos = require_attnum(pending_oid, "position", INT4OID, NULL);
        AttrNumber atrust = require_attnum(pending_oid, "trust", FLOAT8OID, NULL);

        lp_vec(ApplyUpdateTarget) updates = {0};
        if (lp_strmap_count(staged_map) > 0) {
            Oid idxOid = find_cwl_index(w.rel, ac, aw, al, false);
            if (OidIsValid(idxOid)) {
                Relation idx = index_open(idxOid, AccessShareLock);
                IndexScanDesc iscan = index_beginscan(w.rel, idx, w.snap, NULL, 3, 0);
                for (size_t i = 0; i < rows.n; i++) {
                    if (!rows.v[i].staged) continue;
                    const ApplyDeltaRow *r = &rows.v[i];
                    ScanKeyData sk[3];
                    init_cwl_scankeys(sk, idx, native_id, &r->claim, &r->witness, r->has_lin, &r->lineage);
                    index_rescan(iscan, sk, 3, NULL, 0);
                    while (index_getnext_slot(iscan, ForwardScanDirection, w.old_slot)) {
                        lp_push(&updates, ((ApplyUpdateTarget){ w.old_slot->tts_tid, i }));
                        ExecClearTuple(w.old_slot);
                    }
                }
                index_endscan(iscan);
                index_close(idx, NoLock);
            } else {
                TableScanDesc pscan = table_beginscan(w.rel, w.snap, 0, NULL);
                while (table_scan_getnextslot(pscan, ForwardScanDirection, w.old_slot)) {
                    CHECK_FOR_INTERRUPTS();
                    bool mc, mw, ml;
                    Datum vc = slot_getattr(w.old_slot, ac, &mc);
                    Datum vw = slot_getattr(w.old_slot, aw, &mw);
                    Datum vl = slot_getattr(w.old_slot, al, &ml);
                    if (!mc && !mw) {
                        uint8_t key[49];
                        cwl_key_pack(key, (const lp_id *)DatumGetPointer(vc), (const lp_id *)DatumGetPointer(vw),
                                     !ml, ml ? NULL : (const lp_id *)DatumGetPointer(vl));
                        size_t *match = lp_strmap_lookup(staged_map, key, sizeof key);
                        if (match) lp_push(&updates, ((ApplyUpdateTarget){ w.old_slot->tts_tid, *match }));
                    }
                    ExecClearTuple(w.old_slot);
                }
                table_endscan(pscan);
            }
        }

        ApplyWriteCtx ctx = {
            .a_claim = ac, .a_wit = aw, .a_lin = al, .a_games = ag,
            .a_tok = atok, .a_score = ascore, .a_pos = apos, .a_trust = atrust,
            .preserve_other_cols = true
        };
        for (size_t i = 0; i < updates.n; i++) {
            CHECK_FOR_INTERRUPTS();
            ctx.row = &rows.v[updates.v[i].delta_idx];
            pg_writer_update_tid(&w, &updates.v[i].tid, write_apply_slot, &ctx, true);
        }
        lp_vec_free(&updates);

        bool *assigned = palloc0(sizeof(bool) * (size_t)w.td->natts);
        assigned[ac - 1] = assigned[aw - 1] = assigned[al - 1] = assigned[ag - 1] = true;
        assigned[atok - 1] = assigned[ascore - 1] = assigned[apos - 1] = assigned[atrust - 1] = true;
        ctx.preserve_other_cols = false;
        for (size_t i = 0; i < rows.n; i++) {
            if (rows.v[i].staged) continue;
            CHECK_FOR_INTERRUPTS();
            ExecClearTuple(w.new_slot);
            pg_writer_fill_unset(&w, assigned);
            ctx.row = &rows.v[i];
            write_apply_slot(NULL, w.new_slot, &ctx);
            pg_writer_insert_new_slot(&w);
        }
        pfree(assigned);
        pg_writer_close(&w);
        CommandCounterIncrement();
    }
    lp_strmap_free(staged_map);
    lp_vec_free(&rows);

    int64 affected = attest_stage_core(pending_oid, native_id, changed_claims, buckets);
    lp_idmap_free(changed_claims);
    lp_pg_truncate_rel(delta_oid, InvalidOid);
    PG_RETURN_INT64(affected);
}

typedef struct {
    bool found;
    bool has_lineage;
    lp_id lineage;
    double trust;
} MeasureWitnessMeta;

static void capture_witness_meta(const lp_id *id, const lp_id *lineage, double trust, void *context)
{
    (void)id;
    MeasureWitnessMeta *m = context;
    m->found = true;
    m->has_lineage = (lineage != NULL);
    if (lineage) m->lineage = *lineage;
    m->trust = trust;
}

typedef struct {
    lp_id claim, witness, lineage;
    int64 games, tokens;
    double score, trust;
    int32 pos;
    bool has_lin, has_pos;
    Oid partition;
    ItemPointerData tid;
    bool found;
} MeasureAttestItem;

static int measure_attest_cmp(const void *a, const void *b)
{
    const MeasureAttestItem *x = a, *y = b;
    int c = (x->partition > y->partition) - (x->partition < y->partition);
    if (c) return c;
    c = lp_id_cmp(&x->claim, &y->claim);
    if (c) return c;
    if (x->has_lin != y->has_lin) return (int)x->has_lin - (int)y->has_lin;
    return x->has_lin ? lp_id_cmp(&x->lineage, &y->lineage) : 0;
}

typedef struct {
    const MeasureAttestItem *item;
    AttrNumber ac, aw, al, ag, atok, ascore, apos, atrust;
    bool preserve_identity;
} MeasureFillCtx;

static void fill_attestation_slot(TupleTableSlot *old_s, TupleTableSlot *new_s, void *context)
{
    (void)old_s;
    const MeasureFillCtx *c = context;
    const MeasureAttestItem *m = c->item;
    ExecClearTuple(new_s);
    memset(new_s->tts_isnull, 0, sizeof(bool) * (size_t)new_s->tts_tupleDescriptor->natts);
    for (int col = 0; col < new_s->tts_tupleDescriptor->natts; col++)
        if (TupleDescAttr(new_s->tts_tupleDescriptor, col)->attisdropped) new_s->tts_isnull[col] = true;
    new_s->tts_values[c->ac - 1] = PointerGetDatum(&m->claim);
    new_s->tts_values[c->aw - 1] = PointerGetDatum(&m->witness);
    if (m->has_lin) {
        new_s->tts_values[c->al - 1] = PointerGetDatum(&m->lineage);
    } else {
        new_s->tts_isnull[c->al - 1] = true;
    }
    new_s->tts_values[c->ag - 1] = Int64GetDatum(m->games);
    new_s->tts_values[c->atok - 1] = Int64GetDatum(m->tokens);
    new_s->tts_values[c->ascore - 1] = Float8GetDatum(m->score);
    if (m->has_pos) {
        new_s->tts_values[c->apos - 1] = Int32GetDatum(m->pos);
    } else {
        new_s->tts_isnull[c->apos - 1] = true;
    }
    new_s->tts_values[c->atrust - 1] = Float8GetDatum(m->trust);
    ExecStoreVirtualTuple(new_s);
}

PG_FUNCTION_INFO_V1(laplace_measure_witness);
Datum laplace_measure_witness(PG_FUNCTION_ARGS)
{
    lp_id witness;
    memcpy(witness.b, DatumGetPointer(PG_GETARG_DATUM(0)), sizeof(lp_id));

    MeasureWitnessMeta meta = {0};
    lp_pg_lookup_witnesses(&witness, 1, true, capture_witness_meta, &meta);
    if (!meta.found) PG_RETURN_INT64(0);

    PgContainedSeriesRows series = {0};
    lp_pg_contained_series(&witness, 1, &series);
    if (!series.n) { lp_vec_free(&series); PG_RETURN_INT64(0); }

    Oid att_oid = lp_pg_relation("attestation");
    require_rel_priv(att_oid, ACL_SELECT | ACL_INSERT | ACL_UPDATE);
    Oid native_id = get_atttype(att_oid, get_attnum(att_oid, "claim"));
    Relation parent = table_open(att_oid, AccessShareLock);
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;

    lp_vec(MeasureAttestItem) items = {0};
    for (size_t i = 0; i < series.n; i++) {
        const PgContainedSeriesRow *s = &series.v[i];
        if (!isfinite(s->games) || s->games <= 0 || s->games > 9007199254740992.0) continue;
        int64 games = (int64)llround(s->games);
        int64 tokens = Max((int64)llround(s->tokens), games);
        double score = fmin(1.0, fmax(0.0, s->score / s->games));
        MeasureAttestItem item = {
            .claim = s->claim,
            .witness = witness,
            .games = games,
            .tokens = tokens,
            .score = score,
            .pos = s->position,
            .has_pos = (s->position != 0),
            .has_lin = s->has_dependence ? true : meta.has_lineage,
            .lineage = s->has_dependence ? s->dependence_root : meta.lineage,
            .trust = s->has_dependence ? s->dependence_trust : meta.trust,
            .partition = lp_pg_enrichment_partition(att_oid, pkey, pdesc, PointerGetDatum(&s->claim))
        };
        lp_push(&items, item);
    }
    table_close(parent, NoLock);
    lp_vec_free(&series);
    if (!items.n) { lp_vec_free(&items); PG_RETURN_INT64(0); }

    lp_sort(items.v, items.n, sizeof(*items.v), measure_attest_cmp);
    /* Collapse any duplicate resolved (claim, witness, lineage) if dependence equals source default */
    size_t keep = 0;
    for (size_t i = 0; i < items.n; i++) {
        if (keep > 0 && items.v[keep - 1].partition == items.v[i].partition
            && lp_id_eq(&items.v[keep - 1].claim, &items.v[i].claim)
            && items.v[keep - 1].has_lin == items.v[i].has_lin
            && (!items.v[i].has_lin || lp_id_eq(&items.v[keep - 1].lineage, &items.v[i].lineage))) {
            items.v[keep - 1] = items.v[i];
        } else {
            items.v[keep++] = items.v[i];
        }
    }
    items.n = keep;

    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions("attestation", &parts, &pks, NULL, NULL);
    int64 affected = 0;

    for (size_t begin = 0; begin < items.n;) {
        CHECK_FOR_INTERRUPTS();
        size_t end = begin + 1;
        while (end < items.n && items.v[end].partition == items.v[begin].partition) end++;

        Oid partOid = items.v[begin].partition;
        PgRelWriter w;
        pg_writer_open(&w, partOid, false);
        Oid pkOid = InvalidOid;
        for (int p = 0; p < nparts; p++) if (parts[p] == partOid) { pkOid = pks[p]; break; }
        if (!OidIsValid(pkOid)) pkOid = RelationGetPrimaryKeyIndex(w.rel, false);
        if (!OidIsValid(pkOid)) ereport(ERROR, (errmsg("Native attestation: partition has no primary index")));
        Relation idx = index_open(pkOid, AccessShareLock);

        AttrNumber ac = get_attnum(partOid, "claim");
        AttrNumber aw = get_attnum(partOid, "witness");
        AttrNumber al = get_attnum(partOid, "lineage");
        AttrNumber ag = get_attnum(partOid, "games");
        AttrNumber atok = get_attnum(partOid, "tokens");
        AttrNumber ascore = get_attnum(partOid, "score");
        AttrNumber apos = get_attnum(partOid, "position");
        AttrNumber atrust = get_attnum(partOid, "trust");

        IndexScanDesc iscan = index_beginscan(w.rel, idx, w.snap, NULL, 3, 0);
        for (size_t i = begin; i < end; i++) {
            if (!((i - begin) & 255)) CHECK_FOR_INTERRUPTS();
            MeasureAttestItem *m = &items.v[i];
            ScanKeyData sk[3];
            init_cwl_scankeys(sk, idx, native_id, &m->claim, &m->witness, m->has_lin, &m->lineage);
            index_rescan(iscan, sk, 3, NULL, 0);
            if (index_getnext_slot(iscan, ForwardScanDirection, w.old_slot)) {
                m->tid = w.old_slot->tts_tid;
                m->found = true;
                ExecClearTuple(w.old_slot);
            }
        }
        index_endscan(iscan);
        index_close(idx, NoLock);

        MeasureFillCtx fctx = { NULL, ac, aw, al, ag, atok, ascore, apos, atrust, false };
        for (size_t i = begin; i < end; i++) {
            CHECK_FOR_INTERRUPTS();
            fctx.item = &items.v[i];
            if (items.v[i].found) {
                if (pg_writer_update_tid(&w, &items.v[i].tid, fill_attestation_slot, &fctx, false))
                    affected++;
            } else {
                fill_attestation_slot(NULL, w.new_slot, &fctx);
                pg_writer_insert_new_slot(&w);
                affected++;
            }
        }
        pg_writer_close(&w);
        begin = end;
    }

    lp_vec_free(&items);
    if (affected > 0) CommandCounterIncrement();
    PG_RETURN_INT64(affected);
}

/* ---------------- Zero-SPI stage, sweep, forget, and replay C kernels ---------------- */

typedef struct {
    lp_id id;
    Oid partition;
} PartIdItem;

static int part_id_item_cmp(const void *a, const void *b)
{
    const PartIdItem *x = a, *y = b;
    int c = (x->partition > y->partition) - (x->partition < y->partition);
    return c ? c : lp_id_cmp(&x->id, &y->id);
}

static int64 delete_part_matching_ids(Oid partOid, Oid pkOid, const char *relname, Oid native_id,
                                      const PartIdItem *items, size_t begin, size_t end, size_t array_ids)
{
    PgRelWriter w;
    pg_writer_open(&w, partOid, false);
    if (!OidIsValid(pkOid)) pkOid = RelationGetPrimaryKeyIndex(w.rel, false);
    if (!OidIsValid(pkOid)) ereport(ERROR, (errmsg("Native %s: partition has no primary index", relname)));
    Relation idx = index_open(pkOid, AccessShareLock);
    Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
    RegProcedure eq_proc = get_opcode(eq_op);

    lp_vec(ItemPointerData) tids = {0};
    for (size_t c0 = begin; c0 < end;) {
        CHECK_FOR_INTERRUPTS();
        size_t c1 = Min(end, c0 + array_ids);
        size_t len = c1 - c0;
        Datum *vals = palloc(sizeof(Datum) * len);
        for (size_t k = 0; k < len; k++) vals[k] = PointerGetDatum(&items[c0 + k].id);
        ArrayType *wanted = construct_array(vals, (int)len, native_id, sizeof(lp_id), false, TYPALIGN_CHAR);
        pfree(vals);

        ScanKeyData sk;
        ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, native_id,
                               idx->rd_indcollation[0], eq_proc, PointerGetDatum(wanted));
        IndexScanDesc scan = index_beginscan(w.rel, idx, w.snap, NULL, 1, 0);
        index_rescan(scan, &sk, 1, NULL, 0);
        while (index_getnext_slot(scan, ForwardScanDirection, w.old_slot)) {
            CHECK_FOR_INTERRUPTS();
            lp_push(&tids, w.old_slot->tts_tid);
            ExecClearTuple(w.old_slot);
        }
        index_endscan(scan);
        pfree(wanted);
        c0 = c1;
    }
    index_close(idx, NoLock);

    int64 deleted = 0;
    for (size_t k = 0; k < tids.n; k++) {
        CHECK_FOR_INTERRUPTS();
        if (pg_writer_delete_tid(&w, &tids.v[k]))
            deleted++;
    }
    lp_vec_free(&tids);
    pg_writer_close(&w);
    return deleted;
}

static int64 delete_ids_from_partitioned(const char *relname, const char *col, Oid native_id,
                                         const lp_id *ids, size_t n)
{
    (void)col;
    if (!n) return 0;

    Oid relid = lp_pg_relation(relname);
    Relation parent = table_open(relid, AccessShareLock);
    PartitionKey pkey = RelationGetPartitionKey(parent);
    AttrNumber tier_att = get_attnum(relid, "tier");
    bool by_tier = (pkey && pkey->strategy == PARTITION_STRATEGY_RANGE && pkey->partnatts == 1 &&
                    tier_att != InvalidAttrNumber && pkey->partattrs[0] == tier_att);
    PartitionDesc pdesc = (!by_tier && pkey) ? RelationGetPartitionDesc(parent, true) : NULL;

    PartIdItem *items = palloc_extended(sizeof(*items) * n, MCXT_ALLOC_HUGE);
    for (size_t i = 0; i < n; i++) {
        items[i] = (PartIdItem){
            .id = ids[i],
            .partition = by_tier ? InvalidOid : lp_pg_enrichment_partition(relid, pkey, pdesc, PointerGetDatum(&ids[i]))
        };
    }
    table_close(parent, NoLock);
    lp_sort(items, n, sizeof(*items), part_id_item_cmp);

    size_t unique = 0;
    for (size_t i = 0; i < n; i++) {
        if (!unique || items[unique - 1].partition != items[i].partition ||
            !lp_id_eq(&items[unique - 1].id, &items[i].id))
            items[unique++] = items[i];
    }

    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions(relname, &parts, &pks, NULL, NULL);
    size_t array_ids = (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id);
    int64 deleted = 0;

    if (by_tier) {
        for (int p = 0; p < nparts; p++) {
            CHECK_FOR_INTERRUPTS();
            deleted += delete_part_matching_ids(parts[p], pks[p], relname, native_id, items, 0, unique, array_ids);
        }
    } else {
        for (size_t begin = 0; begin < unique;) {
            CHECK_FOR_INTERRUPTS();
            size_t end = begin + 1;
            while (end < unique && items[end].partition == items[begin].partition) end++;
            Oid partOid = items[begin].partition;
            Oid pkOid = InvalidOid;
            for (int p = 0; p < nparts; p++) if (parts[p] == partOid) { pkOid = pks[p]; break; }
            deleted += delete_part_matching_ids(partOid, pkOid, relname, native_id, items, begin, end, array_ids);
            begin = end;
        }
    }

    pfree(items);
    return deleted;
}

typedef struct {
    const lp_id *chunk_ids;
    const size_t *chunk_idx;
    size_t n;
    bool *keep;
} SweepHolderCtx;

static void sweep_holder_visit(const PgHolderHit *hit, void *context)
{
    SweepHolderCtx *c = context;
    if (hit->ordinal < 1 || (size_t)hit->ordinal > c->n) return;
    size_t i = (size_t)(hit->ordinal - 1);
    if (hit->hub || !lp_id_eq(&hit->entity, &c->chunk_ids[i]))
        c->keep[c->chunk_idx[i]] = true;
}

static int64 sweep_orphan_map(lp_idmap *cand, Oid native_id)
{
    if (!cand || lp_idmap_count(cand) == 0) return 0;

    const Oid *att_parts = NULL;
    int n_att = lp_pg_canonical_partitions("attestation", &att_parts, NULL, NULL, NULL);
    const Oid *path_parts = NULL;
    int n_path = lp_pg_canonical_partitions("physicality", &path_parts, NULL, NULL, NULL);

    /* attestation rows are immutable during sweep; build the witness+lineage reference set once. */
    lp_idmap *attested_refs = lp_idmap_new();
    int64 deleted = 0;
    volatile bool snap_pushed = false;

    PushActiveSnapshot(GetTransactionSnapshot());
    snap_pushed = true;

    PG_TRY(); {
        Snapshot init_snap = GetActiveSnapshot();
        for (int p = 0; p < n_att; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(att_parts[p], AccessShareLock);
            AttrNumber aw = get_attnum(att_parts[p], "witness");
            AttrNumber al = get_attnum(att_parts[p], "lineage");
            TableScanDesc scan = table_beginscan(part, init_snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool nw = false, nl = false;
                Datum dw = slot_getattr(slot, aw, &nw);
                if (!nw) {
                    lp_id wid;
                    memcpy(wid.b, DatumGetPointer(dw), sizeof(lp_id));
                    lp_idmap_put(attested_refs, &wid, NULL);
                }
                Datum dl = slot_getattr(slot, al, &nl);
                if (!nl) {
                    lp_id lid;
                    memcpy(lid.b, DatumGetPointer(dl), sizeof(lp_id));
                    lp_idmap_put(attested_refs, &lid, NULL);
                }
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }

        double total_paths = 0.0;
        for (int p = 0; p < n_path; p++) {
            Relation part = table_open(path_parts[p], AccessShareLock);
            if (part->rd_rel->reltuples > 0) total_paths += part->rd_rel->reltuples;
            table_close(part, NoLock);
        }

        while (lp_idmap_count(cand) > 0) {
            CHECK_FOR_INTERRUPTS();
            size_t ncand = lp_idmap_count(cand);
            bool *keep = palloc0(ncand);
            for (size_t i = 0; i < ncand; i++) {
                if (lp_idmap_find(attested_refs, lp_idmap_key(cand, i)) >= 0)
                    keep[i] = true;
            }

            if (ncand <= 4096 || (double)ncand * 64.0 < total_paths) {
                lp_vec(lp_id) probe_ids = {0};
                lp_vec(size_t) probe_idx = {0};
                for (size_t i = 0; i < ncand; i++) {
                    if (!keep[i]) {
                        lp_push(&probe_ids, *lp_idmap_key(cand, i));
                        lp_push(&probe_idx, i);
                    }
                }
                for (size_t off = 0; off < probe_ids.n; off += 4096) {
                    CHECK_FOR_INTERRUPTS();
                    size_t chunk = Min((size_t)4096, probe_ids.n - off);
                    SweepHolderCtx hctx = {
                        .chunk_ids = probe_ids.v + off,
                        .chunk_idx = probe_idx.v + off,
                        .n = chunk,
                        .keep = keep
                    };
                    lp_pg_holders_direct(probe_ids.v + off, (int)chunk, NULL, 0,
                                         true, -1, 2, 3, 0, 0, sweep_holder_visit, &hctx);
                }
                lp_vec_free(&probe_ids);
                lp_vec_free(&probe_idx);
            } else {
                Snapshot rsnap = GetActiveSnapshot();
                MemoryContext old_ctx = CurrentMemoryContext;
                MemoryContext row_ctx = AllocSetContextCreate(old_ctx, "laplace_sweep_row", ALLOCSET_DEFAULT_SIZES);
                for (int p = 0; p < n_path; p++) {
                    CHECK_FOR_INTERRUPTS();
                    Relation part = table_open(path_parts[p], AccessShareLock);
                    AttrNumber ae = get_attnum(path_parts[p], "entity");
                    AttrNumber ap = get_attnum(path_parts[p], "path");
                    TableScanDesc scan = table_beginscan(part, rsnap, 0, NULL);
                    TupleTableSlot *slot = table_slot_create(part, NULL);
                    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                        CHECK_FOR_INTERRUPTS();
                        bool ne = false, np = false;
                        Datum de = slot_getattr(slot, ae, &ne);
                        Datum dp = slot_getattr(slot, ap, &np);
                        if (!ne && !np) {
                            const lp_id *ent = (const lp_id *)DatumGetPointer(de);
                            MemoryContextSwitchTo(row_ctx);
                            int nv = 0;
                            lp_id *keys = lp_pg_path_keys(lp_pg_path_ewkb(dp), &nv);
                            for (int k = 0; k < nv; k++) {
                                if (lp_id_eq(ent, &keys[k])) continue;
                                int64 pos = lp_idmap_find(cand, &keys[k]);
                                if (pos >= 0) keep[pos] = true;
                            }
                            MemoryContextSwitchTo(old_ctx);
                            MemoryContextReset(row_ctx);
                        }
                        ExecClearTuple(slot);
                    }
                    ExecDropSingleTupleTableSlot(slot);
                    table_endscan(scan);
                    table_close(part, NoLock);
                }
                MemoryContextDelete(row_ctx);
            }

            lp_vec(lp_id) to_delete = {0};
            lp_vec(lp_id) surviving = {0};
            for (size_t i = 0; i < ncand; i++) {
                if (keep[i]) lp_push(&surviving, *lp_idmap_key(cand, i));
                else lp_push(&to_delete, *lp_idmap_key(cand, i));
            }
            pfree(keep);

            if (!to_delete.n) {
                lp_vec_free(&to_delete);
                lp_vec_free(&surviving);
                break;
            }

            delete_ids_from_partitioned("consensus", "claim", native_id, to_delete.v, to_delete.n);
            delete_ids_from_partitioned("physicality", "entity", native_id, to_delete.v, to_delete.n);
            deleted += delete_ids_from_partitioned("entity", "id", native_id, to_delete.v, to_delete.n);
            lp_vec_free(&to_delete);

            lp_idmap_clear(cand);
            for (size_t i = 0; i < surviving.n; i++)
                lp_idmap_put(cand, &surviving.v[i], NULL);
            lp_vec_free(&surviving);

            CommandCounterIncrement();
            PopActiveSnapshot();
            snap_pushed = false;
            PushActiveSnapshot(GetTransactionSnapshot());
            snap_pushed = true;
        }
    } PG_FINALLY(); {
        if (snap_pushed) PopActiveSnapshot();
        lp_idmap_free(attested_refs);
    } PG_END_TRY();

    return deleted;
}

PG_FUNCTION_INFO_V1(laplace_sweep_orphans);
Datum laplace_sweep_orphans(PG_FUNCTION_ARGS)
{
    Oid att_oid = lp_pg_relation("attestation");
    Oid con_oid = lp_pg_relation("consensus");
    Oid path_oid = lp_pg_relation("physicality");
    Oid ent_oid = lp_pg_relation("entity");
    require_rel_priv(att_oid, ACL_SELECT);
    require_rel_priv(con_oid, ACL_SELECT | ACL_DELETE);
    require_rel_priv(path_oid, ACL_SELECT | ACL_DELETE);
    require_rel_priv(ent_oid, ACL_SELECT | ACL_DELETE);

    DirectFunctionCall2(pg_advisory_xact_lock_int4, Int32GetDatum(1279348803), Int32GetDatum(-2));
    LockRelationOid(att_oid, ShareRowExclusiveLock);

    Oid native_id = get_atttype(ent_oid, get_attnum(ent_oid, "id"));
    lp_idmap *cand = lp_idmap_new();
    if (PG_NARGS() > 0 && !PG_ARGISNULL(0)) {
        int nc = 0;
        lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nc);
        for (int i = 0; i < nc; i++) lp_idmap_put(cand, &ids[i], NULL);
        pfree(ids);
    } else {
        const Oid *ent_parts = NULL;
        int n_ent = lp_pg_canonical_partitions("entity", &ent_parts, NULL, NULL, NULL);
        Snapshot snap = GetActiveSnapshot();
        for (int p = 0; p < n_ent; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(ent_parts[p], AccessShareLock);
            AttrNumber aid = get_attnum(ent_parts[p], "id");
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool nul = false;
                Datum dv = slot_getattr(slot, aid, &nul);
                if (!nul) {
                    lp_id id;
                    memcpy(id.b, DatumGetPointer(dv), sizeof(lp_id));
                    lp_idmap_put(cand, &id, NULL);
                }
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }
    }

    int64 deleted = lp_idmap_count(cand) ? sweep_orphan_map(cand, native_id) : 0;
    lp_idmap_free(cand);
    PG_RETURN_INT64(deleted);
}

static void forget_collect_wit(const lp_id *id, const lp_id *lineage, double trust, void *context)
{
    (void)lineage; (void)trust;
    lp_vec(lp_id) *out = context;
    lp_push(out, *id);
}

typedef struct {
    lp_idmap *id_map;
    lp_idmap *rec_map;
    lp_idmap *witness_started_recs;
    lp_idmap *roots;
    bool invert;
} ForgetWitSelectCtx;

static void forget_wit_path_visit(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    ForgetWitSelectCtx *c = context;
    bytea *ewkb = lp_pg_path_ewkb(path);
    lp_pg_path_ends ends = lp_pg_path_endpoints(ewkb);
    pfree(ewkb);

    bool matches_id = (lp_idmap_find(c->id_map, id) >= 0);
    bool matches_rec = (ends.found && lp_idmap_find(c->rec_map, &ends.first) >= 0);
    if (ends.found && lp_idmap_find(c->rec_map, &ends.first) >= 0)
        lp_idmap_put(c->witness_started_recs, &ends.first, NULL);

    if ((matches_id || matches_rec) != c->invert)
        lp_idmap_put(c->roots, id, NULL);
}

static void forget_rec_holder_visit(const PgHolderHit *hit, void *context)
{
    ForgetWitSelectCtx *c = context;
    bytea *ewkb = lp_pg_path_ewkb(hit->path);
    lp_pg_path_ends ends = lp_pg_path_endpoints(ewkb);
    pfree(ewkb);
    if (ends.found && lp_idmap_find(c->rec_map, &ends.first) >= 0 &&
        lp_idmap_find(c->witness_started_recs, &ends.first) < 0)
        lp_idmap_put(c->roots, &hit->entity, NULL);
}

typedef struct {
    lp_idmap *seen;
    lp_idmap *next;
    lp_idmap *claims;
    lp_mask claim_bit;
} ForgetBfsCtx;

static void forget_bfs_visit(const lp_id *id, Datum path, Datum mask, void *context)
{
    ForgetBfsCtx *c = context;
    bytea *ewkb = lp_pg_path_ewkb(path);
    int nv = 0;
    lp_id *keys = lp_pg_path_keys(ewkb, &nv);
    pfree(ewkb);
    for (int i = 0; i < nv; i++) {
        bool fresh = false;
        lp_idmap_put(c->seen, &keys[i], &fresh);
        if (fresh) lp_idmap_put(c->next, &keys[i], NULL);
    }
    if (keys) pfree(keys);

    if ((lp_pg_mask_native(mask) & c->claim_bit) != 0 && lp_path_len(geo_of(path).p) >= 2)
        lp_idmap_put(c->claims, id, NULL);
}

typedef struct {
    lp_id id;
    Oid partition;
} PartProbeItem;

static int part_probe_cmp(const void *a, const void *b)
{
    const PartProbeItem *x = a, *y = b;
    int c = (x->partition > y->partition) - (x->partition < y->partition);
    return c ? c : lp_id_cmp(&x->id, &y->id);
}

static void collect_present_consensus_claims(const lp_id *ids, size_t n, Oid native_id, lp_idmap *present_out)
{
    if (!n) return;

    Oid con_oid = lp_pg_relation("consensus");
    Relation parent = table_open(con_oid, AccessShareLock);
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;

    PartProbeItem *items = palloc_extended(sizeof(*items) * n, MCXT_ALLOC_HUGE);
    for (size_t i = 0; i < n; i++) {
        items[i] = (PartProbeItem){
            .id = ids[i],
            .partition = lp_pg_enrichment_partition(con_oid, pkey, pdesc, PointerGetDatum(&ids[i]))
        };
    }
    table_close(parent, NoLock);
    lp_sort(items, n, sizeof(*items), part_probe_cmp);

    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions("consensus", &parts, &pks, NULL, NULL);
    size_t array_ids = (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id);
    Snapshot snap = GetActiveSnapshot();

    for (size_t begin = 0; begin < n;) {
        CHECK_FOR_INTERRUPTS();
        size_t end = begin + 1;
        while (end < n && items[end].partition == items[begin].partition) end++;
        if (end - begin > array_ids) end = begin + array_ids;
        size_t len = end - begin;

        Oid partOid = items[begin].partition;
        Relation part = table_open(partOid, AccessShareLock);
        Oid pkOid = InvalidOid;
        for (int p = 0; p < nparts; p++) if (parts[p] == partOid) { pkOid = pks[p]; break; }
        if (!OidIsValid(pkOid)) pkOid = RelationGetPrimaryKeyIndex(part, false);
        if (!OidIsValid(pkOid)) ereport(ERROR, (errmsg("Native consensus: partition has no primary index")));
        Relation idx = index_open(pkOid, AccessShareLock);
        AttrNumber ac = get_attnum(partOid, "claim");

        Datum *vals = palloc(sizeof(Datum) * len);
        for (size_t k = 0; k < len; k++) vals[k] = PointerGetDatum(&items[begin + k].id);
        ArrayType *wanted = construct_array(vals, (int)len, native_id, sizeof(lp_id), false, TYPALIGN_CHAR);
        pfree(vals);

        Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
        ScanKeyData sk;
        ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, native_id,
                               idx->rd_indcollation[0], get_opcode(eq_op), PointerGetDatum(wanted));
        IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 1, 0);
        index_rescan(scan, &sk, 1, NULL, 0);
        TupleTableSlot *slot = table_slot_create(part, NULL);
        while (index_getnext_slot(scan, ForwardScanDirection, slot)) {
            bool nul = false;
            Datum vc = slot_getattr(slot, ac, &nul);
            if (!nul) lp_idmap_put(present_out, (const lp_id *)DatumGetPointer(vc), NULL);
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);
        index_endscan(scan);
        pfree(wanted);
        index_close(idx, NoLock);
        table_close(part, NoLock);
        begin = end;
    }
    pfree(items);
}

PG_FUNCTION_INFO_V1(laplace_forget_witnesses);
Datum laplace_forget_witnesses(PG_FUNCTION_ARGS)
{
    Oid att_oid = lp_pg_relation("attestation");
    Oid con_oid = lp_pg_relation("consensus");
    Oid path_oid = lp_pg_relation("physicality");
    Oid ent_oid = lp_pg_relation("entity");
    require_rel_priv(att_oid, ACL_SELECT | ACL_DELETE);
    require_rel_priv(con_oid, ACL_SELECT | ACL_INSERT | ACL_UPDATE | ACL_DELETE);
    require_rel_priv(path_oid, ACL_SELECT | ACL_DELETE);
    require_rel_priv(ent_oid, ACL_SELECT | ACL_DELETE);

    DirectFunctionCall2(pg_advisory_xact_lock_int4, Int32GetDatum(1279348803), Int32GetDatum(-2));
    LockRelationOid(att_oid, ShareRowExclusiveLock);

    Oid native_id = get_atttype(ent_oid, get_attnum(ent_oid, "id"));
    lp_idmap *id_map = lp_idmap_new();
    lp_idmap *rec_map = lp_idmap_new();
    if (PG_NARGS() > 0 && !PG_ARGISNULL(0)) {
        int n = 0; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
        for (int i = 0; i < n; i++) lp_idmap_put(id_map, &ids[i], NULL);
        pfree(ids);
    }
    lp_vec(lp_id) rec_vec = {0};
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1)) {
        int n = 0; lp_id *recs = ids_of(PG_GETARG_ARRAYTYPE_P(1), &n);
        for (int i = 0; i < n; i++) {
            bool fresh = false;
            lp_idmap_put(rec_map, &recs[i], &fresh);
            if (fresh) lp_push(&rec_vec, recs[i]);
        }
        pfree(recs);
    }
    bool invert = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_BOOL(2) : false;
    bool has_exclude = (PG_NARGS() > 3 && !PG_ARGISNULL(3));
    lp_id exclude_trunk = {0};
    if (has_exclude) memcpy(exclude_trunk.b, DatumGetPointer(PG_GETARG_DATUM(3)), sizeof(lp_id));

    lp_vec(lp_id) attested_wits = {0};
    lp_pg_scan_all_witnesses(forget_collect_wit, &attested_wits);

    lp_idmap *witness_started_recs = lp_idmap_new();
    lp_idmap *roots = lp_idmap_new();
    ForgetWitSelectCtx sctx = {
        .id_map = id_map,
        .rec_map = rec_map,
        .witness_started_recs = witness_started_recs,
        .roots = roots,
        .invert = invert
    };
    if (attested_wits.n > 0)
        lp_pg_lookup_paths(attested_wits.v, attested_wits.n, false, forget_wit_path_visit, &sctx);
    lp_vec_free(&attested_wits);

    if (!invert && rec_vec.n > 0)
        lp_pg_holders_direct(rec_vec.v, (int)rec_vec.n, NULL, 0, false, -1, -1, 3, 0, 1, forget_rec_holder_visit, &sctx);

    lp_vec_free(&rec_vec);
    lp_idmap_free(witness_started_recs);
    lp_idmap_free(id_map);
    lp_idmap_free(rec_map);

    lp_vec(lp_id) root_list = {0};
    for (size_t i = 0; i < lp_idmap_count(roots); i++) {
        const lp_id *r = lp_idmap_key(roots, i);
        if (has_exclude && lp_id_eq(r, &exclude_trunk)) continue;
        lp_push(&root_list, *r);
    }
    lp_idmap_free(roots);

    int64 wit_removed = (int64)root_list.n;
    int64 ent_removed = 0, stand_recomputed = 0, orphaned = 0;

    if (root_list.n > 0) {
        /* 1. Traverse downward structural closure from selected witness roots. */
        lp_idmap *seen = lp_idmap_new();
        lp_idmap *frontier = lp_idmap_new();
        lp_idmap *valid_claims = lp_idmap_new();
        for (size_t i = 0; i < root_list.n; i++) {
            lp_idmap_put(seen, &root_list.v[i], NULL);
            lp_idmap_put(frontier, &root_list.v[i], NULL);
        }
        lp_mask claim_bit = lp_pg_kind_mask("claim");
        while (lp_idmap_count(frontier) > 0) {
            CHECK_FOR_INTERRUPTS();
            lp_idmap *next = lp_idmap_new();
            ForgetBfsCtx bctx = { .seen = seen, .next = next, .claims = valid_claims, .claim_bit = claim_bit };
            lp_pg_lookup_paths(lp_idmap_keys(frontier), lp_idmap_count(frontier), true, forget_bfs_visit, &bctx);
            lp_idmap_free(frontier);
            frontier = next;
        }
        lp_idmap_free(frontier);

        /* Keep only candidates present in entity (matching JOIN entity in forget.c). */
        size_t nseen = lp_idmap_count(seen);
        const lp_id *seen_keys = lp_idmap_keys(seen);
        bool *present = palloc0(nseen ? nseen : 1);
        if (nseen > 0) lp_pg_present(seen_keys, nseen, present);
        lp_idmap *candidates = lp_idmap_new();
        for (size_t i = 0; i < nseen; i++)
            if (present[i]) lp_idmap_put(candidates, &seen_keys[i], NULL);
        pfree(present);
        lp_idmap_free(seen);

        /* Also inspect any consensus claims among candidates. */
        lp_idmap *cons_in_cand = lp_idmap_new();
        collect_present_consensus_claims(lp_idmap_keys(candidates), lp_idmap_count(candidates), native_id, cons_in_cand);
        lp_vec(lp_id) stale_cons = {0};
        for (size_t i = 0; i < lp_idmap_count(cons_in_cand); i++) {
            const lp_id *cid = lp_idmap_key(cons_in_cand, i);
            if (lp_idmap_find(valid_claims, cid) < 0)
                lp_push(&stale_cons, *cid);
        }
        lp_idmap_free(cons_in_cand);

        /* Filter valid_claims to those present in candidates. */
        lp_idmap *claims_to_replay = lp_idmap_new();
        uint16_t claim_buckets = 0;
        for (size_t i = 0; i < lp_idmap_count(valid_claims); i++) {
            const lp_id *cid = lp_idmap_key(valid_claims, i);
            if (lp_idmap_find(candidates, cid) >= 0) {
                lp_idmap_put(claims_to_replay, cid, NULL);
                claim_buckets |= (uint16_t)(1u << (cid->b[0] >> 4));
            }
        }
        lp_idmap_free(valid_claims);

        /* 2. Delete attestation rows of the forgotten witness roots. */
        lp_idmap *root_map = lp_idmap_new();
        for (size_t i = 0; i < root_list.n; i++) lp_idmap_put(root_map, &root_list.v[i], NULL);

        const Oid *att_parts = NULL, *att_aux = NULL;
        int n_att = lp_pg_canonical_partitions("attestation", &att_parts, NULL, &att_aux, NULL);
        for (int p = 0; p < n_att; p++) {
            CHECK_FOR_INTERRUPTS();
            PgRelWriter w;
            pg_writer_open(&w, att_parts[p], false);
            AttrNumber aw = get_attnum(att_parts[p], "witness");
            lp_vec(ItemPointerData) tids = {0};

            if (OidIsValid(att_aux[p])) {
                Relation idx = index_open(att_aux[p], AccessShareLock);
                Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
                RegProcedure eq_proc = get_opcode(eq_op);
                IndexScanDesc scan = index_beginscan(w.rel, idx, w.snap, NULL, 1, 0);
                for (size_t i = 0; i < root_list.n; i++) {
                    CHECK_FOR_INTERRUPTS();
                    ScanKeyData sk;
                    ScanKeyEntryInitialize(&sk, 0, 1, BTEqualStrategyNumber, native_id,
                                           idx->rd_indcollation[0], eq_proc, PointerGetDatum(&root_list.v[i]));
                    index_rescan(scan, &sk, 1, NULL, 0);
                    while (index_getnext_slot(scan, ForwardScanDirection, w.old_slot)) {
                        lp_push(&tids, w.old_slot->tts_tid);
                        ExecClearTuple(w.old_slot);
                    }
                }
                index_endscan(scan);
                index_close(idx, NoLock);
            } else {
                TableScanDesc scan = table_beginscan(w.rel, w.snap, 0, NULL);
                while (table_scan_getnextslot(scan, ForwardScanDirection, w.old_slot)) {
                    bool nw = false;
                    Datum dw = slot_getattr(w.old_slot, aw, &nw);
                    if (!nw && lp_idmap_find(root_map, (const lp_id *)DatumGetPointer(dw)) >= 0)
                        lp_push(&tids, w.old_slot->tts_tid);
                    ExecClearTuple(w.old_slot);
                }
                table_endscan(scan);
            }

            for (size_t k = 0; k < tids.n; k++) {
                CHECK_FOR_INTERRUPTS();
                pg_writer_delete_tid(&w, &tids.v[k]);
            }
            lp_vec_free(&tids);
            pg_writer_close(&w);
        }
        lp_idmap_free(root_map);

        if (stale_cons.n > 0)
            delete_ids_from_partitioned("consensus", "claim", native_id, stale_cons.v, stale_cons.n);
        lp_vec_free(&stale_cons);

        CommandCounterIncrement();

        /* 3. Recompute consensus standings for affected claims while closures are still intact. */
        size_t nclaims = lp_idmap_count(claims_to_replay);
        if (nclaims > 0) {
            attest_stage_core(InvalidOid, native_id, claims_to_replay, claim_buckets);
            CommandCounterIncrement();

            lp_idmap *after_cons = lp_idmap_new();
            PushActiveSnapshot(GetTransactionSnapshot());
            collect_present_consensus_claims(lp_idmap_keys(claims_to_replay), nclaims, native_id, after_cons);
            PopActiveSnapshot();
            stand_recomputed = (int64)lp_idmap_count(after_cons);
            orphaned = (int64)nclaims - stand_recomputed;
            lp_idmap_free(after_cons);
        }
        lp_idmap_free(claims_to_replay);

        /* 4. Sweep newly orphaned closure entities. */
        ent_removed = sweep_orphan_map(candidates, native_id);
        lp_idmap_free(candidates);
    }

    lp_vec_free(&root_list);

    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_forget_witnesses requires composite result")));
    Datum vals[4] = {
        Int64GetDatum(wit_removed),
        Int64GetDatum(ent_removed),
        Int64GetDatum(stand_recomputed),
        Int64GetDatum(orphaned)
    };
    bool nulls[4] = { false, false, false, false };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), vals, nulls)));
}

typedef struct {
    lp_standing st;
    bool seen_after;
} ReplayBeforeEntry;

PG_FUNCTION_INFO_V1(laplace_replay_verify);
Datum laplace_replay_verify(PG_FUNCTION_ARGS)
{
    bool do_apply = (PG_NARGS() > 0 && !PG_ARGISNULL(0)) ? PG_GETARG_BOOL(0) : false;

    Oid path_oid = lp_pg_relation("physicality");
    Oid att_oid = lp_pg_relation("attestation");
    Oid con_oid = lp_pg_relation("consensus");
    require_rel_priv(path_oid, ACL_SELECT);
    require_rel_priv(att_oid, ACL_SELECT);
    require_rel_priv(con_oid, do_apply ? (ACL_SELECT | ACL_INSERT | ACL_UPDATE | ACL_DELETE) : ACL_SELECT);

    if (do_apply) {
        for (int h = 0; h < 16; h++)
            DirectFunctionCall2(pg_advisory_xact_lock_int4, Int32GetDatum(1279348803), Int32GetDatum(h));
    }

    Oid native_id = get_atttype(path_oid, get_attnum(path_oid, "entity"));
    lp_mask claim_bit = lp_pg_kind_mask("claim");

    lp_idmap *before = lp_idmap_sized(sizeof(ReplayBeforeEntry));
    const Oid *con_parts = NULL;
    int n_con = lp_pg_canonical_partitions("consensus", &con_parts, NULL, NULL, NULL);
    Snapshot snap = GetActiveSnapshot();
    for (int p = 0; p < n_con; p++) {
        CHECK_FOR_INTERRUPTS();
        Relation part = table_open(con_parts[p], AccessShareLock);
        AttrNumber ac = get_attnum(con_parts[p], "claim");
        AttrNumber ar = get_attnum(con_parts[p], "rating");
        AttrNumber ad = get_attnum(con_parts[p], "deviation");
        AttrNumber av = get_attnum(con_parts[p], "volatility");
        AttrNumber am = get_attnum(con_parts[p], "matches");
        TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
        TupleTableSlot *slot = table_slot_create(part, NULL);
        while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool nc = false, nr = false, nd = false, nv = false, nm = false;
            Datum dc = slot_getattr(slot, ac, &nc);
            Datum dr = slot_getattr(slot, ar, &nr);
            Datum dd = slot_getattr(slot, ad, &nd);
            Datum dv = slot_getattr(slot, av, &nv);
            Datum dm = slot_getattr(slot, am, &nm);
            if (!nc && !nr && !nd && !nv && !nm) {
                lp_id cid;
                memcpy(cid.b, DatumGetPointer(dc), sizeof(lp_id));
                ReplayBeforeEntry *e = lp_idmap_get(before, &cid, NULL);
                e->st.rating = DatumGetFloat8(dr);
                e->st.deviation = DatumGetFloat8(dd);
                e->st.volatility = DatumGetFloat8(dv);
                int64 m = DatumGetInt64(dm);
                e->st.matches = (uint64_t)(m >= 0 ? m : 0);
                e->seen_after = false;
            }
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);
        table_endscan(scan);
        table_close(part, NoLock);
    }

    lp_idmap *valid_claims = lp_idmap_new();
    const Oid *path_parts = NULL;
    int n_path = lp_pg_canonical_partitions("physicality", &path_parts, NULL, NULL, NULL);
    MemoryContext old_ctx = CurrentMemoryContext;
    MemoryContext row_ctx = AllocSetContextCreate(old_ctx, "laplace_replay_row", ALLOCSET_DEFAULT_SIZES);
    for (int p = 0; p < n_path; p++) {
        CHECK_FOR_INTERRUPTS();
        Relation part = table_open(path_parts[p], AccessShareLock);
        AttrNumber ae = get_attnum(path_parts[p], "entity");
        AttrNumber ap = get_attnum(path_parts[p], "path");
        AttrNumber am = get_attnum(path_parts[p], "mask");
        TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
        TupleTableSlot *slot = table_slot_create(part, NULL);
        while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool ne = false, np = false, nm = false;
            Datum de = slot_getattr(slot, ae, &ne);
            Datum dm = slot_getattr(slot, am, &nm);
            if (!ne && !nm && (lp_pg_mask_native(dm) & claim_bit) != 0) {
                Datum dp = slot_getattr(slot, ap, &np);
                if (!np) {
                    MemoryContextSwitchTo(row_ctx);
                    size_t len = lp_path_len(geo_of(dp).p);
                    MemoryContextSwitchTo(old_ctx);
                    MemoryContextReset(row_ctx);
                    if (len >= 2) {
                        lp_id cid;
                        memcpy(cid.b, DatumGetPointer(de), sizeof(lp_id));
                        lp_idmap_put(valid_claims, &cid, NULL);
                    }
                }
            }
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);
        table_endscan(scan);
        table_close(part, NoLock);
    }
    MemoryContextDelete(row_ctx);

    PgEvaluatedSeries eval = {0};
    if (lp_idmap_count(valid_claims) > 0)
        eval_series_for_claims(InvalidOid, native_id, valid_claims, CurrentMemoryContext, &eval);
    lp_idmap_free(valid_claims);

    int64 identical = 0, different = 0;
    lp_vec(ConsensusWriteItem) write_items = {0};
    for (size_t i = 0; i < eval.count; i++) {
        if (!eval.present[i]) continue;
        ReplayBeforeEntry *b = lp_idmap_lookup(before, &eval.claims[i]);
        if (b) {
            b->seen_after = true;
            if (memcmp(&b->st.rating, &eval.standings[i].rating, sizeof(double)) == 0 &&
                memcmp(&b->st.deviation, &eval.standings[i].deviation, sizeof(double)) == 0 &&
                memcmp(&b->st.volatility, &eval.standings[i].volatility, sizeof(double)) == 0 &&
                b->st.matches == eval.standings[i].matches) {
                identical++;
                continue;
            }
        }
        different++;
        if (do_apply) {
            ConsensusWriteItem witem = {
                .claim = eval.claims[i],
                .standing = eval.standings[i],
                .present = true,
                .partition = InvalidOid,
                .orig_idx = i
            };
            lp_push(&write_items, witem);
        }
    }

    lp_vec(lp_id) delete_claims = {0};
    for (size_t i = 0; i < lp_idmap_count(before); i++) {
        const ReplayBeforeEntry *b = lp_idmap_at(before, i);
        if (!b->seen_after) {
            different++;
            if (do_apply) lp_push(&delete_claims, *lp_idmap_key(before, i));
        }
    }
    lp_idmap_free(before);

    if (do_apply && different > 0) {
        if (delete_claims.n > 0)
            delete_ids_from_partitioned("consensus", "claim", native_id, delete_claims.v, delete_claims.n);
        if (write_items.n > 0)
            write_consensus_items(write_items.v, write_items.n, native_id);
    }

    lp_vec_free(&delete_claims);
    lp_vec_free(&write_items);
    if (eval.claims) pfree(eval.claims);
    if (eval.standings) pfree(eval.standings);
    if (eval.present) pfree(eval.present);

    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_replay_verify requires composite result")));
    Datum vals[3] = {
        Int64GetDatum(identical),
        Int64GetDatum(different),
        BoolGetDatum(do_apply)
    };
    bool nulls[3] = { false, false, false };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), vals, nulls)));
}

PG_FUNCTION_INFO_V1(laplace_stage_flush_claims);
Datum laplace_stage_flush_claims(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_flush_claims requires distinct UNLOGGED stage tables")));
    PG_RETURN_INT64(accumulate_series_core(PG_GETARG_OID(0), PG_GETARG_OID(1), PG_GETARG_OID(2),
                                           "laplace_stage_flush_claims"));
}

typedef struct {
    AttrNumber aw;
    lp_id witness;
} RewriteWitnessCtx;

static void fill_rewrite_witness_slot(TupleTableSlot *old_s, TupleTableSlot *new_s, void *context)
{
    const RewriteWitnessCtx *c = context;
    ExecClearTuple(new_s);
    int natts = new_s->tts_tupleDescriptor->natts;
    for (int col = 0; col < natts; col++) {
        new_s->tts_values[col] = old_s->tts_values[col];
        new_s->tts_isnull[col] = old_s->tts_isnull[col];
    }
    new_s->tts_values[c->aw - 1] = PointerGetDatum(&c->witness);
    new_s->tts_isnull[c->aw - 1] = false;
    ExecStoreVirtualTuple(new_s);
}

PG_FUNCTION_INFO_V1(laplace_stage_rewrite_witness);
Datum laplace_stage_rewrite_witness(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_rewrite_witness requires an UNLOGGED stage table and witness ID")));

    Oid pending_oid = PG_GETARG_OID(0);
    require_stage_kind(pending_oid, false, "laplace_stage_rewrite_witness requires an UNLOGGED stage table");
    require_rel_priv(pending_oid, ACL_SELECT | ACL_UPDATE);

    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));
    AttrNumber aw = require_attnum(pending_oid, "witness", native_id, NULL);

    lp_id target;
    memcpy(target.b, DatumGetPointer(PG_GETARG_DATUM(1)), sizeof(lp_id));

    PgRelWriter w;
    pg_writer_open(&w, pending_oid, false);
    lp_vec(ItemPointerData) tids = {0};
    TableScanDesc scan = table_beginscan(w.rel, w.snap, 0, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, w.old_slot)) {
        CHECK_FOR_INTERRUPTS();
        bool nul = false;
        Datum dv = slot_getattr(w.old_slot, aw, &nul);
        if (nul || memcmp(DatumGetPointer(dv), target.b, sizeof(lp_id)) != 0)
            lp_push(&tids, w.old_slot->tts_tid);
        ExecClearTuple(w.old_slot);
    }
    table_endscan(scan);

    int64 updated = 0;
    RewriteWitnessCtx rw_ctx = { .aw = aw, .witness = target };
    for (size_t i = 0; i < tids.n; i++) {
        CHECK_FOR_INTERRUPTS();
        if (pg_writer_update_tid(&w, &tids.v[i], fill_rewrite_witness_slot, &rw_ctx, true))
            updated++;
    }
    lp_vec_free(&tids);
    pg_writer_close(&w);
    if (updated > 0) CommandCounterIncrement();
    PG_RETURN_INT64(updated);
}

typedef struct {
    lp_id id, lineage;
    double trust;
    bool has_lin;
} RestoreWitness;

typedef struct {
    lp_id claim, witness, lineage;
    int64 games, tokens;
    double wins, dep_trust, wit_trust;
    int32 pos;
    bool has_lin, has_pos, has_dep_trust;
} RestoreGroup;

PG_FUNCTION_INFO_V1(laplace_stage_restore_pruned);
Datum laplace_stage_restore_pruned(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) PG_RETURN_INT64(0);
    int nfiles = 0;
    lp_id *file_ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nfiles);
    if (!nfiles) { pfree(file_ids); PG_RETURN_INT64(0); }

    if (PG_ARGISNULL(1) || PG_ARGISNULL(2)) {
        pfree(file_ids);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_restore_pruned requires distinct UNLOGGED stage tables")));
    }
    Oid wit_oid = PG_GETARG_OID(1);
    Oid delta_oid = PG_GETARG_OID(2);
    if (wit_oid == delta_oid) {
        pfree(file_ids);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_restore_pruned requires distinct UNLOGGED stage tables")));
    }
    require_stage_kind(wit_oid, true, "laplace_stage_restore_pruned requires UNLOGGED stage tables");
    require_stage_kind(delta_oid, false, "laplace_stage_restore_pruned requires UNLOGGED stage tables");
    require_rel_priv(wit_oid, ACL_SELECT);
    require_rel_priv(delta_oid, ACL_SELECT | ACL_INSERT);

    bool has_single = (PG_NARGS() > 3 && !PG_ARGISNULL(3));
    lp_id single_wit = {0};
    if (has_single) memcpy(single_wit.b, DatumGetPointer(PG_GETARG_DATUM(3)), sizeof(lp_id));

    Oid physicality = lp_pg_relation("physicality");
    Oid native_id = get_atttype(physicality, get_attnum(physicality, "entity"));

    lp_idmap *wmap = lp_idmap_sized(sizeof(RestoreWitness));
    lp_vec(RestoreWitness) wlist = {0};
    Relation wrel = table_open(wit_oid, AccessShareLock);
    AttrNumber w_id = require_attnum(wit_oid, "id", native_id, NULL);
    AttrNumber w_lin = require_attnum(wit_oid, "lineage", native_id, NULL);
    AttrNumber w_tr = require_attnum(wit_oid, "trust", InvalidOid, NULL);
    Oid tw_tr = get_atttype(wit_oid, w_tr);
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc wscan = table_beginscan(wrel, snap, 0, NULL);
    TupleTableSlot *wslot = table_slot_create(wrel, NULL);
    while (table_scan_getnextslot(wscan, ForwardScanDirection, wslot)) {
        CHECK_FOR_INTERRUPTS();
        bool ni = false, nl = false, nt = false;
        RestoreWitness rw = {0};
        Datum di = slot_getattr(wslot, w_id, &ni);
        if (ni) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"id\"")));
        memcpy(rw.id.b, DatumGetPointer(di), sizeof(lp_id));
        Datum dl = slot_getattr(wslot, w_lin, &nl);
        rw.has_lin = !nl;
        if (!nl) memcpy(rw.lineage.b, DatumGetPointer(dl), sizeof(lp_id));
        rw.trust = read_float8_slot(wslot, w_tr, tw_tr, &nt);
        if (nt) ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("null value in column \"trust\"")));
        RestoreWitness *entry = lp_idmap_get(wmap, &rw.id, NULL);
        *entry = rw;
        lp_push(&wlist, rw);
        ExecClearTuple(wslot);
    }
    ExecDropSingleTupleTableSlot(wslot);
    table_endscan(wscan);
    table_close(wrel, NoLock);

    PgContainedSeriesRows rows = {0};
    lp_pg_contained_series(file_ids, nfiles, &rows);
    pfree(file_ids);

    lp_vec(RestoreGroup) groups = {0};
    if (rows.n > 0 && wlist.n > 0) {
        if (has_single) {
            lp_strmap *gmap = lp_strmap_sized(sizeof(size_t));
            for (size_t i = 0; i < rows.n; i++) {
                const PgContainedSeriesRow *c = &rows.v[i];
                if (!isfinite(c->games) || c->games <= 0) continue;
                int64 cg = (int64)llround(c->games);
                int64 ct = (int64)llround(c->tokens);
                for (size_t wi = 0; wi < wlist.n; wi++) {
                    const RestoreWitness *w = &wlist.v[wi];
                    bool has_lin = c->has_dependence ? true : w->has_lin;
                    lp_id lin = c->has_dependence ? c->dependence_root : w->lineage;
                    uint8_t key[41];
                    memcpy(key, c->claim.b, 16);
                    key[16] = has_lin ? 1 : 0;
                    if (has_lin) memcpy(key + 17, lin.b, 16);
                    else memset(key + 17, 0, 16);
                    memcpy(key + 33, &w->trust, sizeof(double));

                    bool fresh = false;
                    size_t *idxp = lp_strmap_get(gmap, key, sizeof key, &fresh);
                    if (fresh) {
                        *idxp = groups.n;
                        RestoreGroup g = {
                            .claim = c->claim,
                            .witness = single_wit,
                            .lineage = lin,
                            .has_lin = has_lin,
                            .games = cg,
                            .tokens = ct,
                            .wins = c->score,
                            .wit_trust = w->trust,
                            .pos = c->position > 0 ? c->position : 0,
                            .has_pos = (c->position > 0),
                            .has_dep_trust = c->has_dependence,
                            .dep_trust = c->has_dependence ? c->dependence_trust : 0.0
                        };
                        lp_push(&groups, g);
                    } else {
                        RestoreGroup *g = &groups.v[*idxp];
                        g->games += cg;
                        g->tokens += ct;
                        g->wins += c->score;
                        if (c->position > 0 && (!g->has_pos || c->position < g->pos)) {
                            g->has_pos = true;
                            g->pos = c->position;
                        }
                        if (c->has_dependence) {
                            if (!g->has_dep_trust || c->dependence_trust > g->dep_trust) {
                                g->has_dep_trust = true;
                                g->dep_trust = c->dependence_trust;
                            }
                        }
                    }
                }
            }
            lp_strmap_free(gmap);
        } else {
            for (size_t i = 0; i < rows.n; i++) {
                const PgContainedSeriesRow *c = &rows.v[i];
                if (!isfinite(c->games) || c->games <= 0) continue;
                const RestoreWitness *w = lp_idmap_lookup(wmap, &c->trunk);
                if (!w) continue;
                int64 cg = (int64)llround(c->games);
                int64 ct = (int64)llround(c->tokens);
                RestoreGroup g = {
                    .claim = c->claim,
                    .witness = c->trunk,
                    .lineage = c->has_dependence ? c->dependence_root : w->lineage,
                    .has_lin = c->has_dependence ? true : w->has_lin,
                    .games = cg,
                    .tokens = Max(ct, cg),
                    .wins = c->score,
                    .wit_trust = w->trust,
                    .dep_trust = c->dependence_trust,
                    .has_dep_trust = c->has_dependence,
                    .pos = c->position > 0 ? c->position : 0,
                    .has_pos = (c->position > 0)
                };
                lp_push(&groups, g);
            }
        }
    }
    lp_vec_free(&rows);
    lp_vec_free(&wlist);
    lp_idmap_free(wmap);

    int64 inserted = 0;
    if (groups.n > 0) {
        PgRelWriter dw;
        pg_writer_open(&dw, delta_oid, true);
        AttrNumber ac = require_attnum(delta_oid, "claim", native_id, NULL);
        AttrNumber aw = require_attnum(delta_oid, "witness", native_id, NULL);
        AttrNumber al = require_attnum(delta_oid, "lineage", native_id, NULL);
        AttrNumber ag = require_attnum(delta_oid, "games", INT8OID, NULL);
        AttrNumber atok = require_attnum(delta_oid, "tokens", INT8OID, NULL);
        AttrNumber ascore = require_attnum(delta_oid, "score", FLOAT8OID, NULL);
        AttrNumber apos = require_attnum(delta_oid, "position", INT4OID, NULL);
        AttrNumber atrust = require_attnum(delta_oid, "trust", FLOAT8OID, NULL);
        AttrNumber awins = require_attnum(delta_oid, "wins", FLOAT8OID, NULL);

        bool *assigned = palloc0(sizeof(bool) * (size_t)dw.td->natts);
        assigned[ac - 1] = assigned[aw - 1] = assigned[al - 1] = assigned[ag - 1] = true;
        assigned[atok - 1] = assigned[ascore - 1] = assigned[apos - 1] = assigned[atrust - 1] = assigned[awins - 1] = true;

        for (size_t i = 0; i < groups.n; i++) {
            CHECK_FOR_INTERRUPTS();
            const RestoreGroup *g = &groups.v[i];
            if (g->games <= 0) continue;
            int64 tokens = Max(g->tokens, g->games);
            double score = fmin(1.0, fmax(0.0, g->wins / (double)g->games));
            double trust = g->has_dep_trust ? g->dep_trust : g->wit_trust;

            ExecClearTuple(dw.new_slot);
            pg_writer_fill_unset(&dw, assigned);
            dw.new_slot->tts_values[ac - 1] = PointerGetDatum(&g->claim); dw.new_slot->tts_isnull[ac - 1] = false;
            dw.new_slot->tts_values[aw - 1] = PointerGetDatum(&g->witness); dw.new_slot->tts_isnull[aw - 1] = false;
            dw.new_slot->tts_values[al - 1] = g->has_lin ? PointerGetDatum(&g->lineage) : (Datum)0;
            dw.new_slot->tts_isnull[al - 1] = !g->has_lin;
            dw.new_slot->tts_values[ag - 1] = Int64GetDatum(g->games); dw.new_slot->tts_isnull[ag - 1] = false;
            dw.new_slot->tts_values[atok - 1] = Int64GetDatum(tokens); dw.new_slot->tts_isnull[atok - 1] = false;
            dw.new_slot->tts_values[ascore - 1] = Float8GetDatum(score); dw.new_slot->tts_isnull[ascore - 1] = false;
            dw.new_slot->tts_values[apos - 1] = g->has_pos ? Int32GetDatum(g->pos) : (Datum)0;
            dw.new_slot->tts_isnull[apos - 1] = !g->has_pos;
            dw.new_slot->tts_values[atrust - 1] = Float8GetDatum(trust); dw.new_slot->tts_isnull[atrust - 1] = false;
            dw.new_slot->tts_values[awins - 1] = Float8GetDatum(g->wins); dw.new_slot->tts_isnull[awins - 1] = false;
            ExecStoreVirtualTuple(dw.new_slot);
            pg_writer_insert_new_slot(&dw);
            inserted++;
        }
        pfree(assigned);
        pg_writer_close(&dw);
    }
    lp_vec_free(&groups);
    if (inserted > 0) CommandCounterIncrement();
    PG_RETURN_INT64(inserted);
}

static int publish_attest_cmp(const void *a, const void *b)
{
    const MeasureAttestItem *x = a, *y = b;
    int c = (x->partition > y->partition) - (x->partition < y->partition);
    if (c) return c;
    c = lp_id_cmp(&x->claim, &y->claim);
    if (c) return c;
    c = lp_id_cmp(&x->witness, &y->witness);
    if (c) return c;
    if (x->has_lin != y->has_lin) return (int)x->has_lin - (int)y->has_lin;
    return x->has_lin ? lp_id_cmp(&x->lineage, &y->lineage) : 0;
}

PG_FUNCTION_INFO_V1(laplace_stage_publish_attestations);
Datum laplace_stage_publish_attestations(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_publish_attestations requires an UNLOGGED pending stage table")));
    Oid pending_oid = PG_GETARG_OID(0);
    require_stage_kind(pending_oid, false, "laplace_stage_publish_attestations requires an UNLOGGED pending stage table");
    require_rel_priv(pending_oid, ACL_SELECT);

    Oid affected_oid = (PG_NARGS() > 1 && !PG_ARGISNULL(1)) ? PG_GETARG_OID(1) : InvalidOid;
    if (OidIsValid(affected_oid)) {
        if (affected_oid == pending_oid)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("laplace_stage_publish_attestations requires distinct pending and affected tables")));
        require_stage_kind(affected_oid, true, "laplace_stage_publish_attestations requires an UNLOGGED affected stage table");
        require_rel_priv(affected_oid, ACL_SELECT);
    }

    Oid att_oid = lp_pg_relation("attestation");
    require_rel_priv(att_oid, ACL_SELECT | ACL_INSERT | ACL_UPDATE);
    Oid native_id = get_atttype(att_oid, get_attnum(att_oid, "claim"));

    Relation pending_rel = table_open(pending_oid, AccessShareLock);
    AttrNumber pc = require_attnum(pending_oid, "claim", native_id, NULL);
    AttrNumber pw = require_attnum(pending_oid, "witness", native_id, NULL);
    AttrNumber pl = require_attnum(pending_oid, "lineage", native_id, NULL);
    AttrNumber pg_col = require_attnum(pending_oid, "games", InvalidOid, NULL);
    AttrNumber ptok = require_attnum(pending_oid, "tokens", InvalidOid, NULL);
    AttrNumber pscore = require_attnum(pending_oid, "score", InvalidOid, NULL);
    AttrNumber ppos = require_attnum(pending_oid, "position", INT4OID, NULL);
    AttrNumber ptrust = require_attnum(pending_oid, "trust", InvalidOid, NULL);
    Oid tg = get_atttype(pending_oid, pg_col);
    Oid ttok = get_atttype(pending_oid, ptok);
    Oid tscore = get_atttype(pending_oid, pscore);
    Oid ttrust = get_atttype(pending_oid, ptrust);

    Relation att_parent = table_open(att_oid, AccessShareLock);
    PartitionKey pkey = RelationGetPartitionKey(att_parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(att_parent, true) : NULL;

    lp_vec(MeasureAttestItem) items = {0};
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc pscan = table_beginscan(pending_rel, snap, 0, NULL);
    TupleTableSlot *pslot = table_slot_create(pending_rel, NULL);
    while (table_scan_getnextslot(pscan, ForwardScanDirection, pslot)) {
        CHECK_FOR_INTERRUPTS();
        bool nc = false, nw = false, nl = false, ng = false, nt = false, ns = false, np = false, ntr = false;
        MeasureAttestItem m = {0};
        Datum v = slot_getattr(pslot, pc, &nc);
        if (nc) pg_series_invalid();
        memcpy(m.claim.b, DatumGetPointer(v), sizeof(lp_id));
        v = slot_getattr(pslot, pw, &nw);
        if (nw) pg_series_invalid();
        memcpy(m.witness.b, DatumGetPointer(v), sizeof(lp_id));
        v = slot_getattr(pslot, pl, &nl);
        m.has_lin = !nl;
        if (!nl) memcpy(m.lineage.b, DatumGetPointer(v), sizeof(lp_id));
        m.games = read_int64_slot(pslot, pg_col, tg, &ng);
        if (ng || m.games <= 0) pg_series_invalid();
        m.tokens = read_int64_slot(pslot, ptok, ttok, &nt);
        if (nt) pg_series_invalid();
        m.tokens = Max(m.tokens, m.games);
        m.score = read_float8_slot(pslot, pscore, tscore, &ns);
        if (ns) pg_series_invalid();
        v = slot_getattr(pslot, ppos, &np);
        m.has_pos = !np;
        if (!np) m.pos = DatumGetInt32(v);
        m.trust = read_float8_slot(pslot, ptrust, ttrust, &ntr);
        if (ntr) pg_series_invalid();
        m.partition = lp_pg_enrichment_partition(att_oid, pkey, pdesc, PointerGetDatum(&m.claim));
        lp_push(&items, m);
        ExecClearTuple(pslot);
    }
    ExecDropSingleTupleTableSlot(pslot);
    table_endscan(pscan);
    table_close(pending_rel, NoLock);
    table_close(att_parent, NoLock);

    const Oid *parts = NULL, *pks = NULL, *aux = NULL;
    int nparts = lp_pg_canonical_partitions("attestation", &parts, &pks, &aux, NULL);

    if (items.n > 0) {
        lp_sort(items.v, items.n, sizeof(*items.v), publish_attest_cmp);
        size_t keep = 0;
        for (size_t i = 0; i < items.n; i++) {
            if (keep > 0 && items.v[keep - 1].partition == items.v[i].partition
                && lp_id_eq(&items.v[keep - 1].claim, &items.v[i].claim)
                && lp_id_eq(&items.v[keep - 1].witness, &items.v[i].witness)
                && items.v[keep - 1].has_lin == items.v[i].has_lin
                && (!items.v[i].has_lin || lp_id_eq(&items.v[keep - 1].lineage, &items.v[i].lineage))) {
                items.v[keep - 1] = items.v[i];
            } else {
                items.v[keep++] = items.v[i];
            }
        }
        items.n = keep;

        for (size_t begin = 0; begin < items.n;) {
            CHECK_FOR_INTERRUPTS();
            size_t end = begin + 1;
            while (end < items.n && items.v[end].partition == items.v[begin].partition) end++;

            Oid partOid = items.v[begin].partition;
            PgRelWriter w;
            pg_writer_open(&w, partOid, false);
            Oid pkOid = InvalidOid;
            for (int p = 0; p < nparts; p++) if (parts[p] == partOid) { pkOid = pks[p]; break; }
            if (!OidIsValid(pkOid)) pkOid = RelationGetPrimaryKeyIndex(w.rel, false);
            if (!OidIsValid(pkOid)) ereport(ERROR, (errmsg("Native attestation: partition has no primary index")));
            Relation idx = index_open(pkOid, AccessShareLock);

            AttrNumber ac = get_attnum(partOid, "claim");
            AttrNumber aw = get_attnum(partOid, "witness");
            AttrNumber al = get_attnum(partOid, "lineage");
            AttrNumber ag = get_attnum(partOid, "games");
            AttrNumber atok = get_attnum(partOid, "tokens");
            AttrNumber ascore = get_attnum(partOid, "score");
            AttrNumber apos = get_attnum(partOid, "position");
            AttrNumber atrust = get_attnum(partOid, "trust");

            IndexScanDesc iscan = index_beginscan(w.rel, idx, w.snap, NULL, 3, 0);
            for (size_t i = begin; i < end; i++) {
                if (!((i - begin) & 255)) CHECK_FOR_INTERRUPTS();
                MeasureAttestItem *m = &items.v[i];
                ScanKeyData sk[3];
                init_cwl_scankeys(sk, idx, native_id, &m->claim, &m->witness, m->has_lin, &m->lineage);
                index_rescan(iscan, sk, 3, NULL, 0);
                if (index_getnext_slot(iscan, ForwardScanDirection, w.old_slot)) {
                    m->tid = w.old_slot->tts_tid;
                    m->found = true;
                    ExecClearTuple(w.old_slot);
                }
            }
            index_endscan(iscan);
            index_close(idx, NoLock);

            MeasureFillCtx fctx = { NULL, ac, aw, al, ag, atok, ascore, apos, atrust, false };
            for (size_t i = begin; i < end; i++) {
                CHECK_FOR_INTERRUPTS();
                fctx.item = &items.v[i];
                if (items.v[i].found) {
                    pg_writer_update_tid(&w, &items.v[i].tid, fill_attestation_slot, &fctx, false);
                } else {
                    fill_attestation_slot(NULL, w.new_slot, &fctx);
                    pg_writer_insert_new_slot(&w);
                }
            }
            pg_writer_close(&w);
            begin = end;
        }
        CommandCounterIncrement();
    }
    lp_vec_free(&items);

    int64 admitted = 0;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2)) {
        int nw = 0;
        lp_id *wids = ids_of(PG_GETARG_ARRAYTYPE_P(2), &nw);
        if (nw > 0) {
            lp_idmap *wmap = lp_idmap_new();
            for (int i = 0; i < nw; i++) lp_idmap_put(wmap, &wids[i], NULL);
            size_t nuw = lp_idmap_count(wmap);
            const lp_id *uw = lp_idmap_keys(wmap);
            lp_strmap *seen_wc = lp_strmap_sized(1);

            PushActiveSnapshot(GetTransactionSnapshot());
            Snapshot rsnap = GetActiveSnapshot();
            for (int p = 0; p < nparts; p++) {
                CHECK_FOR_INTERRUPTS();
                Relation part = table_open(parts[p], AccessShareLock);
                AttrNumber ac = get_attnum(parts[p], "claim");
                AttrNumber aw = get_attnum(parts[p], "witness");
                TupleTableSlot *slot = table_slot_create(part, NULL);
                if (OidIsValid(aux[p])) {
                    Relation idx = index_open(aux[p], AccessShareLock);
                    Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
                    RegProcedure eq_proc = get_opcode(eq_op);
                    IndexScanDesc iscan = index_beginscan(part, idx, rsnap, NULL, 1, 0);
                    for (size_t i = 0; i < nuw; i++) {
                        CHECK_FOR_INTERRUPTS();
                        ScanKeyData sk;
                        ScanKeyEntryInitialize(&sk, 0, 1, BTEqualStrategyNumber, native_id,
                                               idx->rd_indcollation[0], eq_proc, PointerGetDatum(&uw[i]));
                        index_rescan(iscan, &sk, 1, NULL, 0);
                        while (index_getnext_slot(iscan, ForwardScanDirection, slot)) {
                            bool nc = false;
                            Datum dc = slot_getattr(slot, ac, &nc);
                            if (!nc) {
                                uint8_t wc[32];
                                memcpy(wc, uw[i].b, 16);
                                memcpy(wc + 16, DatumGetPointer(dc), 16);
                                lp_strmap_put(seen_wc, wc, sizeof wc, NULL);
                            }
                            ExecClearTuple(slot);
                        }
                    }
                    index_endscan(iscan);
                    index_close(idx, NoLock);
                } else {
                    TableScanDesc s = table_beginscan(part, rsnap, 0, NULL);
                    while (table_scan_getnextslot(s, ForwardScanDirection, slot)) {
                        bool nc = false, nw_col = false;
                        Datum dw = slot_getattr(slot, aw, &nw_col);
                        if (!nw_col && lp_idmap_find(wmap, (const lp_id *)DatumGetPointer(dw)) >= 0) {
                            Datum dc = slot_getattr(slot, ac, &nc);
                            if (!nc) {
                                uint8_t wc[32];
                                memcpy(wc, DatumGetPointer(dw), 16);
                                memcpy(wc + 16, DatumGetPointer(dc), 16);
                                lp_strmap_put(seen_wc, wc, sizeof wc, NULL);
                            }
                        }
                        ExecClearTuple(slot);
                    }
                    table_endscan(s);
                }
                ExecDropSingleTupleTableSlot(slot);
                table_close(part, NoLock);
            }
            PopActiveSnapshot();
            admitted = (int64)lp_strmap_count(seen_wc);
            lp_strmap_free(seen_wc);
            lp_idmap_free(wmap);
        }
        pfree(wids);
    }

    int64 consensus_rows = 0;
    if (OidIsValid(affected_oid)) {
        lp_idmap *claims = lp_idmap_new();
        uint16_t buckets = 0;
        collect_stage_claims(affected_oid, native_id, claims, &buckets);
        if (lp_idmap_count(claims) > 0)
            consensus_rows = attest_stage_core(pending_oid, native_id, claims, buckets);
        lp_idmap_free(claims);
    }

    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_stage_publish_attestations requires composite result")));
    Datum vals[2] = { Int64GetDatum(admitted), Int64GetDatum(consensus_rows) };
    bool nulls[2] = { false, false };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), vals, nulls)));
}
