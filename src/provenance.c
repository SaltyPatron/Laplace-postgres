/* Native containment DAG: upward testimony and downward stored series share
 * edge decoding, run counts, dependence classes and tally reduction. */
#include "pg_core.h"
#include "geo.h"
#include "executor/spi.h"
#include "access/htup_details.h"
#include "nodes/plannodes.h"
#include "nodes/params.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "laplace/highway.h"
#include "laplace/geometry.h"
#include "laplace/consensus.h"
#include <math.h>
#include <limits.h>

/* ---- provenance by containment and basis-point royalty attribution (laplace_attested, laplace_attribution)
 * Walks up the container index from claims to their trunks and witnesses in one native SPI operator. */
#define PG_NONE ((size_t)-1)
enum { PG_ALL_CONTAINERS, PG_POSITIVE_WITNESSES, PG_REGISTERED_WITNESSES };
typedef struct { size_t p, c; uint64_t times; uint32_t outcome, spare, position; uint8_t claim, rec; size_t in_next;
                 lp_id dependence; double dependence_trust; uint8_t has_dependence; } PgEdge;
typedef struct { lp_id id; int nparents; size_t in_head; uint8_t file, claim; } PgWNode;
typedef lp_vec(PgWNode) PgWNodes;
typedef lp_vec(PgEdge)  PgEdges;
typedef struct { lp_idmap *m; PgWNodes n; PgEdges e; } PgDag;
typedef struct { double games, tokens, score; uint32_t position; lp_id claim, dependence; double dependence_trust; uint8_t has_dependence; } PgTally;
typedef struct { lp_id claim, trunk; double games, tokens, score; uint32_t position; double trust; int witness, share_bp;
                 lp_id dependence; double source_trust; uint8_t has_dependence; } PgAttrRow;
typedef lp_vec(PgAttrRow) PgAttrRows;

static size_t pg_dag_node(PgDag *d, const lp_id *id, int *fresh_out)
{
    bool fresh = false;
    size_t i = lp_idmap_put(d->m, id, &fresh);
    if (fresh) {
        PgWNode w = { .id=*id, .in_head=PG_NONE };
        lp_push(&d->n, w);
    }
    if (fresh_out) *fresh_out = fresh ? 1 : 0;
    return i;
}

static PgEdge *pg_dag_edge(PgDag *d, size_t p, size_t c, const lp_vertex *v)
{
    size_t k = d->e.n;
    PgEdge e = { .p=p,.c=c,.times=v->run,.outcome=v->outcome,.spare=v->spare,.position=v->position,.in_next=d->n.v[c].in_head };
    lp_push(&d->e, e);
    d->n.v[c].in_head = k;
    d->n.v[c].nparents++;
    return &d->e.v[k];
}

static double *pg_down_from(const PgDag *d, size_t top)
{
    size_t n = d->n.n;
    double *cnt = palloc0((n ? n : 1) * sizeof(double));
    int *indeg = palloc0((n ? n : 1) * sizeof(int));
    size_t *head = palloc((n ? n : 1) * sizeof(size_t));
    size_t *link = palloc((d->e.n ? d->e.n : 1) * sizeof(size_t));
    for (size_t i = 0; i < n; i++) head[i] = PG_NONE;
    for (size_t k = 0; k < d->e.n; k++) {
        link[k] = head[d->e.v[k].p];
        head[d->e.v[k].p] = k;
        indeg[d->e.v[k].c]++;
    }
    size_t *q = palloc((n ? n : 1) * sizeof(size_t)), qh = 0, qt = 0;
    for (size_t i = 0; i < n; i++) if (!indeg[i]) q[qt++] = i;
    cnt[top] = 1.0;
    while (qh < qt) {
        size_t p = q[qh++];
        for (size_t k = head[p]; k != PG_NONE; k = link[k]) {
            size_t c = d->e.v[k].c;
            cnt[c] += cnt[p] * (double)d->e.v[k].times;
            if (!--indeg[c]) q[qt++] = c;
        }
    }
    pfree(indeg); pfree(head); pfree(link); pfree(q);
    return cnt;
}

static void pg_tally_under(const PgDag *d, const double *cnt, lp_strmap *per_claim)
{
    for (size_t k = 0; k < d->e.n; k++) {
        const PgEdge *e = &d->e.v[k];
        if (!e->claim) continue;
        double c = cnt[e->p]; if (c <= 0) continue;
        double g = e->rec ? c * (double)e->times : c;
        double tk = c * (double)e->times;
        double s = g * lp_outcome_score(e->outcome, e->spare);
        bool fresh;
        uint8_t key[32]={0}; memcpy(key,d->n.v[e->c].id.b,16);
        if (e->has_dependence) memcpy(key+16,e->dependence.b,16);
        PgTally *t = lp_strmap_get(per_claim,key,sizeof key,&fresh);
        if (fresh) *t = (PgTally){ .claim=d->n.v[e->c].id,.dependence=e->dependence,
            .dependence_trust=e->dependence_trust,.has_dependence=e->has_dependence };
        t->games += g; t->tokens += tk; t->score += s;
        if (e->position && (!t->position || e->position < t->position)) t->position = e->position;
    }
}

static const lp_trust_class *pg_dependence_class(lp_idmap *classes, const lp_vertex *vertices, int n, int at){
    const lp_vertex *v=vertices+at; bool added;
    const lp_trust_class **c=lp_idmap_get(classes,&v->id,&added);
    if(added)*c=lp_trust_class_for_id(lp_pg_text_context(),&v->id);
    if(!*c || v->run!=1 || at+1==n
        || (vertices[at+1].said!=LP_SAID_CLAIM && vertices[at+1].said!=LP_SAID_RECORD))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace containment: invalid dependence-class qualifier")));
    return *c;
}

/* Restore testimony from persisted trunks without opening their source bytes.
 * Every SPI request is a bounded frontier set, not a command per vertex. */
PG_FUNCTION_INFO_V1(laplace_contained_series);
Datum laplace_contained_series(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts=set_begin(fcinfo,"laplace_contained_series",&td);
    int n; lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n); if(!n)PG_RETURN_NULL();
    Oid idarr=get_fn_expr_argtype(fcinfo->flinfo,0),idelm=get_element_type(idarr);
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("laplace_contained_series: SPI_connect")));
    PgDag d={lp_idmap_new(),{0},{0}};
    lp_idmap *roots=lp_idmap_new(),*front=lp_idmap_new(),*seen=lp_idmap_new();
    lp_idmap *classes=lp_idmap_sized(sizeof(const lp_trust_class *));
    lp_vec(lp_vertex) vertices={0};
    for(int i=0;i<n;i++){lp_idmap_put(roots,ids+i,NULL);lp_idmap_put(front,ids+i,NULL);pg_dag_node(&d,ids+i,NULL);}
    SPIPlanPtr downward=SPI_prepare_cursor(
        "SELECT p.entity,p.path,laplace_mask_has_value(p.mask,'kind','claim') AND laplace_path_length(p.path)>=2"
        " FROM public.physicality p WHERE p.entity=ANY($1) ORDER BY p.entity",1,&idarr,
        CURSOR_OPT_CUSTOM_PLAN|CURSOR_OPT_PARALLEL_OK);
    if(!downward)ereport(ERROR,(errmsg("laplace_contained_series: downward plan failed")));
    ParamListInfo params=makeParamList(1);
    params->params[0].ptype=idarr;params->params[0].isnull=false;params->params[0].pflags=0;
    const lp_tier0_record *floor=lp_pg_tier0();
    while(lp_idmap_count(front)){
        lp_idmap *next=lp_idmap_new();size_t count=lp_idmap_count(front);
        for(size_t begin=0;begin<count;begin+=65536){
            size_t amount=count-begin;if(amount>65536)amount=65536;
            ArrayType *window=id_array_of(idelm,lp_idmap_keys(front)+begin,(int)amount);
            params->params[0].value=PointerGetDatum(window);
            if(SPI_execute_plan_with_paramlist(downward,params,true,0)!=SPI_OK_SELECT)
                ereport(ERROR,(errmsg("laplace_contained_series: downward walk failed")));
            for(uint64 row=0;row<SPI_processed;row++){
                bool nl;lp_id id;
                memcpy(id.b,DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,1,&nl)),16);
                size_t pi=pg_dag_node(&d,&id,NULL);lp_idmap_put(seen,&id,NULL);
                d.n.v[pi].claim=DatumGetBool(SPI_getbinval(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,3,&nl));
                Geo geometry=geo_of(SPI_getbinval(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,2,&nl));
                int nv=(int)geometry.p.n;if(nv<=0)continue;
                lp_vec_reserve(&vertices,(size_t)nv);
                if(lp_path_decode(geometry.p,vertices.v,(size_t)nv)!=(size_t)nv)
                    ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_contained_series: invalid stored path")));
                d.n.v[pi].file=vertices.v[0].said==LP_SAID_METADATA;
                const lp_trust_class *pending=NULL;lp_id pending_id={{0}};
                for(int k=0;k<nv;k++){
                    const lp_vertex *v=vertices.v+k;
                    if(v->said==LP_SAID_DEPENDENCE){pending=pg_dependence_class(classes,vertices.v,nv,k);pending_id=v->id;continue;}
                    const lp_trust_class *scope=pending;pending=NULL;
                    if(v->said==LP_SAID_METADATA)continue;
                    size_t ci=pg_dag_node(&d,&v->id,NULL);
                    PgEdge *edge=pg_dag_edge(&d,pi,ci,v);
                    if(v->said==LP_SAID_CLAIM || v->said==LP_SAID_RECORD){
                        edge->claim=1;edge->rec=v->said==LP_SAID_RECORD;
                        if(scope){edge->has_dependence=1;edge->dependence=pending_id;edge->dependence_trust=scope->prior;}
                    }
                    if(!lp_tier0_contains(floor,&v->id) && lp_idmap_find(seen,&v->id)<0 && lp_idmap_find(front,&v->id)<0)
                        lp_idmap_put(next,&v->id,NULL);
                }
            }
            SPI_freetuptable(SPI_tuptable);pfree(window);
        }
        lp_idmap_free(front);front=next;
    }
    SPI_freeplan(downward);pfree(params);lp_idmap_free(front);lp_vec_free(&vertices);lp_idmap_free(classes);
    /* Assertion metadata names a claim only when the child's carrier does too.
     * This is the same admission boundary used by laplace_series_snapshot. */
    for(size_t i=0;i<d.e.n;i++)if(!d.n.v[d.e.v[i].c].claim)d.e.v[i].claim=0;
    bool absent=false;
    for(size_t i=0;i<lp_idmap_count(roots);i++){
        const lp_id *root=lp_idmap_keys(roots)+i;
        if(lp_idmap_find(seen,root)<0){absent=true;break;}
        size_t top=(size_t)lp_idmap_find(d.m,root);
        double *counts=pg_down_from(&d,top);lp_strmap *per_claim=lp_strmap_sized(sizeof(PgTally));
        pg_tally_under(&d,counts,per_claim);
        for(size_t j=0;j<lp_strmap_count(per_claim);j++){
            const PgTally *t=lp_strmap_at(per_claim,j);if(t->games<=0)continue;
            Datum values[8]={id_datum(root),id_datum(&t->claim),Float8GetDatum(t->games),Float8GetDatum(t->tokens),
                Float8GetDatum(t->score),Int32GetDatum((int32)t->position),
                t->has_dependence?id_datum(&t->dependence):(Datum)0,
                t->has_dependence?Float8GetDatum(t->dependence_trust):(Datum)0};
            bool nulls[8]={false,false,false,false,false,false,!t->has_dependence,!t->has_dependence};
            set_put(fcinfo,ts,td,values,nulls);
        }
        lp_strmap_free(per_claim);pfree(counts);
    }
    lp_idmap_free(roots);lp_idmap_free(seen);lp_idmap_free(d.m);lp_vec_free(&d.n);lp_vec_free(&d.e);SPI_finish();
    if(absent)ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_contained_series: root has no stored path")));
    PG_RETURN_NULL();
}

static int by_attr_row(const void *a, const void *b)
{
    const PgAttrRow *x = a, *y = b;
    int c = memcmp(x->claim.b, y->claim.b, 16); if (c) return c;
    if (x->witness != y->witness) return y->witness - x->witness;
    if (x->games != y->games) return x->games > y->games ? -1 : 1;
    c=memcmp(x->trunk.b, y->trunk.b, 16); if (c) return c;
    if (x->has_dependence!=y->has_dependence) return (int)x->has_dependence-(int)y->has_dependence;
    return memcmp(x->dependence.b,y->dependence.b,16);
}

static void compute_attested_rows(const lp_id *claims, int n, Oid idarr, int witness_filter, int include_files, PgAttrRows *out)
{
    Oid idelm = get_element_type(idarr);
    PgDag d = { lp_idmap_new(), {0}, {0} };
    lp_idmap *front = lp_idmap_new();
    for (int i = 0; i < n; i++) {
        lp_idmap_put(front, &claims[i], NULL);
        pg_dag_node(&d, &claims[i], NULL);
    }
    lp_vec(lp_vertex) vx = {0};
    lp_idmap *classes=lp_idmap_sized(sizeof(const lp_trust_class *));
    /* A custom plan copies the complete frontier into every partition's
     * scan. Serializing that plan for parallel workers can exceed 1 GiB.
     * Keep one parameter without PARAM_FLAG_CONST: the planner can estimate
     * its actual search keys without embedding its value in every scan. */
    SPIPlanPtr upward = SPI_prepare_cursor(
        "SELECT p.entity, p.path FROM physicality p "
        "WHERE p.path && $1 ORDER BY p.entity", 1, &idarr,
        CURSOR_OPT_CUSTOM_PLAN | CURSOR_OPT_PARALLEL_OK);
    if (!upward) ereport(ERROR, (errmsg("laplace_attested: upward plan failed")));
    ParamListInfo frontier_params = makeParamList(1);
    frontier_params->params[0].ptype = idarr;
    frontier_params->params[0].isnull = false;
    frontier_params->params[0].pflags = 0;
    /* The prepared search retains two ID arrays, shared across partition
     * expressions. Size its working set from PostgreSQL's configured memory
     * budget; a fixed 4096 IDs repeatedly rescans a large physicality store.
     * Cover the complete frontier even when it needs several such windows. */
    uint64 memory_bytes = (uint64)work_mem * 1024;
    size_t array_ids = (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id);
    size_t window_ids = (size_t)Min(memory_bytes / (2 * sizeof(lp_id)), (uint64)array_ids);
    if (!window_ids) window_ids = 1;
    for (int lvl = 0; lp_idmap_count(front); lvl++) {
        size_t count = lp_idmap_count(front);
        lp_idmap *next = lp_idmap_new();
        lp_idmap *seen = lp_idmap_new();
        /* Bound each GIN search and its parameter/cache memory. A parent can
         * match several windows: decode it once against the WHOLE frontier,
         * retaining all of its edges without counting its testimony twice. */
        for (size_t begin = 0; begin < count; begin += window_ids) {
            size_t amount = count - begin; if (amount > window_ids) amount = window_ids;
            ArrayType *window = id_array_of(idelm, lp_idmap_keys(front) + begin, (int)amount);
            frontier_params->params[0].value = PointerGetDatum(window);
            Portal scan = SPI_cursor_open_with_paramlist(NULL, upward, frontier_params, true);
            if (!scan)
                ereport(ERROR, (errmsg("laplace_attested: upward walk failed")));
            PlannedStmt *statement = PortalGetPrimaryStmt(scan);
            size_t tuple_bytes = MAXALIGN(sizeof(HeapTupleData)) + MAXALIGN(SizeofHeapTupleHeader)
                + sizeof(HeapTuple) + MAXALIGN((size_t)Max(statement->planTree->plan_width, 1));
            long fetch_rows = (long)Min(memory_bytes / tuple_bytes, (uint64)LONG_MAX);
            if (!fetch_rows) fetch_rows = 1;
            for (;;) {
                SPI_cursor_fetch(scan, true, fetch_rows);
                if (!SPI_processed) { SPI_freetuptable(SPI_tuptable); break; }
                for (uint64 r = 0; r < SPI_processed; r++) {
                    bool nl; lp_id pid;
                    memcpy(pid.b, DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)), 16);
                    bool first; lp_idmap_put(seen, &pid, &first); if (!first) continue;
                    Geo g = geo_of(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl));
                    int nv = (int)g.p.n; if (nv <= 0) continue;
                    lp_vec_reserve(&vx, (size_t)nv);
                    if (lp_path_decode(g.p, vx.v, (size_t)nv) != (size_t)nv) continue;
                    size_t pi = PG_NONE; int fresh = 0;
                    const lp_trust_class *pending_class=NULL; lp_id pending_id={{0}};
                    for (int k = 0; k < nv; k++) {
                        if (vx.v[k].said==LP_SAID_DEPENDENCE) {
                            pending_class=pg_dependence_class(classes,vx.v,nv,k); pending_id=vx.v[k].id; continue;
                        }
                        const lp_trust_class *scope=pending_class; pending_class=NULL;
                        if (lp_idmap_find(front, &vx.v[k].id) < 0) continue;
                        uint32_t sd = vx.v[k].said;
                        if (lvl == 0 && sd != LP_SAID_CLAIM && sd != LP_SAID_RECORD) continue;
                        if (lvl > 0 && sd == LP_SAID_METADATA) continue;
                        if (pi == PG_NONE) {
                            pi = pg_dag_node(&d, &pid, &fresh);
                            d.n.v[pi].file = vx.v[0].said == LP_SAID_METADATA;
                        }
                        size_t ci = (size_t)lp_idmap_find(d.m, &vx.v[k].id);
                        PgEdge *e = pg_dag_edge(&d, pi, ci, &vx.v[k]);
                        if (lvl == 0) { e->claim = 1; e->rec = (sd == LP_SAID_RECORD);
                            if (scope) { e->has_dependence=1; e->dependence=pending_id; e->dependence_trust=scope->prior; }
                        }
                    }
                    if (pi != PG_NONE && fresh) lp_idmap_put(next, &pid, NULL);
                }
                SPI_freetuptable(SPI_tuptable);
            }
            SPI_cursor_close(scan);
            pfree(window);
        }
        lp_idmap_free(seen);
        lp_idmap_free(front);
        front = next;
    }
    SPI_freeplan(upward);
    pfree(frontier_params);
    lp_idmap_free(front);
    lp_vec_free(&vx);
    lp_idmap_free(classes);

    lp_vec(size_t) rix = {0};
    lp_vec(lp_id)  rids = {0};
    /* A registered source can itself be contained by another trunk. Its own
     * assertion must not disappear merely because it is not a graph root.
     * The input can also repeat IDs, so input length is not a DAG index. */
    for (size_t i = 0; i < d.n.n; i++) {
        lp_push(&rids, d.n.v[i].id);
    }
    lp_idmap *wmap = lp_idmap_sized(sizeof(double));
    if (rids.n > 0) {
        Datum a[1] = { PointerGetDatum(id_array_of(idelm, rids.v, (int)rids.n)) }; Oid t[1] = { idarr };
        if (SPI_execute_with_args("SELECT id, trust FROM witness WHERE id = ANY($1)", 1, t, a, NULL, true, 0) != SPI_OK_SELECT)
            ereport(ERROR, (errmsg("laplace_attested: witness lookup failed")));
        for (uint64 r = 0; r < SPI_processed; r++) {
            bool nl; lp_id wid;
            memcpy(wid.b, DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)), 16);
            double tr = DatumGetFloat8(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl));
            bool wf; *(double *)lp_idmap_get(wmap, &wid, &wf) = tr;
        }
    }

    for (size_t i = 0; i < d.n.n; i++)
        if (d.n.v[i].nparents == 0 || (include_files && d.n.v[i].file) || lp_idmap_lookup(wmap, &d.n.v[i].id)) lp_push(&rix, i);

    for (size_t r = 0; r < rix.n; r++) {
        size_t top = rix.v[r];
        lp_id tid = d.n.v[top].id;
        const double *wtr = lp_idmap_lookup(wmap, &tid);
        int is_w = (wtr != NULL);
        double tr = is_w ? *wtr : 0.0;
        if (witness_filter != PG_ALL_CONTAINERS && !is_w) continue;
        if (witness_filter == PG_POSITIVE_WITNESSES && tr <= 0.0) continue;
        double *cnt = pg_down_from(&d, top);
        lp_strmap *per_claim = lp_strmap_sized(sizeof(PgTally));
        pg_tally_under(&d, cnt, per_claim);
        for (size_t i = 0; i < lp_strmap_count(per_claim); i++) {
            const PgTally *tl = lp_strmap_at(per_claim, i);
            if (tl->games <= 0) continue;
            PgAttrRow row = { .claim=tl->claim,.trunk=tid,.games=tl->games,.tokens=tl->tokens,.score=tl->score,
                .position=tl->position,.trust=tl->has_dependence ? tl->dependence_trust : tr,.witness=is_w,
                .dependence=tl->dependence,.source_trust=tr,.has_dependence=tl->has_dependence };
            lp_push(out, row);
        }
        lp_strmap_free(per_claim);
        pfree(cnt);
    }
    lp_sort(out->v, out->n, sizeof(PgAttrRow), by_attr_row);

    /* Largest-remainder basis-point attribution per claim across witnessed trunks with positive weight */
    for (size_t i = 0; i < out->n;) {
        size_t j = i + 1;
        while (j < out->n && memcmp(out->v[j].claim.b, out->v[i].claim.b, 16) == 0) j++;
        double wsum = 0.0;
        for (size_t k = i; k < j; k++) if (out->v[k].witness && out->v[k].trust > 0.0)
            wsum += out->v[k].games * out->v[k].trust;
        if (wsum > 0.0) {
            int bp_sum = 0;
            double *rem = palloc((j - i) * sizeof(double));
            for (size_t k = i; k < j; k++) {
                if (out->v[k].witness && out->v[k].trust > 0.0) {
                    double exact = (out->v[k].games * out->v[k].trust * 10000.0) / wsum;
                    int fl = (int)floor(exact);
                    out->v[k].share_bp = fl;
                    rem[k - i] = exact - (double)fl;
                    bp_sum += fl;
                } else {
                    out->v[k].share_bp = 0;
                    rem[k - i] = -1.0;
                }
            }
            while (bp_sum < 10000) {
                size_t best = i; double best_r = -1.0;
                for (size_t k = i; k < j; k++) if (rem[k - i] > best_r) { best_r = rem[k - i]; best = k; }
                if (best_r < 0.0) break;
                out->v[best].share_bp++;
                rem[best - i] = -1.0;
                bp_sum++;
            }
            pfree(rem);
        }
        i = j;
    }

    lp_idmap_free(wmap);
    lp_vec_free(&rix); lp_vec_free(&rids);
    lp_idmap_free(d.m); lp_vec_free(&d.n); lp_vec_free(&d.e);
}

/* The source-facing provenance API remains one row per corpus/container. The
 * series API below retains each explicit dependence scope for Native solving. */
static void pg_rollup_sources(PgAttrRows *rows){
    lp_strmap *map=lp_strmap_sized(sizeof(PgAttrRow));
    for (size_t i=0; i<rows->n; i++) {
        const PgAttrRow *r=rows->v+i; uint8_t key[32]; memcpy(key,r->claim.b,16); memcpy(key+16,r->trunk.b,16);
        bool fresh; PgAttrRow *v=lp_strmap_get(map,key,sizeof key,&fresh);
        if (fresh) { *v=*r; v->trust=r->source_trust; v->has_dependence=0; memset(&v->dependence,0,sizeof v->dependence); }
        else { v->games+=r->games; v->tokens+=r->tokens; v->score+=r->score; v->share_bp+=r->share_bp;
            if (r->position && (!v->position || r->position<v->position)) v->position=r->position; }
    }
    rows->n=0;
    for (size_t i=0; i<lp_strmap_count(map); i++) lp_push(rows,*(PgAttrRow *)lp_strmap_at(map,i));
    lp_strmap_free(map); lp_sort(rows->v,rows->n,sizeof(PgAttrRow),by_attr_row);
}

PG_FUNCTION_INFO_V1(laplace_attested_series);
Datum laplace_attested_series(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts=set_begin(fcinfo,"laplace_attested_series",&td);
    int n; lp_id *claims=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n); if (!n) PG_RETURN_NULL();
    Oid idarr=get_fn_expr_argtype(fcinfo->flinfo,0);
    if (SPI_connect()!=SPI_OK_CONNECT) ereport(ERROR,(errmsg("laplace_attested_series: SPI_connect")));
    PgAttrRows rows={0};
    int filter = PG_NARGS() > 1 && PG_GETARG_BOOL(1) ? PG_REGISTERED_WITNESSES : PG_ALL_CONTAINERS;
    compute_attested_rows(claims,n,idarr,filter,0,&rows);
    for (size_t i=0; i<rows.n; i++) {
        const PgAttrRow *r=rows.v+i;
        Datum v[9]={id_datum(&r->claim),id_datum(&r->trunk),Float8GetDatum(r->games),Float8GetDatum(r->tokens),
            Float8GetDatum(r->score),Int32GetDatum((int32)r->position),Float8GetDatum(r->trust),BoolGetDatum(r->witness!=0),
            r->has_dependence ? id_datum(&r->dependence) : (Datum)0};
        bool nl[9]={false,false,false,false,false,false,false,false,!r->has_dependence};
        set_put(fcinfo,ts,td,v,nl);
    }
    lp_vec_free(&rows); SPI_finish(); PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_attested);
Datum laplace_attested(PG_FUNCTION_ARGS)
{
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_attested", &td);
    int n; lp_id *claims = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (!n) PG_RETURN_NULL();
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_attested: SPI_connect")));
    PgAttrRows rows = {0};
    int include_files = PG_NARGS() > 1 && !PG_ARGISNULL(1) && PG_GETARG_BOOL(1);
    compute_attested_rows(claims, n, idarr, PG_ALL_CONTAINERS, include_files, &rows);
    pg_rollup_sources(&rows);
    for (size_t i = 0; i < rows.n; i++) {
        const PgAttrRow *r = &rows.v[i];
        Datum v[8] = {
            id_datum(&r->claim), id_datum(&r->trunk),
            Float8GetDatum(r->games), Float8GetDatum(r->tokens), Float8GetDatum(r->score),
            Int32GetDatum((int32)r->position), Float8GetDatum(r->trust), BoolGetDatum(r->witness != 0)
        };
        bool nl[8] = {0};
        set_put(fcinfo, ts, td, v, nl);
    }
    lp_vec_free(&rows);
    SPI_finish();
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_attribution);
Datum laplace_attribution(PG_FUNCTION_ARGS)
{
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_attribution", &td);
    int n; lp_id *claims = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (!n) PG_RETURN_NULL();
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_attribution: SPI_connect")));
    PgAttrRows rows = {0};
    compute_attested_rows(claims, n, idarr, PG_POSITIVE_WITNESSES, 0, &rows);
    pg_rollup_sources(&rows);
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
    SPI_finish();
    PG_RETURN_NULL();
}
