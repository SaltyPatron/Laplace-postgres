/* Native containment DAG: upward testimony and downward stored series share
 * edge decoding, run counts, dependence classes and tally reduction. */
#include "pg_core.h"
#include "geo.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/relscan.h"
#include "access/genam.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/htup_details.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
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

/* The walk down one DAG, prepared once for all its tops: each node's first outgoing edge, the topological order and
 * each node's place in it, and the scratch a top's count over its own reach uses (lp_dag_count_reach), so counting
 * under R tops costs their reaches, not R times the whole DAG. */
typedef struct { size_t *head, *order, *rank, n; double *counts; uint8_t *mark; size_t *reach, *stack, *edges; } PgDown;
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
    size_t *rank = pg_down_array(n,sizeof *rank,false);
    for (size_t i = 0; i < n; i++) rank[order[i]] = i;
    return (PgDown){ .head=head,.order=order,.rank=rank,.n=n,
        .counts=pg_down_array(n,sizeof(double),true),.mark=pg_down_array(n,1,true),
        .reach=pg_down_array(n,sizeof(size_t),false),.stack=pg_down_array(n,sizeof(size_t),false),
        .edges=pg_down_array(Max(d->e.n,(size_t)1),sizeof(size_t),false) };
}

/* The counts under top over its reach (walk->counts, set on walk->reach only); the reach's size. */
static size_t pg_down_from(const PgDag *d, PgDown *walk, size_t top)
{
    PgEdgeReader r = { .d=d,.head=walk->head,.interruptible=true };
    size_t found = lp_dag_count_reach(walk->n, pg_edge_next, &r, walk->order, walk->rank, top, walk->counts, walk->mark, walk->reach, walk->stack);
    if (found == (size_t)-1)
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace containment count failed: %m")));
    return found;
}
static void pg_down_done(PgDown *walk, size_t found){ lp_dag_reach_clear(walk->reach, found, walk->counts, walk->mark); }

static void pg_down_free(PgDown *walk)
{
    pfree(walk->head); pfree(walk->order); pfree(walk->rank); pfree(walk->counts); pfree(walk->mark);
    pfree(walk->reach); pfree(walk->stack); pfree(walk->edges);
}

static int by_edge(const void *a, const void *b){ size_t x = *(const size_t *)a, y = *(const size_t *)b; return x < y ? -1 : x > y; }
/* Each claim edge below top, tallied in the DAG's edge order (the order every reader of it tallies in, so the sums are
 * the same bits): the edges of the reach's nodes alone. */
static void pg_tally_under(const PgDag *d, PgDown *walk, size_t found, lp_strmap *per_claim)
{
    size_t ne = 0;
    for (size_t i = 0; i < found; i++)
        for (size_t k = walk->head[walk->reach[i]]; k != PG_NONE; k = d->e.v[k].next) if (d->e.v[k].claim) walk->edges[ne++] = k;
    qsort(walk->edges, ne, sizeof *walk->edges, by_edge);
    const double *cnt = walk->counts;
    for (size_t j = 0; j < ne; j++) {
        size_t k = walk->edges[j];
        if (!(j & 4095)) CHECK_FOR_INTERRUPTS();
        const PgEdge *e = &d->e.v[k];
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
void lp_pg_contained_series(const lp_id *roots_in, int nroots, PgContainedSeriesRows *out)
{
    if (!nroots) return;
    PgDag d = { lp_idmap_new(), {0}, {0} };
    lp_idmap *roots = lp_idmap_new(), *front = lp_idmap_new(), *seen = lp_idmap_new();
    lp_idmap *classes = lp_idmap_sized(sizeof(const lp_trust_class *));
    PgVertices vertices = {0};
    for (int i = 0; i < nroots; i++) {
        lp_idmap_put(roots, roots_in + i, NULL);
        lp_idmap_put(front, roots_in + i, NULL);
        pg_dag_node(&d, roots_in + i, NULL);
    }
    PgDownFrontier down = { .dag = &d, .seen = seen, .classes = classes, .vertices = &vertices, .floor = lp_pg_tier0() };
    lp_pg_claim_bind(&down.binding);
    bool initial = true;
    while (lp_idmap_count(front)) {
        lp_idmap *next = lp_idmap_new(); down.front = front; down.next = next;
        lp_pg_lookup_paths(lp_idmap_keys(front), lp_idmap_count(front), true, pg_down_frontier_row, &down);
        lp_idmap_free(front); front = next;
        if (initial) {
            for (size_t i = 0; i < lp_idmap_count(roots); i++)
                if (lp_idmap_find(seen, lp_idmap_keys(roots) + i) < 0)
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_contained_series: root has no stored path")));
            initial = false;
        }
    }
    MemoryContextDelete(down.binding.context);
    lp_idmap_free(front); lp_vec_free(&vertices); lp_idmap_free(classes);
    for (size_t i = 0; i < d.e.n; i++) if (!d.n.v[d.e.v[i].c].claim) d.e.v[i].claim = 0;
    PgDown walk = pg_down_prepare(&d);
    for (size_t i = 0; i < lp_idmap_count(roots); i++) {
        const lp_id *root = lp_idmap_keys(roots) + i;
        size_t top = (size_t)lp_idmap_find(d.m, root);
        size_t found = pg_down_from(&d, &walk, top);
        lp_strmap *per_claim = lp_strmap_sized(sizeof(PgTally));
        pg_tally_under(&d, &walk, found, per_claim); pg_down_done(&walk, found);
        for (size_t j = 0; j < lp_strmap_count(per_claim); j++) {
            const PgTally *t = lp_strmap_at(per_claim, j); if (t->t.games <= 0) continue;
            PgContainedSeriesRow row = {
                .trunk = *root,
                .claim = t->claim,
                .dependence_root = t->dependence,
                .games = t->t.games,
                .tokens = t->t.tokens,
                .score = t->t.wins,
                .dependence_trust = t->dependence_trust,
                .position = (int32)t->t.position,
                .has_dependence = t->has_dependence != 0
            };
            lp_push(out, row);
        }
        lp_strmap_free(per_claim);
    }
    pg_down_free(&walk);
    lp_idmap_free(roots); lp_idmap_free(seen); lp_idmap_free(d.m); lp_vec_free(&d.n); lp_vec_free(&d.e);
}

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
    if(selected){
        PgDag d={lp_idmap_new(),{0},{0}};
        lp_idmap *roots=lp_idmap_new(),*front=lp_idmap_new(),*seen=lp_idmap_new();
        lp_idmap *classes=lp_idmap_sized(sizeof(const lp_trust_class *));
        PgVertices vertices={0};
        for(int i=0;i<n;i++){lp_idmap_put(roots,ids+i,NULL);lp_idmap_put(front,ids+i,NULL);pg_dag_node(&d,ids+i,NULL);}
        pfree(ids);
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
    PgContainedSeriesRows out={0};
    lp_pg_contained_series(ids,n,&out);
    pfree(ids);
    for(size_t i=0;i<out.n;i++){
        const PgContainedSeriesRow *r=out.v+i;
        Datum values[8]={id_datum(&r->trunk),id_datum(&r->claim),Float8GetDatum(r->games),Float8GetDatum(r->tokens),
            Float8GetDatum(r->score),Int32GetDatum(r->position),
            r->has_dependence?id_datum(&r->dependence_root):(Datum)0,
            r->has_dependence?Float8GetDatum(r->dependence_trust):(Datum)0};
        bool nulls[8]={false,false,false,false,false,false,!r->has_dependence,!r->has_dependence};
        set_put(fcinfo,ts,td,values,nulls);
    }
    lp_vec_free(&out);
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
        size_t found = pg_down_from(&d, &walk, top);
        lp_strmap *per_claim = lp_strmap_sized(sizeof(PgTally));
        pg_tally_under(&d, &walk, found, per_claim);
        pg_down_done(&walk, found);
        for (size_t i = 0; i < lp_strmap_count(per_claim); i++) {
            const PgTally *tl = lp_strmap_at(per_claim, i);
            if (tl->t.games <= 0) continue;
            PgAttrRow row = { .claim=tl->claim,.trunk=tid,.games=tl->t.games,.tokens=tl->t.tokens,.score=tl->t.wins,
                .position=tl->t.position,.trust=tl->has_dependence ? tl->dependence_trust : tr,.witness=is_w,
                .dependence=tl->dependence,.source_trust=tr,.has_dependence=tl->has_dependence };
            lp_push(out, row);
        }
        lp_strmap_free(per_claim);
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

void lp_pg_read_attestation_rows(const lp_id *claims, int n, const lp_idmap *roots, PgAttrRows *out, lp_idmap *witnesses)
{
    if (n <= 0) return;
    lp_pg_read_attestation_pk(claims, n, roots, out, witnesses);
    if (out->n) lp_sort(out->v, out->n, sizeof(PgAttrRow), by_attr_row);
}

/* Scan the caller's stage or temp claim selection directly and resolve each
 * unique claim against the canonical attestation primary-key partitions. */
void lp_pg_read_attestation_stage(Oid selection_relid, PgAttrRows *out, lp_idmap *witnesses)
{
    if (!OidIsValid(selection_relid)) return;
    Relation rel = table_open(selection_relid, AccessShareLock);
    AttrNumber ac = get_attnum(selection_relid, "claim");
    if (ac == InvalidAttrNumber)
        ereport(ERROR, (errmsg("laplace_read_attestation_stage: relation \"%s\" has no claim column", RelationGetRelationName(rel))));
    lp_idmap *claims = lp_idmap_new();
    TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
    TupleTableSlot *slot = table_slot_create(rel, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool missing;
        Datum val = slot_getattr(slot, ac, &missing);
        if (!missing) {
            lp_id claim;
            memcpy(claim.b, DatumGetPointer(val), sizeof(lp_id));
            lp_idmap_put(claims, &claim, NULL);
        }
        ExecClearTuple(slot);
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    table_close(rel, AccessShareLock);
    size_t count = lp_idmap_count(claims);
    if (count > (size_t)INT_MAX)
        ereport(ERROR, (errmsg("laplace_read_attestation_stage: claim selection exceeds supported count")));
    if (count > 0) lp_pg_read_attestation_rows(lp_idmap_keys(claims), (int)count, NULL, out, witnesses);
    lp_idmap_free(claims);
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

static int by_export_id(const void *a, const void *b)
{
    return memcmp(((const lp_id *)a)->b, ((const lp_id *)b)->b, sizeof(lp_id));
}

static int by_export_row(const void *a, const void *b)
{
    const PgAttrRow *x = a, *y = b;
    int c = memcmp(x->claim.b, y->claim.b, sizeof(lp_id));
    if (c) return c;
    return memcmp(x->trunk.b, y->trunk.b, sizeof(lp_id));
}

/* Direct-AM full-estate attestation replay export streamed in sorted claim windows. */
PG_FUNCTION_INFO_V1(laplace_replay_export);
Datum laplace_replay_export(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_replay_export", &td);
    int include_files = (PG_NARGS() > 0 && !PG_ARGISNULL(0) && PG_GETARG_BOOL(0)) ? 1 : 0;

    const char *pcols[] = { "entity", "path", "mask" }, *ccols[] = { "claim" }, *acols[] = { "claim", "witness" };
    const char *col = NULL;
    Relation pp = lp_pg_read_parent(lp_pg_relation("physicality"), &col, pcols, 3);
    table_close(pp, NoLock);
    Relation cp = lp_pg_read_parent(lp_pg_relation("consensus"), &col, ccols, 1);
    table_close(cp, NoLock);
    Relation ap = lp_pg_read_parent(lp_pg_relation("attestation"), &col, acols, 2);
    table_close(ap, NoLock);

    lp_mask claim_bit = lp_pg_kind_mask("claim");
    Snapshot snap = GetActiveSnapshot();
    lp_idmap *claim_set = lp_idmap_new();
    lp_id *sorted_claims = NULL;
    MemoryContext old_ctx = CurrentMemoryContext;
    MemoryContext row_ctx = AllocSetContextCreate(old_ctx, "laplace_replay_export_row", ALLOCSET_DEFAULT_SIZES);

    PG_TRY();
    {
        const Oid *con_parts = NULL, *path_parts = NULL;
        int n_con = lp_pg_canonical_partitions("consensus", &con_parts, NULL, NULL, NULL);
        for (int p = 0; p < n_con; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(con_parts[p], AccessShareLock);
            AttrNumber ac = get_attnum(con_parts[p], "claim");
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool nc = false;
                Datum dc = slot_getattr(slot, ac, &nc);
                if (!nc) lp_idmap_put(claim_set, (const lp_id *)DatumGetPointer(dc), NULL);
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }

        int n_path = lp_pg_canonical_partitions("physicality", &path_parts, NULL, NULL, NULL);
        for (int p = 0; p < n_path; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(path_parts[p], AccessShareLock);
            AttrNumber ae = get_attnum(path_parts[p], "entity");
            AttrNumber apath = get_attnum(path_parts[p], "path");
            AttrNumber am = get_attnum(path_parts[p], "mask");
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool ne = false, np = false, nm = false;
                Datum de = slot_getattr(slot, ae, &ne);
                Datum dm = slot_getattr(slot, am, &nm);
                if (!ne && !nm && (lp_pg_mask_native(dm) & claim_bit) != 0) {
                    Datum dp = slot_getattr(slot, apath, &np);
                    if (!np) {
                        MemoryContextSwitchTo(row_ctx);
                        size_t len = lp_path_len(geo_of(dp).p);
                        MemoryContextSwitchTo(old_ctx);
                        MemoryContextReset(row_ctx);
                        if (len >= 2) lp_idmap_put(claim_set, (const lp_id *)DatumGetPointer(de), NULL);
                    }
                }
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }

        size_t nclaims = lp_idmap_count(claim_set);
        if (nclaims > 0) {
            sorted_claims = palloc_extended(nclaims * sizeof(lp_id), MCXT_ALLOC_HUGE);
            memcpy(sorted_claims, lp_idmap_keys(claim_set), nclaims * sizeof(lp_id));
            lp_idmap_free(claim_set);
            claim_set = NULL;
            lp_sort(sorted_claims, nclaims, sizeof(lp_id), by_export_id);

            const size_t window = 4096;
            for (size_t begin = 0; begin < nclaims; begin += window) {
                CHECK_FOR_INTERRUPTS();
                size_t wn = Min(window, nclaims - begin);
                PgAttrRows rows = {0};
                if (!include_files)
                    lp_pg_read_attestation_rows(sorted_claims + begin, (int)wn, NULL, &rows, NULL);
                lp_pg_attested_unstored(sorted_claims + begin, (int)wn, InvalidOid, include_files, &rows, NULL);
                lp_pg_rollup_sources(&rows);
                size_t kept = 0;
                for (size_t i = 0; i < rows.n; i++) {
                    if (rows.v[i].witness || include_files)
                        rows.v[kept++] = rows.v[i];
                }
                if (kept > 1) lp_sort(rows.v, kept, sizeof(PgAttrRow), by_export_row);
                for (size_t i = 0; i < kept; i++) {
                    const PgAttrRow *r = &rows.v[i];
                    Datum v[6] = {
                        id_datum(&r->claim),
                        id_datum(&r->trunk),
                        Float8GetDatum(r->games),
                        Float8GetDatum(r->tokens),
                        Float8GetDatum(r->score),
                        Int32GetDatum((int32)r->position)
                    };
                    bool nl[6] = { false, false, false, false, false, false };
                    set_put(fcinfo, ts, td, v, nl);
                    pfree(DatumGetPointer(v[0]));
                    pfree(DatumGetPointer(v[1]));
                }
                lp_vec_free(&rows);
            }
        }
    }
    PG_FINALLY();
    {
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(row_ctx);
        if (claim_set) lp_idmap_free(claim_set);
        if (sorted_claims) pfree(sorted_claims);
    }
    PG_END_TRY();
    return (Datum)0;
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

typedef struct {
    lp_id witness;
    lp_id lineage;
    bool has_lineage;
    lp_id terminal;
    int8 state; /* 0 = unvisited, 1 = visiting, 2 = resolved, 3 = invalid cycle */
} WitnessNode;

typedef struct {
    lp_vec(WitnessNode) nodes;
    lp_idmap *by_id;
} WitnessGraphCtx;

static void collect_witness_node(const lp_id *id, const lp_id *lineage, double trust, void *ctx)
{
    (void)trust;
    WitnessGraphCtx *g = ctx;
    bool fresh = false;
    lp_idmap_put(g->by_id, id, &fresh);
    if (!fresh) return;
    WitnessNode w = {0};
    w.witness = *id;
    w.has_lineage = (lineage != NULL);
    if (lineage) w.lineage = *lineage;
    lp_push(&g->nodes, w);
}

static void resolve_witness_roots(WitnessGraphCtx *g, int64 *invalid_out)
{
    *invalid_out = 0;
    size_t n = g->nodes.n;
    lp_vec(size_t) stack = {0};
    for (size_t i = 0; i < n; i++) {
        if (g->nodes.v[i].state >= 2) continue;
        stack.n = 0;
        size_t cur = i;
        lp_id root = {0};
        bool cyclic = false;
        for (;;) {
            WitnessNode *wn = &g->nodes.v[cur];
            if (wn->state == 2) { root = wn->terminal; break; }
            if (wn->state == 1 || wn->state == 3) { cyclic = true; break; }
            wn->state = 1;
            lp_push(&stack, cur);
            if (!wn->has_lineage || lp_id_eq(&wn->lineage, &wn->witness)) {
                root = wn->witness;
                break;
            }
            int64 nxt = lp_idmap_find(g->by_id, &wn->lineage);
            if (nxt < 0) {
                root = wn->lineage;
                break;
            }
            cur = (size_t)nxt;
        }
        if (cyclic) {
            (*invalid_out)++;
            for (size_t k = 0; k < stack.n; k++) g->nodes.v[stack.v[k]].state = 3;
        } else {
            for (size_t k = 0; k < stack.n; k++) {
                g->nodes.v[stack.v[k]].terminal = root;
                g->nodes.v[stack.v[k]].state = 2;
            }
        }
    }
    lp_vec_free(&stack);
}

typedef struct {
    int32 list_idx;
    uint32 slot;
    size_t claim_ord;
} SlotClaimHit;

typedef struct {
    const lp_highway *hw;
    lp_idmap *matched_claim_map;
    lp_vec(SlotClaimHit) hits;
} AuditPathCtx;

static void audit_visit_claim_path(const lp_id *claim, Datum path, Datum mask, void *ctx)
{
    (void)mask;
    AuditPathCtx *ac = ctx;
    Geo g = geo_of(path);
    size_t n = lp_path_len(g.p);
    int64 cord = -1;
    for (size_t i = 0; i < n; i++) {
        lp_id vid = lp_path_id(g.p, i);
        if (!lp_highway_contains(ac->hw, &vid)) continue;
        for (size_t li = 0; li < ac->hw->nlists; li++) {
            int64 slot = lp_highway_slot(ac->hw, &ac->hw->list[li], &vid);
            if (slot < 0) continue;
            if (cord < 0) cord = (int64)lp_idmap_put(ac->matched_claim_map, claim, NULL);
            lp_push(&ac->hits, ((SlotClaimHit){ (int32)li, (uint32)slot, (size_t)cord }));
        }
    }
}

typedef struct {
    size_t list_slot_ord;
    size_t root_id_ord;
} SlotRootPair;

static int cmp_size_t(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x > y) - (x < y);
}

static size_t dedup_size_t(size_t *arr, size_t n)
{
    if (!n) return 0;
    lp_sort(arr, n, sizeof(size_t), cmp_size_t);
    size_t w = 1;
    for (size_t i = 1; i < n; i++) if (arr[i] != arr[w - 1]) arr[w++] = arr[i];
    return w;
}

static lp_id slot_key_id(int32 list_idx, uint32 slot)
{
    lp_id k = {0};
    memcpy(k.b, &list_idx, sizeof(list_idx));
    memcpy(k.b + 4, &slot, sizeof(slot));
    return k;
}

/* Native Direct-AM highway provenance and 1-hop crosswalk audit (zero SPI). */
PG_FUNCTION_INFO_V1(laplace_highway_audit);
Datum laplace_highway_audit(PG_FUNCTION_ARGS)
{
    static const char *const audit_lists[8] = {
        "ili", "Synset", "fnframe", "fnfe", "fnlu", "vnclass", "pbroleset", "vaframe"
    };
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_highway_audit", &td);
    const lp_highway *hw = lp_pg_highway();

    WitnessGraphCtx wgraph = { {0}, lp_idmap_new() };
    lp_vec(lp_id) chunk_claims = {0};
    AuditPathCtx apctx = { hw, lp_idmap_new(), {0} };
    PgAttrRows att_rows = {0};
    lp_idmap *distinct_roots = lp_idmap_new();
    lp_idmap *slot_map = lp_idmap_new();

    PG_TRY();
    {
        lp_pg_scan_all_witnesses(collect_witness_node, &wgraph);
        int64 invalid_lineage = 0;
        resolve_witness_roots(&wgraph, &invalid_lineage);

        Oid con_oid = lp_pg_relation("consensus");
        AttrNumber a_claim = get_attnum(con_oid, "claim");
        const Oid *parts = NULL;
        int nparts = lp_pg_canonical_partitions("consensus", &parts, NULL, NULL, NULL);
        Snapshot snap = GetActiveSnapshot();

        for (int p = 0; p < nparts; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(parts[p], AccessShareLock);
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool isnull = true;
                Datum dc = slot_getattr(slot, a_claim, &isnull);
                if (!isnull) lp_push(&chunk_claims, *(const lp_id *)DatumGetPointer(dc));
                ExecClearTuple(slot);
                if (chunk_claims.n >= 16384) {
                    lp_pg_lookup_paths(chunk_claims.v, chunk_claims.n, false, audit_visit_claim_path, &apctx);
                    chunk_claims.n = 0;
                }
            }
            table_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
            table_close(part, NoLock);
        }
        if (chunk_claims.n > 0) {
            lp_pg_lookup_paths(chunk_claims.v, chunk_claims.n, false, audit_visit_claim_path, &apctx);
            chunk_claims.n = 0;
        }

        size_t n_mc = lp_idmap_count(apctx.matched_claim_map);
        if (n_mc > 0)
            lp_pg_read_attestation_pk(lp_idmap_keys(apctx.matched_claim_map), (int)n_mc, NULL, &att_rows, NULL);

        typedef struct { size_t claim_ord; size_t root_ord; } ClaimRootPair;
        lp_vec(ClaimRootPair) cr_pairs = {0};
        for (size_t i = 0; i < att_rows.n; i++) {
            int64 cord = lp_idmap_find(apctx.matched_claim_map, &att_rows.v[i].claim);
            if (cord < 0) continue;
            lp_id term = att_rows.v[i].trunk;
            int64 widx = lp_idmap_find(wgraph.by_id, &att_rows.v[i].trunk);
            if (widx >= 0) {
                if (wgraph.nodes.v[widx].state == 3) continue;
                if (wgraph.nodes.v[widx].state == 2) term = wgraph.nodes.v[widx].terminal;
            }
            size_t rord = lp_idmap_put(distinct_roots, &term, NULL);
            lp_push(&cr_pairs, ((ClaimRootPair){ (size_t)cord, rord }));
        }

        size_t *claim_row_begin = n_mc ? palloc(sizeof(size_t) * n_mc) : NULL;
        size_t *claim_row_end = n_mc ? palloc(sizeof(size_t) * n_mc) : NULL;
        size_t *c_counts = n_mc ? palloc0(sizeof(size_t) * n_mc) : NULL;
        for (size_t i = 0; i < cr_pairs.n; i++) c_counts[cr_pairs.v[i].claim_ord]++;
        size_t total_cr = 0;
        for (size_t i = 0; i < n_mc; i++) {
            claim_row_begin[i] = total_cr;
            claim_row_end[i] = total_cr;
            total_cr += c_counts[i];
        }
        size_t *cr_flat = total_cr ? palloc(sizeof(size_t) * total_cr) : NULL;
        for (size_t i = 0; i < cr_pairs.n; i++) {
            size_t c = cr_pairs.v[i].claim_ord;
            cr_flat[claim_row_end[c]++] = cr_pairs.v[i].root_ord;
        }
        lp_vec_free(&cr_pairs);
        if (c_counts) pfree(c_counts);

        lp_vec(SlotRootPair) sr_pairs = {0};
        for (size_t i = 0; i < apctx.hits.n; i++) {
            SlotClaimHit *h = &apctx.hits.v[i];
            lp_id sk = slot_key_id(h->list_idx, h->slot);
            size_t sord = lp_idmap_put(slot_map, &sk, NULL);
            for (size_t k = claim_row_begin[h->claim_ord]; k < claim_row_end[h->claim_ord]; k++)
                lp_push(&sr_pairs, ((SlotRootPair){ sord, cr_flat[k] }));
        }
        if (cr_flat) pfree(cr_flat);
        if (claim_row_begin) pfree(claim_row_begin);
        if (claim_row_end) pfree(claim_row_end);

        size_t n_slots = lp_idmap_count(slot_map);
        size_t *s_counts = n_slots ? palloc0(sizeof(size_t) * n_slots) : NULL;
        size_t *s_begin = n_slots ? palloc0(sizeof(size_t) * n_slots) : NULL;
        size_t *s_end = n_slots ? palloc0(sizeof(size_t) * n_slots) : NULL;
        for (size_t i = 0; i < sr_pairs.n; i++) s_counts[sr_pairs.v[i].list_slot_ord]++;
        size_t total_sr = 0;
        for (size_t i = 0; i < n_slots; i++) {
            s_begin[i] = total_sr;
            s_end[i] = total_sr;
            total_sr += s_counts[i];
        }
        size_t *s_flat = total_sr ? palloc(sizeof(size_t) * total_sr) : NULL;
        for (size_t i = 0; i < sr_pairs.n; i++) {
            size_t s = sr_pairs.v[i].list_slot_ord;
            s_flat[s_end[s]++] = sr_pairs.v[i].root_id_ord;
        }
        lp_vec_free(&sr_pairs);
        if (s_counts) pfree(s_counts);
        for (size_t i = 0; i < n_slots; i++)
            s_end[i] = s_begin[i] + dedup_size_t(&s_flat[s_begin[i]], s_end[i] - s_begin[i]);

        lp_vec(size_t) scratch = {0};

        for (int li = 0; li < 8; li++) {
            const char *lname = audit_lists[li];
            const lp_list *lst = lp_highway_list(hw, lname);
            int src_list_idx = lst ? (int)(lst - hw->list) : -1;
            int64 cached = 0, attested = 0, ind2 = 0, ind3 = 0, ind4 = 0, xw2 = 0, xw3 = 0, xw4 = 0;
            if (lst && src_list_idx >= 0) {
                for (uint32 s = 0; s < lst->count; s++) {
                    CHECK_FOR_INTERRUPTS();
                    if (!lp_highway_at(hw, lst, s)) continue;
                    cached++;

                    lp_id sk = slot_key_id(src_list_idx, s);
                    int64 sord = lp_idmap_find(slot_map, &sk);
                    size_t direct_n = 0;
                    const size_t *direct_ptr = NULL;
                    if (sord >= 0) {
                        attested++;
                        direct_n = s_end[sord] - s_begin[sord];
                        direct_ptr = &s_flat[s_begin[sord]];
                        if (direct_n >= 2) ind2++;
                        if (direct_n >= 3) ind3++;
                        if (direct_n >= 4) ind4++;
                    }

                    scratch.n = 0;
                    for (size_t k = 0; k < direct_n; k++) lp_push(&scratch, direct_ptr[k]);

                    for (size_t el = 0; el < hw->nedgelists; el++) {
                        if (strcmp(hw->edges[el].a, lname) != 0) continue;
                        const lp_list *dst_lst = lp_highway_list(hw, hw->edges[el].b);
                        if (!dst_lst) continue;
                        int dst_list_idx = (int)(dst_lst - hw->list);
                        const lp_edge *edges = NULL;
                        size_t ne = lp_highway_edges(hw, lname, s, hw->edges[el].b, &edges);
                        for (size_t k = 0; k < ne; k++) {
                            if (!lp_highway_at(hw, dst_lst, edges[k].to)) continue;
                            lp_id tsk = slot_key_id(dst_list_idx, edges[k].to);
                            int64 tord = lp_idmap_find(slot_map, &tsk);
                            if (tord < 0) continue;
                            size_t tn = s_end[tord] - s_begin[tord];
                            for (size_t j = 0; j < tn; j++) lp_push(&scratch, s_flat[s_begin[tord] + j]);
                        }
                    }

                    size_t xw_n = dedup_size_t(scratch.v, scratch.n);
                    if (xw_n >= 2) xw2++;
                    if (xw_n >= 3) xw3++;
                    if (xw_n >= 4) xw4++;
                }
            }

            Datum v[10] = {
                CStringGetTextDatum(lname),
                Int64GetDatum(cached),
                Int64GetDatum(attested),
                Int64GetDatum(ind2),
                Int64GetDatum(ind3),
                Int64GetDatum(ind4),
                Int64GetDatum(xw2),
                Int64GetDatum(xw3),
                Int64GetDatum(xw4),
                Int64GetDatum(invalid_lineage)
            };
            bool nl[10] = { false };
            set_put(fcinfo, ts, td, v, nl);
            pfree(DatumGetPointer(v[0]));
        }

        lp_vec_free(&scratch);
        if (s_flat) pfree(s_flat);
        if (s_begin) pfree(s_begin);
        if (s_end) pfree(s_end);
    }
    PG_FINALLY();
    {
        lp_idmap_free(slot_map);
        lp_idmap_free(distinct_roots);
        lp_vec_free(&att_rows);
        lp_idmap_free(apctx.matched_claim_map);
        lp_vec_free(&apctx.hits);
        lp_vec_free(&chunk_claims);
        lp_idmap_free(wgraph.by_id);
        lp_vec_free(&wgraph.nodes);
    }
    PG_END_TRY();
    return (Datum)0;
}

typedef struct { size_t child, parent; } WitnessUpEdge;
typedef struct {
    lp_idmap *scope, *nodes, *front, *next, *seen_level;
    lp_vec(uint8_t) allowed;
    lp_vec(WitnessUpEdge) edges;
    lp_vec(lp_id) new_ids;
    lp_vec(size_t) new_nodes;
    lp_vec(lp_vertex) vertices;
    size_t max_nodes, max_edges;
} WitnessFilterCtx;

static void witness_filter_visit(const lp_id *parent, Datum path, Datum mask, void *context)
{
    WitnessFilterCtx *c = context; (void)mask;
    if (c->edges.n >= c->max_edges) return;
    bool level_fresh = false;
    lp_idmap_put(c->seen_level, parent, &level_fresh);
    if (!level_fresh) return;
    Geo g = geo_of(path);
    int nv = (int)g.p.n;
    if (nv <= 0) return;
    lp_vec_reserve(&c->vertices, (size_t)nv);
    if (lp_path_decode(g.p, c->vertices.v, (size_t)nv) != (size_t)nv) return;
    size_t p_idx = SIZE_MAX;
    for (int k = 0; k < nv; k++) {
        const lp_vertex *v = &c->vertices.v[k];
        if (v->said == LP_SAID_METADATA || v->said == LP_SAID_DEPENDENCE) continue;
        int64_t f_ord = lp_idmap_find(c->front, &v->id);
        if (f_ord < 0) continue;
        int64_t ch_ord = lp_idmap_find(c->nodes, &v->id);
        if (ch_ord < 0) continue;
        if (p_idx == SIZE_MAX) {
            bool p_fresh = false;
            int64_t existing = lp_idmap_find(c->nodes, parent);
            if (existing >= 0) {
                p_idx = (size_t)existing;
            } else {
                if (lp_idmap_count(c->nodes) >= c->max_nodes) return;
                p_idx = lp_idmap_put(c->nodes, parent, &p_fresh);
                uint8_t hit = (lp_idmap_find(c->scope, parent) >= 0) ? 1 : 0;
                lp_push(&c->allowed, hit);
                if (!hit && p_fresh) {
                    lp_idmap_put(c->next, parent, NULL);
                    lp_push(&c->new_ids, *parent);
                    lp_push(&c->new_nodes, p_idx);
                }
            }
        }
        if (c->edges.n < c->max_edges) {
            WitnessUpEdge e = { (size_t)ch_ord, p_idx };
            lp_push(&c->edges, e);
        }
    }
}

typedef struct {
    lp_idmap *closure;
    lp_idmap *next_front;
    lp_vec(lp_vertex) vertices;
    size_t limit;
    bool complete;
} DownwardWitnessCtx;

static void downward_closure_visit(const lp_id *entity, Datum path, Datum mask, void *context)
{
    DownwardWitnessCtx *c = context;
    (void)entity; (void)mask;
    if (!c->complete) return;
    Geo g = geo_of(path);
    size_t nv = g.p.n;
    if (!nv) return;
    lp_vec_reserve(&c->vertices, nv);
    if (lp_path_decode(g.p, c->vertices.v, nv) != nv) return;
    for (size_t k = 0; k < nv; k++) {
        const lp_vertex *v = &c->vertices.v[k];
        if (v->said == LP_SAID_METADATA || v->said == LP_SAID_DEPENDENCE) continue;
        bool fresh = false;
        lp_idmap_put(c->closure, &v->id, &fresh);
        if (fresh) {
            if (lp_idmap_count(c->closure) > c->limit) {
                c->complete = false;
                return;
            }
            lp_idmap_put(c->next_front, &v->id, NULL);
        }
    }
}

static void try_downward_witness_closure(const lp_id *witnesses, int nwitnesses,
                                         lp_idmap *closure, bool *complete)
{
    *complete = false;
    if (!witnesses || nwitnesses <= 0 || !closure) return;

    Oid target = lp_pg_relation("attestation");
    const char *selected[] = { "claim", "witness" };
    const char *column = NULL;
    Relation parent = lp_pg_read_parent(target, &column, selected, 2);
    Oid native_id = get_atttype(target, get_attnum(target, "witness"));
    table_close(parent, NoLock);

    const Oid *parts = NULL, *aux = NULL;
    int nparts = lp_pg_canonical_partitions("attestation", &parts, NULL, &aux, NULL);
    Snapshot snap = GetActiveSnapshot();

    lp_idmap *front = lp_idmap_new();
    lp_vec(lp_id) uniq_w = { 0 };
    bool ok = true;

    PG_TRY(); {
        for (int i = 0; i < nwitnesses; i++) {
            bool fresh = false;
            lp_idmap_put(closure, witnesses + i, &fresh);
            if (fresh) {
                lp_idmap_put(front, witnesses + i, NULL);
                lp_push(&uniq_w, witnesses[i]);
            }
        }

        for (int p = 0; p < nparts && ok; p++) {
            CHECK_FOR_INTERRUPTS();
            if (!OidIsValid(aux[p])) { ok = false; break; }
            Relation part = table_open(parts[p], AccessShareLock);
            Relation idx = index_open(aux[p], AccessShareLock);
            AttrNumber ac = get_attnum(parts[p], "claim");
            Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
            if (!OidIsValid(eq_op)) {
                index_close(idx, NoLock);
                table_close(part, NoLock);
                ok = false;
                break;
            }
            RegProcedure eq_proc = get_opcode(eq_op);
            IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 1, 0);
            TupleTableSlot *slot = table_slot_create(part, NULL);

            for (size_t wi = 0; wi < uniq_w.n && ok; wi++) {
                ScanKeyData sk;
                ScanKeyEntryInitialize(&sk, 0, 1, BTEqualStrategyNumber, native_id,
                                       idx->rd_indcollation[0], eq_proc, PointerGetDatum(&uniq_w.v[wi]));
                index_rescan(scan, &sk, 1, NULL, 0);
                while (ok && index_getnext_slot(scan, ForwardScanDirection, slot)) {
                    CHECK_FOR_INTERRUPTS();
                    bool isnull = true;
                    Datum dc = slot_getattr(slot, ac, &isnull);
                    if (!isnull) {
                        const lp_id *cid = (const lp_id *)DatumGetPointer(dc);
                        bool fresh = false;
                        lp_idmap_put(closure, cid, &fresh);
                        if (fresh) {
                            lp_idmap_put(front, cid, NULL);
                            if (lp_idmap_count(closure) > 512) ok = false;
                        }
                    }
                    ExecClearTuple(slot);
                }
            }

            ExecDropSingleTupleTableSlot(slot);
            index_endscan(scan);
            index_close(idx, NoLock);
            table_close(part, NoLock);
        }

        DownwardWitnessCtx dctx = {
            .closure = closure,
            .next_front = NULL,
            .limit = 2048,
            .complete = ok
        };
        for (int depth = 0; depth < 16 && lp_idmap_count(front) > 0 && dctx.complete; depth++) {
            CHECK_FOR_INTERRUPTS();
            dctx.next_front = lp_idmap_new();
            lp_pg_lookup_paths(lp_idmap_keys(front), lp_idmap_count(front), false, downward_closure_visit, &dctx);
            lp_idmap_free(front);
            front = dctx.next_front;
            dctx.next_front = NULL;
        }
        lp_vec_free(&dctx.vertices);
        ok = dctx.complete;
    } PG_FINALLY(); {
        lp_vec_free(&uniq_w);
        lp_idmap_free(front);
    } PG_END_TRY();

    *complete = ok;
}

void lp_pg_filter_witness_entities(const lp_id *entities, size_t count,
                                   const lp_id *witnesses, int nwitnesses, bool *allowed)
{
    if (!entities || !allowed || !count) return;
    memset(allowed, 0, count * sizeof *allowed);
    if (!witnesses || nwitnesses <= 0) return;

    MemoryContext caller = CurrentMemoryContext;
    MemoryContext context = AllocSetContextCreate(caller, "Native witness entity filter", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(context);

    size_t *orig_node = palloc(count * sizeof *orig_node);
    WitnessFilterCtx wf = {
        .scope = lp_idmap_new(),
        .nodes = lp_idmap_new(),
        .front = lp_idmap_new(),
        .max_nodes = 4096,
        .max_edges = 8192
    };
    lp_idmap *down = lp_idmap_new();
    lp_vec(lp_id) check_att = {0};
    PG_TRY(); {
        for (int i = 0; i < nwitnesses; i++) lp_idmap_put(wf.scope, witnesses + i, NULL);
        for (size_t i = 0; i < count; i++) {
            bool fresh = false;
            size_t idx = lp_idmap_put(wf.nodes, entities + i, &fresh);
            orig_node[i] = idx;
            if (fresh) {
                uint8_t direct = (lp_idmap_find(wf.scope, entities + i) >= 0) ? 1 : 0;
                lp_push(&wf.allowed, direct);
                if (!direct) lp_push(&check_att, entities[i]);
            }
        }
        if (check_att.n > 0) {
            PgAttrRows att = {0};
            lp_pg_read_attestation_pk(check_att.v, (int)check_att.n, wf.scope, &att, NULL);
            for (size_t k = 0; k < att.n; k++) {
                int64_t idx = lp_idmap_find(wf.nodes, &att.v[k].claim);
                if (idx >= 0) wf.allowed.v[idx] = 1;
            }
            lp_vec_free(&att);
        }

        size_t unresolved = 0;
        for (size_t k = 0; k < check_att.n; k++) {
            int64_t idx = lp_idmap_find(wf.nodes, &check_att.v[k]);
            if (idx >= 0 && !wf.allowed.v[idx]) unresolved++;
        }

        if (unresolved > 0) {
            bool down_complete = false;
            if (nwitnesses <= 4) {
                try_downward_witness_closure(witnesses, nwitnesses, down, &down_complete);
                for (size_t k = 0; k < check_att.n; k++) {
                    int64_t idx = lp_idmap_find(wf.nodes, &check_att.v[k]);
                    if (idx >= 0 && !wf.allowed.v[idx] && lp_idmap_find(down, &check_att.v[k]) >= 0) {
                        wf.allowed.v[idx] = 1;
                        unresolved--;
                    }
                }
            }
            if (!(nwitnesses <= 4 && down_complete) && unresolved > 0) {
                for (size_t k = 0; k < check_att.n; k++) {
                    int64_t idx = lp_idmap_find(wf.nodes, &check_att.v[k]);
                    if (idx >= 0 && !wf.allowed.v[idx]) lp_idmap_put(wf.front, &check_att.v[k], NULL);
                }
                for (int lvl = 0; lvl < 4 && lp_idmap_count(wf.front) > 0
                     && lp_idmap_count(wf.nodes) < wf.max_nodes && wf.edges.n < wf.max_edges; lvl++) {
                    CHECK_FOR_INTERRUPTS();
                    wf.next = lp_idmap_new();
                    wf.seen_level = lp_idmap_new();
                    wf.new_ids.n = 0;
                    wf.new_nodes.n = 0;
                    lp_pg_lookup_containers(lp_idmap_keys(wf.front), lp_idmap_count(wf.front),
                                            false, false, witness_filter_visit, &wf);
                    if (wf.new_ids.n > 0) {
                        PgAttrRows att = {0};
                        lp_pg_read_attestation_pk(wf.new_ids.v, (int)wf.new_ids.n, wf.scope, &att, NULL);
                        for (size_t k = 0; k < att.n; k++) {
                            int64_t idx = lp_idmap_find(wf.nodes, &att.v[k].claim);
                            if (idx >= 0) wf.allowed.v[idx] = 1;
                        }
                        lp_vec_free(&att);
                    }
                    lp_idmap_free(wf.seen_level); wf.seen_level = NULL;
                    lp_idmap_free(wf.front);
                    /* Only continue climbing from nodes not yet proved allowed. */
                    lp_idmap *filtered_next = lp_idmap_new();
                    for (size_t k = 0; k < lp_idmap_count(wf.next); k++) {
                        const lp_id *nid = lp_idmap_key(wf.next, k);
                        int64_t nidx = lp_idmap_find(wf.nodes, nid);
                        if (nidx >= 0 && !wf.allowed.v[nidx]) lp_idmap_put(filtered_next, nid, NULL);
                    }
                    lp_idmap_free(wf.next); wf.next = NULL;
                    wf.front = filtered_next;
                }
                for (int pass = 0; pass < 4; pass++) {
                    bool changed = false;
                    for (size_t e = wf.edges.n; e-- > 0;) {
                        WitnessUpEdge edge = wf.edges.v[e];
                        if (wf.allowed.v[edge.parent] && !wf.allowed.v[edge.child]) {
                            wf.allowed.v[edge.child] = 1;
                            changed = true;
                        }
                    }
                    if (!changed) break;
                }
            }
        }
        for (size_t i = 0; i < count; i++) allowed[i] = wf.allowed.v[orig_node[i]] != 0;
    } PG_FINALLY(); {
        lp_idmap_free(down);
        lp_idmap_free(wf.scope);
        lp_idmap_free(wf.nodes);
        lp_idmap_free(wf.front);
        lp_idmap_free(wf.next);
        lp_idmap_free(wf.seen_level);
        lp_vec_free(&wf.allowed);
        lp_vec_free(&wf.edges);
        lp_vec_free(&wf.new_ids);
        lp_vec_free(&wf.new_nodes);
        lp_vec_free(&wf.vertices);
        lp_vec_free(&check_att);
        MemoryContextSwitchTo(caller);
        MemoryContextDelete(context);
    } PG_END_TRY();
}
