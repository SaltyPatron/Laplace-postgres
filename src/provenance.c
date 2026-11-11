/* Native containment DAG: upward testimony and downward stored series share
 * edge decoding, run counts, dependence classes and tally reduction. */
#include "pg_core.h"
#include "geo.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "laplace/highway.h"
#include "laplace/geometry.h"
#include "laplace/consensus.h"
#include <math.h>
#include <limits.h>

#define PG_NONE ((size_t)-1)
typedef struct { size_t p, c; uint64_t times; uint32_t outcome, spare, position; uint8_t claim, rec; size_t next;
                 lp_id dependence; double dependence_trust; uint8_t has_dependence; } PgEdge;
typedef struct { lp_id id; int nparents; uint8_t file, claim; } PgWNode;
typedef lp_vec(PgWNode) PgWNodes;
typedef lp_vec(PgEdge)  PgEdges;
typedef lp_vec(uint64_t) PgOrdinals;
typedef struct { lp_idmap *m; PgWNodes n; PgEdges e; } PgDag;
typedef struct { lp_tally t; lp_id claim, dependence; double dependence_trust; uint8_t has_dependence; } PgTally;

static size_t pg_dag_node(PgDag *d, const lp_id *id, int *fresh_out)
{
    bool fresh = false;
    size_t i = lp_idmap_put(d->m, id, &fresh);
    if (fresh) {
        PgWNode w = { .id=*id };
        lp_push(&d->n, w);
    }
    if (fresh_out) *fresh_out = fresh ? 1 : 0;
    return i;
}

static PgEdge *pg_dag_edge(PgDag *d, size_t p, size_t c, const lp_vertex *v)
{
    PgEdge e = { .p=p,.c=c,.times=v->run,.outcome=v->outcome,.spare=v->spare,.position=v->position,.next=PG_NONE };
    lp_push(&d->e, e);
    d->n.v[c].nparents++;
    return &d->e.v[d->e.n-1];
}

typedef struct { size_t *head, *order, n; } PgDown;
typedef void (*PgTestimonyVisit)(const void *,const void *,const uint64_t *,size_t,double,void *);

static void *pg_down_array(size_t count,size_t width,bool zero)
{
    count=Max(count,(size_t)1);
    if(count>MaxAllocHugeSize/width)
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("laplace containment workspace exceeds addressable memory")));
    return palloc_extended(count*width,MCXT_ALLOC_HUGE|(zero?MCXT_ALLOC_ZERO:0));
}

/* Source roots share one containment DAG. Its adjacency and topological order
 * do not depend on which root is being counted; prepare them once (Native
 * lp_dag_order), then count under each root (Native lp_dag_count, which
 * allocates nothing, so an interrupt between edges leaks nothing). */
typedef struct { const PgDag *d; const size_t *head; size_t calls; bool interruptible; } PgEdgeReader;
static int pg_edge_next(void *context, size_t node, size_t *cursor, size_t *child, uint64_t *times)
{
    PgEdgeReader *r = context;
    if (r->interruptible && !(++r->calls & 4095)) CHECK_FOR_INTERRUPTS();
    size_t k = *cursor ? r->d->e.v[*cursor - 1].next : r->head[node];
    if (k == PG_NONE) return 1;
    *cursor = k + 1; *child = r->d->e.v[k].c; *times = r->d->e.v[k].times;
    return 0;
}

static PgDown pg_down_prepare(PgDag *d)
{
    size_t n = d->n.n;
    size_t *head = pg_down_array(n,sizeof *head,false);
    for (size_t i = 0; i < n; i++) head[i] = PG_NONE;
    for (size_t k = 0; k < d->e.n; k++) {
        /* Incoming adjacency is never consumed. Reuse the edge's own link
         * for downward traversal rather than allocate another word per edge.
         * Civil Comments alone has over 290 million containment edges. */
        d->e.v[k].next = head[d->e.v[k].p];
        head[d->e.v[k].p] = k;
    }
    size_t *order = pg_down_array(n,sizeof *order,false);
    PgEdgeReader r = { .d=d,.head=head };
    if (!lp_dag_order(n, pg_edge_next, &r, order))
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace containment is not a DAG: %m")));
    return (PgDown){head,order,n};
}

static double *pg_down_from(const PgDag *d, const PgDown *walk, size_t top)
{
    double *cnt = pg_down_array(d->n.n,sizeof *cnt,false);
    PgEdgeReader r = { .d=d,.head=walk->head,.interruptible=true };
    if (!lp_dag_count(d->n.n, pg_edge_next, &r, walk->order, top, cnt))
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace containment count failed: %m")));
    return cnt;
}

static void pg_down_free(PgDown *walk)
{
    pfree(walk->head); pfree(walk->order);
}

static void pg_tally_under(const PgDag *d, const double *cnt, lp_strmap *per_claim)
{
    for (size_t k = 0; k < d->e.n; k++) {
        if (!(k & 4095)) CHECK_FOR_INTERRUPTS();
        const PgEdge *e = &d->e.v[k];
        if (!e->claim) continue;
        double c = cnt[e->p]; if (c <= 0) continue;
        bool fresh;
        uint8_t key[32]={0}; memcpy(key,d->n.v[e->c].id.b,16);
        if (e->has_dependence) memcpy(key+16,e->dependence.b,16);
        PgTally *t = lp_strmap_get(per_claim,key,sizeof key,&fresh);
        if (fresh) *t = (PgTally){ .claim=d->n.v[e->c].id,.dependence=e->dependence,
            .dependence_trust=e->dependence_trust,.has_dependence=e->has_dependence };
        lp_tally_edge(&t->t, c, e->times, e->rec, e->outcome, e->spare, e->position);
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

void lp_pg_claim_bind(PgClaimBinding *binding)
{
    binding->context=AllocSetContextCreate(CurrentMemoryContext,"Native claim binding",ALLOCSET_DEFAULT_SIZES);
    MemoryContext previous=MemoryContextSwitchTo(binding->context);
    binding->claim=lp_pg_kind_mask("claim");
    MemoryContextSwitchTo(previous);
}
static bool pg_claim_carrier(PgClaimBinding *binding,Datum path,Datum mask,const Geo *decoded)
{
    if(!(lp_pg_mask_native(mask)&binding->claim))return false;
    Geo geometry;if(!decoded){geometry=geo_of(path);decoded=&geometry;}
    size_t length=lp_path_len(decoded->p);
    if(length>PG_INT64_MAX)
        ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("laplace_path_length: run count exceeds bigint")));
    return length>=2;
}
void lp_pg_claim_frontier_row(const lp_id *id,Datum path,Datum mask,void *context)
{
    PgClaimFrontier *front=context;
    if(!pg_claim_carrier(&front->binding,path,mask,NULL))return;
    front->found++;
    if(front->front)lp_idmap_put(front->front,id,NULL);
    if(front->dag)pg_dag_node((PgDag *)front->dag,id,NULL);
}

typedef lp_vec(lp_vertex) PgVertices;
typedef struct {
    PgDag *dag;lp_idmap *seen,*front,*next,*classes;PgVertices *vertices;
    const lp_tier0_record *floor;PgClaimBinding binding;
} PgDownFrontier;
static void pg_down_frontier_row(const lp_id *id,Datum path,Datum mask,void *context)
{
    PgDownFrontier *front=context;PgDag *d=front->dag;
    size_t pi=pg_dag_node(d,id,NULL);lp_idmap_put(front->seen,id,NULL);
    Geo geometry=geo_of(path);
    d->n.v[pi].claim=pg_claim_carrier(&front->binding,path,mask,&geometry);
    int nv=(int)geometry.p.n;if(nv<=0)return;
    PgVertices *vertices=front->vertices;lp_vec_reserve(vertices,(size_t)nv);
    if(lp_path_decode(geometry.p,vertices->v,(size_t)nv)!=(size_t)nv)
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_contained_series: invalid stored path")));
    d->n.v[pi].file=vertices->v[0].said==LP_SAID_METADATA;
    const lp_trust_class *pending=NULL;lp_id pending_id={{0}};
    for(int k=0;k<nv;k++){
        const lp_vertex *v=vertices->v+k;
        if(v->said==LP_SAID_DEPENDENCE){pending=pg_dependence_class(front->classes,vertices->v,nv,k);pending_id=v->id;continue;}
        const lp_trust_class *scope=pending;pending=NULL;
        if(v->said==LP_SAID_METADATA)continue;
        size_t ci=pg_dag_node(d,&v->id,NULL);PgEdge *edge=pg_dag_edge(d,pi,ci,v);
        if(v->said==LP_SAID_CLAIM || v->said==LP_SAID_RECORD){
            edge->claim=1;edge->rec=v->said==LP_SAID_RECORD;
            if(scope){edge->has_dependence=1;edge->dependence=pending_id;edge->dependence_trust=scope->prior;}
        }
        if(!lp_tier0_contains(front->floor,&v->id) && lp_idmap_find(front->seen,&v->id)<0 && lp_idmap_find(front->front,&v->id)<0)
            lp_idmap_put(front->next,&v->id,NULL);
    }
}

typedef struct { PgVertices *vertices;size_t found; } PgRootFrontier;
static void pg_root_frontier_row(const lp_id *id,Datum path,Datum mask,void *context)
{
    PgRootFrontier *front=context;Geo geometry=geo_of(path);
    lp_vec_reserve(front->vertices,geometry.p.n);
    if(lp_path_decode(geometry.p,front->vertices->v,geometry.p.n)!=geometry.p.n)
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_contained_series: invalid stored path")));
    front->found++;
}

/* Restore testimony from persisted trunks without opening their source bytes.
 * Each Native index request is a bounded frontier, never a command per vertex. */
PG_FUNCTION_INFO_V1(laplace_contained_series);
Datum laplace_contained_series(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts=set_begin(fcinfo,"laplace_contained_series",&td);
    int n; lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n);
    lp_idmap *selected=NULL;
    if(PG_NARGS()>1){
        int count; lp_id *claims=ids_of(PG_GETARG_ARRAYTYPE_P(1),&count);
        selected=lp_idmap_new();
        for(int i=0;i<count;i++)lp_idmap_put(selected,claims+i,NULL);
        pfree(claims);
        if(!count){pfree(ids);lp_idmap_free(selected);PG_RETURN_NULL();}
    }
    if(!n){pfree(ids);lp_idmap_free(selected);PG_RETURN_NULL();}
    Oid idarr=get_fn_expr_argtype(fcinfo->flinfo,0);
    PgDag d={lp_idmap_new(),{0},{0}};
    lp_idmap *roots=lp_idmap_new(),*front=lp_idmap_new(),*seen=lp_idmap_new();
    lp_idmap *classes=lp_idmap_sized(sizeof(const lp_trust_class *));
    PgVertices vertices={0};
    for(int i=0;i<n;i++){lp_idmap_put(roots,ids+i,NULL);lp_idmap_put(front,ids+i,NULL);pg_dag_node(&d,ids+i,NULL);}
    pfree(ids);
    if(selected){
        /* An explicit claim set starts from those claims. Prove the supplied
         * roots once, then use the shared upward kernel directly; there is no
         * speculative downward graph or cardinality-based fallback. */
        PgRootFrontier probe={&vertices,0};
        lp_pg_lookup_paths(lp_idmap_keys(roots),lp_idmap_count(roots),true,pg_root_frontier_row,&probe);
        if(probe.found!=lp_idmap_count(roots))
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_contained_series: root has no stored path")));
        PgAttrRows rows={0};
        lp_pg_compute_attested_rows(lp_idmap_keys(selected),(int)lp_idmap_count(selected),idarr,0,0,roots,NULL,&rows);
        for(size_t i=0;i<rows.n;i++){
            const PgAttrRow *r=rows.v+i;
            Datum values[8]={id_datum(&r->trunk),id_datum(&r->claim),Float8GetDatum(r->games),Float8GetDatum(r->tokens),
                Float8GetDatum(r->score),Int32GetDatum((int32)r->position),
                r->has_dependence?id_datum(&r->dependence):(Datum)0,
                r->has_dependence?Float8GetDatum(r->trust):(Datum)0};
            bool nulls[8]={false,false,false,false,false,false,!r->has_dependence,!r->has_dependence};
            set_put(fcinfo,ts,td,values,nulls);
        }
        lp_vec_free(&rows);lp_vec_free(&vertices);lp_idmap_free(classes);
        lp_idmap_free(selected);lp_idmap_free(roots);lp_idmap_free(front);lp_idmap_free(seen);
        lp_idmap_free(d.m);lp_vec_free(&d.n);lp_vec_free(&d.e);
        PG_RETURN_NULL();
    }
    /* The caller supplies exact IDs. Route and read their primary indexes
     * directly; a cardinality-dependent SQL join may scan and externally sort
     * the entire physicality relation for every large frontier. */
    PgDownFrontier down={.dag=&d,.seen=seen,.classes=classes,.vertices=&vertices,.floor=lp_pg_tier0()};
    lp_pg_claim_bind(&down.binding);
    bool initial=true;
    while(lp_idmap_count(front)){
        lp_idmap *next=lp_idmap_new();down.front=front;down.next=next;
        lp_pg_lookup_paths(lp_idmap_keys(front),lp_idmap_count(front),true,pg_down_frontier_row,&down);
        lp_idmap_free(front);front=next;
        if(initial){
            for(size_t i=0;i<lp_idmap_count(roots);i++)
                if(lp_idmap_find(seen,lp_idmap_keys(roots)+i)<0)
                    ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_contained_series: root has no stored path")));
            initial=false;
        }
    }
    MemoryContextDelete(down.binding.context);
    lp_idmap_free(front);lp_vec_free(&vertices);lp_idmap_free(classes);
    /* Assertion metadata names a claim only when the child's carrier does too.
     * This is the same admission boundary used by laplace_series_snapshot. */
    for(size_t i=0;i<d.e.n;i++)if(!d.n.v[d.e.v[i].c].claim)d.e.v[i].claim=0;
    PgDown walk=pg_down_prepare(&d);
    for(size_t i=0;i<lp_idmap_count(roots);i++){
        const lp_id *root=lp_idmap_keys(roots)+i;
        size_t top=(size_t)lp_idmap_find(d.m,root);
        double *counts=pg_down_from(&d,&walk,top);lp_strmap *per_claim=lp_strmap_sized(sizeof(PgTally));
        pg_tally_under(&d,counts,per_claim);
        for(size_t j=0;j<lp_strmap_count(per_claim);j++){
            const PgTally *t=lp_strmap_at(per_claim,j);if(t->t.games<=0)continue;
            Datum values[8]={id_datum(root),id_datum(&t->claim),Float8GetDatum(t->t.games),Float8GetDatum(t->t.tokens),
                Float8GetDatum(t->t.wins),Int32GetDatum((int32)t->t.position),
                t->has_dependence?id_datum(&t->dependence):(Datum)0,
                t->has_dependence?Float8GetDatum(t->dependence_trust):(Datum)0};
            bool nulls[8]={false,false,false,false,false,false,!t->has_dependence,!t->has_dependence};
            set_put(fcinfo,ts,td,values,nulls);
        }
        lp_strmap_free(per_claim);pfree(counts);
    }
    pg_down_free(&walk);
    lp_idmap_free(selected);lp_idmap_free(roots);lp_idmap_free(seen);lp_idmap_free(d.m);lp_vec_free(&d.n);lp_vec_free(&d.e);
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

typedef struct { PgDag *dag;lp_idmap *front,*next,*seen,*classes;PgVertices *vertices;int level;PgOrdinals *ordinals; } PgUpward;
static void pg_upward_path(const lp_id *key,Datum value,Datum mask,void *context)
{
    PgUpward *walk=context;lp_id id=*key;bool first;(void)mask;
    lp_idmap_put(walk->seen,&id,&first);if(!first)return;
    Geo geometry=geo_of(value);int count=(int)geometry.p.n;if(count<=0)return;
    lp_vec_reserve(walk->vertices,(size_t)count);lp_vertex *vertices=walk->vertices->v;
    if(lp_path_decode(geometry.p,vertices,(size_t)count)!=(size_t)count)return;
    size_t parent=PG_NONE;int fresh=0;const lp_trust_class *pending=NULL;lp_id dependence={{0}};
    uint64_t ordinal=0;
    for(int k=0;k<count;k++){
        uint64_t position=ordinal;
        if(UINT64_MAX-ordinal<vertices[k].run)
            ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("Native testimony ordinal overflow")));
        ordinal+=vertices[k].run;
        if(vertices[k].said==LP_SAID_DEPENDENCE){pending=pg_dependence_class(walk->classes,vertices,count,k);dependence=vertices[k].id;continue;}
        const lp_trust_class *scope=pending;pending=NULL;
        if(lp_idmap_find(walk->front,&vertices[k].id)<0)continue;
        uint32_t said=vertices[k].said;
        if(walk->level==0 && said!=LP_SAID_CLAIM && said!=LP_SAID_RECORD)continue;
        if(walk->level>0 && said==LP_SAID_METADATA)continue;
        if(parent==PG_NONE){parent=pg_dag_node(walk->dag,&id,&fresh);walk->dag->n.v[parent].file=vertices[0].said==LP_SAID_METADATA;}
        size_t child=pg_dag_node(walk->dag,&vertices[k].id,NULL);
        PgEdge *edge=pg_dag_edge(walk->dag,parent,child,vertices+k);
        if(walk->ordinals)lp_push(walk->ordinals,position);
        if(walk->level==0){edge->claim=1;edge->rec=said==LP_SAID_RECORD;
            if(scope){edge->has_dependence=1;edge->dependence=dependence;edge->dependence_trust=scope->prior;}}
    }
    if(parent!=PG_NONE && fresh)lp_idmap_put(walk->next,&id,NULL);
}

void lp_pg_witness_metadata(const lp_id *id,const lp_id *lineage,double trust,void *context)
{
    PgSeriesWitness *w=lp_idmap_get((lp_idmap *)context,id,NULL);
    *w=(PgSeriesWitness){.trust=trust,.has_lineage=lineage!=NULL};
    if(lineage)w->lineage=*lineage;
}

void lp_pg_compute_attested_registry(const lp_id *claims, int n, Oid idarr, int witnessed_only,
                                     int include_files, const lp_idmap *roots,
                                     const lp_idmap *known_witnesses, PgAttrRows *out,lp_idmap **registry,
                                     PgTestimonyVisit testimony,void *testimony_context)
{
    (void)idarr;
    PgDag d = { lp_idmap_new(), {0}, {0} };
    lp_idmap *front = lp_idmap_new();
    if(roots && !known_witnesses){
        /* The downward API admits assertions by the child's carrier, not
         * solely by an incoming edge. Preserve that boundary in either
         * direction, including callers selecting non-claim content. */
        if(n){
            PgClaimFrontier selected={.front=front,.dag=&d};lp_pg_claim_bind(&selected.binding);
            lp_pg_lookup_paths(claims,(size_t)n,true,lp_pg_claim_frontier_row,&selected);
            MemoryContextDelete(selected.binding.context);
        }
    }else for (int i = 0; i < n; i++) lp_idmap_put(front, &claims[i], NULL);
    /* The complete input remains an index selection, not a materialized DAG.
     * Admit a node only when a retained assertion edge actually names it.
     * Absent/unasserted operands cannot produce provenance; allocating one
     * root and reducing a full node array for each made sparse misses O(n^2). */
    PgVertices vx = {0};
    PgOrdinals ordinals={0};
    lp_idmap *classes=lp_idmap_sized(sizeof(const lp_trust_class *));
    for (int lvl = 0; lp_idmap_count(front); lvl++) {
        size_t count = lp_idmap_count(front);
        lp_idmap *next = lp_idmap_new();
        lp_idmap *seen = lp_idmap_new();
        PgUpward walk={&d,front,next,seen,classes,&vx,lvl,testimony?&ordinals:NULL};
        lp_pg_lookup_containers(lp_idmap_keys(front),count,false,lvl==0,pg_upward_path,&walk);
        elog(DEBUG1,"Native containment: level %d; selected %zu; next %zu; nodes %zu; edges %zu",
             lvl,count,lp_idmap_count(next),d.n.n,d.e.n);
        lp_idmap_free(seen);
        lp_idmap_free(front);
        front = next;
    }
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
    lp_idmap *wmap = known_witnesses ? (lp_idmap *)known_witnesses : lp_idmap_sized(sizeof(PgSeriesWitness));
    if (!known_witnesses && rids.n > 0) {
        lp_pg_lookup_witnesses(rids.v,rids.n,registry!=NULL,lp_pg_witness_metadata,wmap);
    }

    for (size_t i = 0; i < d.n.n; i++)
        if (roots ? lp_idmap_find(roots,&d.n.v[i].id)>=0
                  : d.n.v[i].nparents == 0 || (include_files && d.n.v[i].file) || lp_idmap_lookup(wmap, &d.n.v[i].id)) lp_push(&rix, i);

    PgDown walk=pg_down_prepare(&d);
    for (size_t r = 0; r < rix.n; r++) {
        size_t top = rix.v[r];
        lp_id tid = d.n.v[top].id;
        const double *wtr = lp_idmap_lookup(wmap, &tid);
        int is_w = (wtr != NULL);
        double tr = is_w ? *wtr : 0.0;
        if (witnessed_only && (!is_w || tr <= 0.0)) continue;
        if(testimony){
            if(is_w)testimony(&d,&walk,ordinals.v,top,tr,testimony_context);
            continue;
        }
        double *cnt = pg_down_from(&d, &walk, top);
        lp_strmap *per_claim = lp_strmap_sized(sizeof(PgTally));
        pg_tally_under(&d, cnt, per_claim);
        for (size_t i = 0; i < lp_strmap_count(per_claim); i++) {
            const PgTally *tl = lp_strmap_at(per_claim, i);
            if (tl->t.games <= 0) continue;
            PgAttrRow row = { .claim=tl->claim,.trunk=tid,.games=tl->t.games,.tokens=tl->t.tokens,.score=tl->t.wins,
                .position=tl->t.position,.trust=tl->has_dependence ? tl->dependence_trust : tr,.witness=is_w,
                .dependence=tl->dependence,.source_trust=tr,.has_dependence=tl->has_dependence };
            lp_push(out, row);
        }
        lp_strmap_free(per_claim);
        pfree(cnt);
    }
    pg_down_free(&walk);
    lp_vec_free(&ordinals);
    if(out)lp_sort(out->v, out->n, sizeof(PgAttrRow), by_attr_row);

    if(registry)*registry=wmap;
    else if (!known_witnesses) lp_idmap_free(wmap);
    lp_vec_free(&rix); lp_vec_free(&rids);
    lp_idmap_free(d.m); lp_vec_free(&d.n); lp_vec_free(&d.e);
}

void lp_pg_compute_attested_rows(const lp_id *claims,int n,Oid idarr,int witnessed_only,
                                 int include_files,const lp_idmap *roots,const lp_idmap *known_witnesses,PgAttrRows *out)
{
    lp_pg_compute_attested_registry(claims,n,idarr,witnessed_only,include_files,roots,known_witnesses,out,NULL,NULL,NULL);
}

typedef struct { PgAttrRows *out; const lp_idmap *roots; lp_idmap *witnesses; } PgAttestedRead;
static void pg_attested_read_row(TupleTableSlot *slot, void *context)
{
    PgAttestedRead *read = context;
    bool missing; Datum val;
    PgAttrRow row = { .witness = 1 };
    val = slot_getattr(slot, 1, &missing);
    if (missing) ereport(ERROR, (errmsg("laplace attestation: null claim")));
    memcpy(row.claim.b, DatumGetPointer(val), 16);
    val = slot_getattr(slot, 2, &missing);
    if (missing) ereport(ERROR, (errmsg("laplace attestation: null witness")));
    memcpy(row.trunk.b, DatumGetPointer(val), 16);
    if (read->roots && lp_idmap_find(read->roots, &row.trunk) < 0) return;
    val = slot_getattr(slot, 3, &missing);
    row.has_dependence = !missing;
    if (!missing) memcpy(row.dependence.b, DatumGetPointer(val), 16);
    val = slot_getattr(slot, 4, &missing);
    int64 games = missing ? 0 : DatumGetInt64(val);
    val = slot_getattr(slot, 5, &missing);
    int64 tokens = missing ? games : DatumGetInt64(val);
    val = slot_getattr(slot, 6, &missing);
    double mean_score = missing ? 0.0 : DatumGetFloat8(val);
    val = slot_getattr(slot, 7, &missing);
    row.position = missing ? 0 : (uint32_t)DatumGetInt32(val);
    val = slot_getattr(slot, 8, &missing);
    row.trust = missing ? 0.0 : DatumGetFloat8(val);
    row.source_trust = row.trust;
    row.games = (double)games;
    row.tokens = (double)(tokens >= games ? tokens : games);
    row.score = (double)games * mean_score;
    if (read->witnesses) {
        bool fresh;
        PgSeriesWitness *w = lp_idmap_get(read->witnesses, &row.trunk, &fresh);
        if (fresh || !row.has_dependence) {
            w->trust = row.trust;
            w->has_lineage = row.has_dependence;
            if (row.has_dependence) w->lineage = row.dependence;
        }
    }
    lp_push(read->out, row);
}

void lp_pg_read_attestation_rows(const lp_id *claims, int n, const lp_idmap *roots, PgAttrRows *out, lp_idmap *witnesses)
{
    if (n <= 0) return;
    Oid attestation = lp_pg_relation("attestation");
    const char *selected[] = {"claim", "witness", "lineage", "games", "tokens", "score", "position", "trust"};
    const char *column;
    Relation parent = lp_pg_read_parent(attestation, &column, selected, 8);
    Oid native_id = get_atttype(attestation, get_attnum(attestation, "claim"));
    Oid idarray = get_array_type(native_id);
    table_close(parent, NoLock);
    PgAttestedRead read = { .out = out, .roots = roots, .witnesses = witnesses };
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_read_attestation_rows: SPI connect failed")));
    SPIPlanPtr plan = lp_pg_prepare_cursor(
        "SELECT claim,witness,lineage,games,tokens,score,\"position\",trust FROM @extschema@.attestation WHERE claim OPERATOR(@extschema@.=) ANY($1)",
        1, &idarray, 0);
    if (!plan) ereport(ERROR, (errmsg("laplace_read_attestation_rows: plan failed")));
    size_t window = frontier_window_ids();
    for (size_t at = 0; at < (size_t)n;) {
        size_t amount = Min(window, (size_t)n - at);
        ArrayType *wanted = id_array_of(native_id, claims + at, (int)amount);
        ParamListInfo params = makeParamList(1);
        params->params[0] = (ParamExternData){ .ptype = idarray, .value = PointerGetDatum(wanted), .pflags = 0 };
        lp_pg_stream(plan, params, pg_attested_read_row, &read);
        pfree(params); pfree(wanted); at += amount;
    }
    SPI_freeplan(plan); SPI_finish();
    if (out->n) lp_sort(out->v, out->n, sizeof(PgAttrRow), by_attr_row);
}

/* A stage-resident claim set stays a set: one semi-join lets PostgreSQL hash or merge
 * the staged claims against every attestation partition, instead of binding the whole
 * set into an array parameter that each partition re-sorts and rechecks per tuple. */
void lp_pg_read_attestation_stage(const char *selection, PgAttrRows *out, lp_idmap *witnesses)
{
    PgAttestedRead read = { .out = out, .roots = NULL, .witnesses = witnesses };
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_read_attestation_stage: SPI connect failed")));
    char *sql = psprintf("SELECT a.claim,a.witness,a.lineage,a.games,a.tokens,a.score,a.\"position\",a.trust"
        " FROM @extschema@.attestation a WHERE EXISTS(SELECT FROM %s c WHERE c.claim OPERATOR(@extschema@.=) a.claim)", selection);
    SPIPlanPtr plan = lp_pg_prepare_cursor(sql, 0, NULL, CURSOR_OPT_NO_SCROLL);
    if (!plan) ereport(ERROR, (errmsg("laplace_read_attestation_stage: plan failed")));
    lp_pg_stream(plan, NULL, pg_attested_read_row, &read);
    SPI_freeplan(plan); pfree(sql); SPI_finish();
    if (out->n) lp_sort(out->v, out->n, sizeof(PgAttrRow), by_attr_row);
}

/* Each claim's evidence is its stored attestations; a claim with none stored yet is
 * still read from containment. Per claim, never per batch: one published claim must
 * not hide the containment evidence of the others asked with it. registry, when given,
 * receives the containment witnesses not already in it. */
void lp_pg_attested_unstored(const lp_id *claims,int n,Oid idarr,int include_files,PgAttrRows *rows,lp_idmap *registry)
{
    if (n <= 0) return;
    lp_idmap *stored = lp_idmap_new();
    for (size_t i = 0; i < rows->n; i++) lp_idmap_put(stored, &rows->v[i].claim, NULL);
    lp_id *rest = palloc(sizeof(lp_id) * (size_t)n); int nrest = 0;
    lp_idmap *asked = lp_idmap_new();
    for (int i = 0; i < n; i++) {
        bool fresh; lp_idmap_put(asked, &claims[i], &fresh);
        if (fresh && lp_idmap_find(stored, &claims[i]) < 0) rest[nrest++] = claims[i];
    }
    lp_idmap_free(asked); lp_idmap_free(stored);
    if (nrest) {
        PgAttrRows found = {0}; lp_idmap *witnesses = NULL;
        lp_pg_compute_attested_registry(rest, nrest, idarr, 0, include_files, NULL, NULL, &found, registry ? &witnesses : NULL, NULL, NULL);
        for (size_t i = 0; i < found.n; i++) lp_push(rows, found.v[i]);
        lp_vec_free(&found);
        if (witnesses) {
            for (size_t i = 0; i < lp_idmap_count(witnesses); i++) {
                bool fresh; PgSeriesWitness *w = lp_idmap_get(registry, lp_idmap_keys(witnesses) + i, &fresh);
                if (fresh) *w = *(const PgSeriesWitness *)lp_idmap_at(witnesses, i);
            }
            lp_idmap_free(witnesses);
        }
        if (rows->n) lp_sort(rows->v, rows->n, sizeof(PgAttrRow), by_attr_row);
    }
    pfree(rest);
}

/* The source-facing provenance API remains one row per corpus/container. The
 * series API below retains each explicit dependence scope for Native solving. */
void lp_pg_rollup_sources(PgAttrRows *rows){
    lp_strmap *map=lp_strmap_sized(sizeof(PgAttrRow));
    for (size_t i=0; i<rows->n; i++) {
        const PgAttrRow *r=rows->v+i; uint8_t key[32]; memcpy(key,r->claim.b,16); memcpy(key+16,r->trunk.b,16);
        bool fresh; PgAttrRow *v=lp_strmap_get(map,key,sizeof key,&fresh);
        if (fresh) { *v=*r; v->trust=r->source_trust; v->has_dependence=0; memset(&v->dependence,0,sizeof v->dependence); }
        else { v->games+=r->games; v->tokens+=r->tokens; v->score+=r->score; v->fisher+=r->fisher;
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
    PgAttrRows rows={0};
    lp_pg_read_attestation_rows(claims,n,NULL,&rows,NULL);
    lp_pg_attested_unstored(claims,n,idarr,0,&rows,NULL);
    for (size_t i=0; i<rows.n; i++) {
        const PgAttrRow *r=rows.v+i;
        Datum v[9]={id_datum(&r->claim),id_datum(&r->trunk),Float8GetDatum(r->games),Float8GetDatum(r->tokens),
            Float8GetDatum(r->score),Int32GetDatum((int32)r->position),Float8GetDatum(r->trust),BoolGetDatum(r->witness!=0),
            r->has_dependence ? id_datum(&r->dependence) : (Datum)0};
        bool nl[9]={false,false,false,false,false,false,false,false,!r->has_dependence};
        set_put(fcinfo,ts,td,v,nl);
    }
    lp_vec_free(&rows); PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_attested);
Datum laplace_attested(PG_FUNCTION_ARGS)
{
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_attested", &td);
    int n; lp_id *claims = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (!n) PG_RETURN_NULL();
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0);
    PgAttrRows rows = {0};
    int include_files = PG_NARGS() > 1 && !PG_ARGISNULL(1) && PG_GETARG_BOOL(1);
    if (!include_files) lp_pg_read_attestation_rows(claims, n, NULL, &rows, NULL);
    lp_pg_attested_unstored(claims, n, idarr, include_files, &rows, NULL);
    lp_pg_rollup_sources(&rows);
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
    PG_RETURN_NULL();
}

typedef struct {
    FunctionCallInfo call;Tuplestorestate *rows;TupleDesc descriptor;
    lp_idmap *sources;lp_id calculation;
} PgTestimony;

static uint64_t pg_testimony_product(uint64_t a,uint64_t b)
{
    if(b && a>(uint64_t)PG_INT64_MAX/b)
        ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("Native testimony multiplicity exceeds bigint")));
    return a*b;
}

/* Exact occurrence arithmetic over the same indexed DAG as standings and
 * attribution. Outcomes come from assertion vertices, never a folded score. */
static void pg_testimony_root(const void *dag_ptr,const void *walk_ptr,const uint64_t *ordinals,size_t top,double trust,void *context)
{
    const PgDag *d=dag_ptr;const PgDown *walk=walk_ptr;
    PgTestimony *out=context;const lp_id *source=&d->n.v[top].id;
    if(out->sources && lp_idmap_find(out->sources,source)<0)return;
    uint64_t *counts=pg_down_array(d->n.n,sizeof(*counts),false);
    PgEdgeReader reader={.d=d,.head=walk->head,.interruptible=true};
    if(!lp_dag_count_exact(d->n.n,pg_edge_next,&reader,walk->order,top,counts))
        ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("Native testimony multiplicity exceeds bigint")));
    for(size_t i=0;i<d->n.n;i++)if(counts[i]>(uint64_t)PG_INT64_MAX)
        ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("Native testimony multiplicity exceeds bigint")));
    for(size_t k=0;k<d->e.n;k++){
        if(!(k&4095))CHECK_FOR_INTERRUPTS();
        const PgEdge *e=d->e.v+k;if(!e->claim || !counts[e->p])continue;
        if(ordinals[k]>(uint64_t)PG_INT64_MAX)
            ereport(ERROR,(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),errmsg("Native testimony ordinal exceeds bigint")));
        uint64_t tokens=pg_testimony_product(counts[e->p],e->times);
        uint64_t occurrences=e->rec?tokens:counts[e->p];
        bool scored=e->outcome==LP_OUTCOME_SCORE;
        int16 outcome=e->outcome==LP_OUTCOME_WIN?2:e->outcome==LP_OUTCOME_DRAW?1:0;
        /* Fixed-width IDs are borrowed until tuplestore copies this row. */
        Datum values[]={PointerGetDatum(&d->n.v[e->c].id),PointerGetDatum(source),PointerGetDatum(&d->n.v[e->p].id),
            Int64GetDatum((int64)ordinals[k]),Int16GetDatum(outcome),Int64GetDatum((int64)occurrences),
            Int64GetDatum((int64)tokens),Float8GetDatum(lp_outcome_score(e->outcome,e->spare)),
            Int32GetDatum((int32)e->position),Float8GetDatum(e->has_dependence?e->dependence_trust:trust),
            e->has_dependence?PointerGetDatum(&e->dependence):(Datum)0,
            BoolGetDatum(e->has_dependence && lp_id_eq(&e->dependence,&out->calculation))};
        bool nulls[lengthof(values)]={false};nulls[4]=scored;nulls[10]=!e->has_dependence;
        set_put(out->call,out->rows,out->descriptor,values,nulls);
    }
    pfree(counts);
}

PG_FUNCTION_INFO_V1(laplace_testimony);
Datum laplace_testimony(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_testimony",&descriptor);
    if(PG_ARGISNULL(0))PG_RETURN_NULL();
    int n;lp_id *claims=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n);if(!n)PG_RETURN_NULL();
    PgTestimony out={.call=fcinfo,.rows=rows,.descriptor=descriptor};
    const lp_trust_class *calculation=lp_trust_class_named("DerivedCalculation");
    if(!calculation || !lp_text_id(lp_pg_text_context(),calculation->label,strlen(calculation->label),&out.calculation))
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native calculation dependence class is unavailable")));
    if(!PG_ARGISNULL(1)){
        int count;lp_id *sources=ids_of(PG_GETARG_ARRAYTYPE_P(1),&count);out.sources=lp_idmap_new();
        for(int i=0;i<count;i++)lp_idmap_put(out.sources,sources+i,NULL);
        pfree(sources);
        if(!count){pfree(claims);lp_idmap_free(out.sources);PG_RETURN_NULL();}
    }
    PG_TRY();{
        lp_pg_compute_attested_registry(claims,n,get_fn_expr_argtype(fcinfo->flinfo,0),0,0,NULL,NULL,NULL,NULL,
            pg_testimony_root,&out);
    }PG_FINALLY();{pfree(claims);lp_idmap_free(out.sources);}PG_END_TRY();
    PG_RETURN_NULL();
}
