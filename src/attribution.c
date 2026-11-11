/* Fisher-information basis-point royalty attribution and rooted series solving
 * (laplace_attribution, laplace_source_attribution, laplace_source_standings,
 * laplace_series_snapshot). */
#include "pg_core.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "nodes/params.h"
#include "utils/builtins.h"
#include "utils/rel.h"
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

static char *pg_series_stage(Oid id, const char *column, Oid native_id, bool local_selection)
{
    Relation stage = table_open(id, AccessShareLock);
    bool stage_input = RelationGetNamespace(stage) == get_namespace_oid("stage", false)
        && stage->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED;
    bool local_input = local_selection && isTempNamespace(RelationGetNamespace(stage))
        && stage->rd_rel->relpersistence == RELPERSISTENCE_TEMP;
    if ((!stage_input && !local_input)
        || stage->rd_rel->relkind != RELKIND_RELATION
        || get_atttype(id, get_attnum(id, column)) != native_id)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("laplace_series_snapshot requires an UNLOGGED stage table with Native %s IDs", column)));
    List *inputs = find_all_inheritors(id, ShareLock, NULL); list_free(inputs);
    char *name = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(stage)), RelationGetRelationName(stage));
    table_close(stage, NoLock);
    return name;
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

typedef struct { size_t row;double remainder;lp_id id; } PgRemainder;
static int pg_remainder_order(const void *a,const void *b)
{
    const PgRemainder *x=a,*y=b;
    if(x->remainder!=y->remainder)return x->remainder>y->remainder?-1:1;
    return lp_id_cmp(&x->id,&y->id);
}
/* Allocate only after the selected claim/source set has been reduced. Stable
 * ID ties and one sort replace repeated winner scans and early rounding. */
static void pg_attribution_shares(PgAttrRow *rows,size_t n)
{
    double total=0;size_t positive=0;
    for(size_t i=0;i<n;i++){
        rows[i].share_bp=0;
        if(!isfinite(rows[i].fisher)||rows[i].fisher<0)pg_series_invalid();
        total+=rows[i].fisher;if(rows[i].fisher>0)positive++;
    }
    if(!positive)return;
    if(!isfinite(total)||total<=0)pg_series_invalid();
    PgRemainder *remainder=palloc(positive*sizeof *remainder);size_t at=0;int allocated=0;
    for(size_t i=0;i<n;i++)if(rows[i].fisher>0){
        double exact=10000*(rows[i].fisher/total);int points=(int)floor(exact);
        rows[i].share_bp=points;allocated+=points;
        remainder[at++]=(PgRemainder){i,exact-points,rows[i].trunk};
    }
    if(allocated>10000||(size_t)(10000-allocated)>positive)pg_series_invalid();
    lp_sort(remainder,positive,sizeof *remainder,pg_remainder_order);
    for(int i=0;i<10000-allocated;i++)rows[remainder[i].row].share_bp++;
    pfree(remainder);
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
        if(!valid||standing.matches>PG_INT64_MAX)
            ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("laplace attribution: invalid or non-convergent rooted series")));
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

static int pg_series_by_claim(const void *a, const void *b)
{
    return memcmp(((const PgSeriesObservation *)a)->claim.b, ((const PgSeriesObservation *)b)->claim.b, sizeof(lp_id));
}

static void pg_series_claim_row(TupleTableSlot *slot,void *context)
{
    PgSeriesBatch *batch=context;bool missing;Datum value=slot_getattr(slot,1,&missing);
    if(missing)pg_series_invalid();lp_idmap_put(batch->claims,(const lp_id *)DatumGetPointer(value),NULL);
}
static void pg_series_pending_row(TupleTableSlot *slot,void *context)
{
    PgSeriesBatch *batch=context;
    CHECK_FOR_INTERRUPTS(); bool missing; Datum value; PgSeriesObservation o = {0};
    value = slot_getattr(slot, 1, &missing);
    if (missing) pg_series_invalid(); memcpy(o.claim.b, DatumGetPointer(value), sizeof(lp_id));
    value = slot_getattr(slot, 2, &missing);
    if (missing) pg_series_invalid(); memcpy(o.witness.b, DatumGetPointer(value), sizeof(lp_id));
    value = slot_getattr(slot, 3, &missing);
    o.has_lineage = !missing; if (!missing) memcpy(o.lineage.b, DatumGetPointer(value), sizeof(lp_id));
    value = slot_getattr(slot, 4, &missing);
    if (missing) pg_series_invalid(); int64 games = DatumGetInt64(value);
    if (games<=0 || games>INT64CONST(9007199254740992)) pg_series_invalid(); o.games=(uint64_t)games;
    value = slot_getattr(slot, 5, &missing);
    if (missing) pg_series_invalid(); o.score=DatumGetFloat8(value);
    value = slot_getattr(slot, 6, &missing);
    if (missing) pg_series_invalid(); o.trust=DatumGetFloat8(value);
    if (!isfinite(o.score) || o.score<0 || o.score>1 || !isfinite(o.trust) || o.trust < -1 || o.trust>1) pg_series_invalid();
    uint8_t key[49]={0}; memcpy(key,o.claim.b,16); memcpy(key+16,o.witness.b,16); memcpy(key+32,o.lineage.b,16); key[48]=o.has_lineage;
    bool fresh; lp_strmap_put(batch->pending,key,sizeof key,&fresh); if (!fresh) pg_series_invalid();
    lp_strmap_put(batch->pairs,key,32,NULL);
    double *trust = lp_strmap_get(batch->trusts,key+16,33,&fresh);
    if (!fresh && *trust != o.trust) pg_series_invalid(); *trust=o.trust;
    lp_push(&batch->observations,o);
}

PG_FUNCTION_INFO_V1(laplace_series_snapshot);
Datum laplace_series_snapshot(PG_FUNCTION_ARGS)
{
    TupleDesc td; Tuplestorestate *result = set_begin(fcinfo, "laplace_series_snapshot", &td);
    if (PG_ARGISNULL(0)) pg_series_invalid();
    MemoryContext caller = CurrentMemoryContext;
    MemoryContext context = AllocSetContextCreate(caller, "Native series batch", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);
    PgSeriesBatch *batch = palloc0(sizeof *batch);
    MemoryContextCallback *cleanup = palloc(sizeof *cleanup);
    *cleanup = (MemoryContextCallback){.func=pg_series_batch_free, .arg=batch};
    MemoryContextRegisterResetCallback(context, cleanup);
    batch->claims = lp_idmap_new(); batch->witnesses = lp_idmap_sized(sizeof(PgSeriesWitness));
    batch->pending = lp_strmap_sized(1); batch->pairs = lp_strmap_sized(1); batch->trusts = lp_strmap_sized(sizeof(double));
    Oid physicality=lp_pg_relation("physicality");
    Oid native_id=get_atttype(physicality,get_attnum(physicality,"entity"));
    Oid idarray = get_array_type(native_id);
    char *pending = pg_series_stage(PG_GETARG_OID(0), "claim", native_id, false);
    char *changed = NULL;
    bool stage_selection = get_fn_expr_argtype(fcinfo->flinfo, 1) == REGCLASSOID;
    if (stage_selection && PG_ARGISNULL(1)) pg_series_invalid();
    if (stage_selection) changed = pg_series_stage(PG_GETARG_OID(1), "claim", native_id, true);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_series_snapshot: SPI_connect")));
    if (!stage_selection && !PG_ARGISNULL(1)) {
        int count; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(1), &count);
        for (int i = 0; i < count; i++) lp_idmap_put(batch->claims, ids+i, NULL);
        pfree(ids);
    } else {
        char *sql = psprintf("SELECT claim FROM %s", changed ? changed : pending);
        SPIPlanPtr plan = lp_pg_prepare_cursor(sql, 0, NULL, CURSOR_OPT_NO_SCROLL);
        if(!plan)ereport(ERROR,(errmsg("laplace_series_snapshot: claim plan failed")));
        lp_pg_stream(plan,NULL,pg_series_claim_row,batch);
        SPI_freeplan(plan);pfree(sql);
    }
    size_t count = lp_idmap_count(batch->claims);
    if (count) {
        /* Validate the requested carrier set once. The provenance kernel
         * reuses this proof and the witness registry instead of querying both again. */
        PgClaimFrontier verified={0};lp_pg_claim_bind(&verified.binding);
        lp_pg_lookup_paths(lp_idmap_keys(batch->claims),count,true,lp_pg_claim_frontier_row,&verified);
        MemoryContextDelete(verified.binding.context);
        if(verified.found!=count)pg_series_invalid();
        /* A stage-resident selection stays a set in PostgreSQL; no client-side
         * array export or fixed-count keyset loop is needed. */
        char *sql = changed ? psprintf("SELECT s.claim,s.witness,s.lineage,s.games::bigint,s.score::float8,s.trust::float8"
            " FROM %s s WHERE EXISTS(SELECT FROM %s c WHERE c.claim OPERATOR(@extschema@.=) s.claim)", pending, changed)
            : psprintf("SELECT s.claim,s.witness,s.lineage,s.games::bigint,s.score::float8,s.trust::float8"
                " FROM %s s WHERE s.claim OPERATOR(@extschema@.=) ANY($1)", pending);
        ArrayType *selected = changed ? NULL : id_array_of(native_id, lp_idmap_keys(batch->claims), (int)count);
        SPIPlanPtr plan=lp_pg_prepare_cursor(sql,changed?0:1,changed?NULL:&idarray,0);
        if(!plan)ereport(ERROR,(errmsg("laplace_series_snapshot: pending plan failed")));
        ParamListInfo parameters=changed?NULL:makeParamList(1);
        if(parameters){parameters->params[0]=(ParamExternData){.value=PointerGetDatum(selected),.ptype=idarray,.pflags=0};}
        lp_pg_stream(plan,parameters,pg_series_pending_row,batch);
        SPI_freeplan(plan);pfree(sql);if(selected)pfree(selected);if(parameters)pfree(parameters);
        for (size_t i=0; i<batch->observations.n; i++) {
            const PgSeriesObservation *o=batch->observations.v+i;
            bool fresh;
            PgSeriesWitness *w=lp_idmap_get(batch->witnesses,&o->witness,&fresh);
            if(fresh || !o->has_lineage){
                w->trust=o->trust; w->has_lineage=o->has_lineage;
                if(o->has_lineage)w->lineage=o->lineage;
            }
        }
        SPI_finish();
        if (changed) lp_pg_read_attestation_stage(changed,&batch->historical,batch->witnesses);
        else lp_pg_read_attestation_rows(lp_idmap_keys(batch->claims),(int)count,NULL,&batch->historical,batch->witnesses);
        pg_attribution_ancestors(batch);
        for (size_t i=0; i<batch->historical.n; i++) {
            const PgAttrRow *r=batch->historical.v+i; uint8_t key[32]; memcpy(key,r->claim.b,16); memcpy(key+16,r->trunk.b,16);
            if (lp_strmap_find(batch->pairs,key,sizeof key)>=0 || r->games<=0) continue;
            PgSeriesWitness *w=lp_idmap_lookup(batch->witnesses,&r->trunk);
            if (!w || r->games>9007199254740992.0 || !isfinite(r->games)) pg_series_invalid();
            PgSeriesObservation o={.claim=r->claim,.witness=r->trunk,.games=(uint64_t)r->games,
                .score=r->score/r->games,.trust=r->has_dependence?r->trust:w->trust,
                .has_lineage=r->has_dependence || w->has_lineage,
                .lineage=r->has_dependence?r->dependence:w->lineage};
            lp_push(&batch->observations,o);
        }
        lp_sort(batch->observations.v,batch->observations.n,sizeof(PgSeriesObservation),pg_series_by_claim);
        lp_id *ordered=palloc(count*sizeof(lp_id)); memcpy(ordered,lp_idmap_keys(batch->claims),count*sizeof(lp_id));
        lp_sort(ordered,count,sizeof(lp_id),lp_id_cmp);
        size_t at=0;
        for (size_t i=0; i<count; i++) {
            CHECK_FOR_INTERRUPTS(); batch->entries.n=0;
            while (at<batch->observations.n && lp_id_eq(&batch->observations.v[at].claim,ordered+i)) {
                const PgSeriesObservation *o=batch->observations.v+at++;
                lp_id root=pg_series_root(batch,o->has_lineage?o->lineage:o->witness);
                lp_series_entry entry;                        /* the one law for an attestation row (lp_series_attested) */
                if(!lp_series_attested(root,(double)o->games,(double)o->games*o->score,o->trust,&entry))pg_series_invalid();
                lp_push(&batch->entries,entry);
            }
            Datum values[5]={id_datum(ordered+i),0,0,0,0}; bool missing[5]={false,true,true,true,true};
            if (batch->entries.n) {
                lp_standing standing;
                if (!lp_series(batch->entries.v,batch->entries.n,&standing) || standing.matches>PG_INT64_MAX)
                    ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("laplace_series: invalid or non-convergent rooted series")));
                values[1]=Float8GetDatum(standing.rating); values[2]=Float8GetDatum(standing.deviation);
                values[3]=Float8GetDatum(standing.volatility); values[4]=Int64GetDatum((int64)standing.matches);
                memset(missing,0,sizeof missing);
            }
            set_put(fcinfo,result,td,values,missing); pfree(DatumGetPointer(values[0]));
        }
    } else SPI_finish();
    MemoryContextSwitchTo(caller); MemoryContextDelete(context);
    PG_RETURN_NULL();
}
