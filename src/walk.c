/* One downward set traversal for consumers of registered canonical structure,
 * and zero-SPI graph walks (laplace_pull, laplace_chains, laplace_above). */
#include "pg_core.h"
#include "geo.h"
#include "catalog/pg_type.h"

typedef struct { lp_id *v; size_t n, cap; } WalkIds;
typedef struct {
    lp_idmap *seen;
    WalkIds frontier, next;
    lp_pg_stored_path_visit visit;
    void *context;
} WitnessWalk;

static void walk_remember(WitnessWalk *walk, WalkIds *frontier, const lp_id *id)
{
    bool fresh;
    lp_idmap_put(walk->seen, id, &fresh);
    if (fresh) lp_push(frontier, *id);
}
static void walk_root_visit(const lp_id *id, const lp_id *lineage, double trust, void *context)
{
    (void)lineage; (void)trust;
    WitnessWalk *walk = context;
    walk_remember(walk, &walk->frontier, id);
}
static void walk_path(const lp_id *id, Datum geometry, Datum mask, void *context)
{
    WitnessWalk *walk = context; Geo path = geo_of(geometry);
    walk->visit(id, path.p, mask, walk->context);
    for (size_t i = 0; i < path.p.n; i++) {
        lp_id child = lp_path_id(path.p, i);
        walk_remember(walk, &walk->next, &child);
    }
}
void lp_pg_walk_witnesses(ArrayType *roots, lp_pg_stored_path_visit visit, void *context)
{
    if (roots && ARR_NDIM(roots) > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("witness roots must be a one-dimensional identity array")));
    WitnessWalk walk = {.seen = lp_idmap_new(), .visit = visit, .context = context};
    if (!roots) {
        lp_pg_scan_all_witnesses(walk_root_visit, &walk);
    } else {
        int nr; lp_id *rids = ids_of(roots, &nr);
        if (nr > 0) lp_pg_lookup_witnesses(rids, (size_t)nr, false, walk_root_visit, &walk);
        pfree(rids);
    }
    while (walk.frontier.n) {
        CHECK_FOR_INTERRUPTS();
        lp_pg_lookup_paths(walk.frontier.v, walk.frontier.n, false, walk_path, &walk);
        WalkIds consumed = walk.frontier;
        walk.frontier = walk.next; walk.next = consumed; walk.next.n = 0;
    }
    lp_vec_free(&walk.frontier); lp_vec_free(&walk.next); lp_idmap_free(walk.seen);
}

/* ---------------------------------------------------------------- the claim reader the walks share
 * One Direct-AM claim reader: every scored claim of three constituents holding a frontier entity at either end, its
 * middle qualified (NULL unconstrained), at most fan an occurrence (-1 every one). */
typedef struct { int64 i; lp_id claim, part[3]; lp_rating r; int64 matches; } WalkClaim;
typedef struct {
    lp_vec(WalkClaim) rows;
    int64 seen;
    int64 cap;
    bool check_cap;
} WalkClaimCollect;
static void walk_claim_visit(const PgClaimHit *hit, void *context){
    WalkClaimCollect *c = context;
    if (!hit->path) return;
    if (c->check_cap && ++c->seen > c->cap)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_chains: paths exceed the admitted frontier; no partial path was returned")));
    Geo g = geo_of(hit->path); lp_id part[4];
    if (lp_path_len(g.p) != 3 || lp_path_expand(g.p, part, 4) != 3) return;
    WalkClaim *wc = lp_vec_add(&c->rows);
    wc->i = hit->i;
    wc->claim = hit->entity;
    memcpy(wc->part, part, sizeof wc->part);
    wc->r = (lp_rating){ hit->rating, hit->deviation, hit->volatility };
    wc->matches = hit->matches;
}

/* ---------------------------------------------------------------- PULL: best-first over rated claims
 * laplace_pull(seeds, goals, hops, fan, nodes, middle, refuse, k, per_hop): the law of the Engine's pull (Native
 * lp_frontier, crossing cost lp_cost): every seed opens at cost 0; the open entities of least cost close a batch at a
 * time, and each closed short of hops is read through its claims, at most fan an entity. A claim crosses from one end
 * to the other; an entity held as the middle names the tie and is not crossed. An entity holding more than fan claims
 * is a hub, reached and not crossed; a seed is crossed through its first fan claims (the lowest claim IDs). It stops
 * when nothing is open, nodes entities are closed, or every goal is. One row a closed entity, in close order, with its
 * chain from its seed. */
typedef struct { lp_id seed, relation; double rating, deviation; bool hub; } PullInfo;
static int by_claim(const void *a, const void *b){
    const WalkClaim *x = a, *y = b; if (x->i != y->i) return x->i < y->i ? -1 : 1; return memcmp(x->claim.b, y->claim.b, 16);
}
#define PULL_BATCH 64
PG_FUNCTION_INFO_V1(laplace_pull);
Datum laplace_pull(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_pull", &td);
    for (int a = 2; a <= 4; a++) if (PG_ARGISNULL(a)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_pull: hops, fan and nodes must not be NULL")));
    if (PG_ARGISNULL(7) || PG_ARGISNULL(8)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_pull: k and per_hop must not be NULL")));
    int32 maxhops = PG_GETARG_INT32(2); int64 fan = PG_GETARG_INT64(3), nodes = PG_GETARG_INT64(4);
    double k = PG_GETARG_FLOAT8(7), per_hop = PG_GETARG_FLOAT8(8);
    if (maxhops < 0 || fan < 0 || nodes < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_pull: hops, fan and nodes must not be negative")));
    if (fan == PG_INT64_MAX) fan--;
    if (PG_ARGISNULL(0) || nodes == 0) PG_RETURN_NULL();
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0), el = get_element_type(idarr);
    int ns; lp_id *seeds = ids_of(PG_GETARG_ARRAYTYPE_P(0), &ns); if (!ns) { pfree(seeds); PG_RETURN_NULL(); }
    int ng = 0; lp_id *goals = PG_ARGISNULL(1) ? NULL : ids_of(PG_GETARG_ARRAYTYPE_P(1), &ng);
    bool no_middle = PG_ARGISNULL(5);
    int nmid = 0; lp_id *mids = no_middle ? NULL : ids_of(PG_GETARG_ARRAYTYPE_P(5), &nmid);
    int nref = 0; lp_id *refuse = PG_ARGISNULL(6) ? NULL : ids_of(PG_GETARG_ARRAYTYPE_P(6), &nref);
    lp_frontier *f = lp_frontier_new();
    lp_idmap *info = lp_idmap_sized(sizeof(PullInfo)), *goal = lp_idmap_new(), *seedset = lp_idmap_new();
    PG_TRY(); {
        for (int i = 0; i < ng; i++) lp_idmap_put(goal, &goals[i], NULL);
        size_t goals_left = lp_idmap_count(goal);
        for (int i = 0; i < ns; i++) {
            bool fresh; lp_idmap_put(seedset, &seeds[i], &fresh); if (!fresh) continue;
            lp_frontier_reach(f, &seeds[i], NULL, NULL, 0, 0, 0);
            PullInfo *pi = lp_idmap_get(info, &seeds[i], NULL); pi->seed = seeds[i];
        }
        lp_id *closed = palloc(sizeof(lp_id) * 64); int64 nclosed = 0, capclosed = 64; bool done = false;
        while (!done && nclosed < nodes) {
            CHECK_FOR_INTERRUPTS();
            lp_reached batch[PULL_BATCH]; int nb = 0; const lp_reached *x;
            while (nb < PULL_BATCH && nclosed < nodes && (x = lp_frontier_next(f))) {
                batch[nb++] = *x;
                if (nclosed == capclosed) { capclosed *= 2; closed = repalloc_huge(closed, sizeof(lp_id) * (Size)capclosed); }
                closed[nclosed++] = x->id;
                if (goals_left && lp_idmap_find(goal, &x->id) >= 0 && --goals_left == 0) { done = true; break; }
            }
            if (!nb || done) break;
            lp_id ids[PULL_BATCH]; int who[PULL_BATCH], m = 0;
            for (int i = 0; i < nb; i++) if ((int64)batch[i].hops < maxhops) { ids[m] = batch[i].id; who[m++] = i; }
            if (!m) continue;
            WalkClaimCollect col = { .rows = { 0 }, .seen = 0, .cap = 0, .check_cap = false };
            lp_pg_claims_each_direct(ids, m, fan + 1, UINT64_C(1),
                                     refuse, nref, 3,
                                     no_middle ? NULL : mids, nmid, 3, NULL, 0,
                                     walk_claim_visit, &col);
            size_t nc = col.rows.n; WalkClaim *cl = col.rows.v;
            lp_sort(cl, nc, sizeof(WalkClaim), by_claim);                         /* each entity's claims in ID order */
            int64 held[PULL_BATCH] = { 0 }, used[PULL_BATCH] = { 0 };
            for (size_t j = 0; j < nc; j++) if (cl[j].i >= 1 && cl[j].i <= m) held[cl[j].i - 1]++;
            for (size_t j = 0; j < nc; j++) {
                int e = (int)cl[j].i - 1; if (e < 0 || e >= m) continue;
                const lp_reached *at = &batch[who[e]];
                if (held[e] > fan) {
                    bool seed = at->hops == 0 && lp_idmap_find(seedset, &at->id) >= 0;
                    if (!seed) { PullInfo *ai = lp_idmap_lookup(info, &at->id); if (ai) ai->hub = true; continue; }
                    if (used[e] >= fan) continue;
                }
                used[e]++;
                int other = lp_tuple_other(cl[j].part, 3, &at->id); if (other < 0) continue;
                double cost = at->cost + lp_cost(&cl[j].r, k, per_hop);
                if (lp_frontier_reach(f, &cl[j].part[other], &at->id, &cl[j].claim, cost, 0, at->hops + 1)) {
                    lp_id seed = ((PullInfo *)lp_idmap_lookup(info, &at->id))->seed;
                    PullInfo *oi = lp_idmap_get(info, &cl[j].part[other], NULL);
                    oi->seed = seed; oi->relation = cl[j].part[1]; oi->rating = cl[j].r.rating; oi->deviation = cl[j].r.deviation;
                }
            }
            lp_vec_free(&col.rows);
        }
        for (int64 c = 0; c < nclosed; c++) {
            CHECK_FOR_INTERRUPTS();
            const lp_reached *r = lp_frontier_find(f, &closed[c]); const PullInfo *pi = lp_idmap_lookup(info, &closed[c]);
            uint32 h = r->hops; lp_id *chain = palloc(sizeof(lp_id) * (h + 1)), *claims = palloc(sizeof(lp_id) * (h ? h : 1));
            const lp_reached *w = r;
            for (int64 s = h; s >= 0 && w; s--) { chain[s] = w->id; if (s > 0) { claims[s - 1] = w->claim; w = lp_frontier_find(f, &w->from); } }
            Datum o[14]; bool nl[14] = { 0 };
            o[0] = Int64GetDatum(c + 1); o[1] = id_datum(&r->id); o[2] = id_datum(&pi->seed);
            if (h) { o[3] = id_datum(&r->from); o[4] = id_datum(&r->claim); o[5] = id_datum(&pi->relation); o[6] = Float8GetDatum(pi->rating); o[7] = Float8GetDatum(pi->deviation); }
            else for (int z = 3; z <= 7; z++) { nl[z] = true; o[z] = 0; }
            o[8] = Float8GetDatum(r->cost); o[9] = Int32GetDatum((int32)h); o[10] = BoolGetDatum(pi->hub);
            o[11] = BoolGetDatum(lp_idmap_find(goal, &r->id) >= 0);
            o[12] = PointerGetDatum(id_array_of(el, chain, (int)h + 1)); o[13] = PointerGetDatum(id_array_of(el, claims, (int)h));
            set_put(fcinfo, ts, td, o, nl); pfree(chain); pfree(claims);
        }
    } PG_FINALLY(); { lp_frontier_free(f); lp_idmap_free(info); lp_idmap_free(goal); lp_idmap_free(seedset); } PG_END_TRY();
    if (refuse) pfree(refuse);
    if (mids) pfree(mids);
    if (goals) pfree(goals);
    pfree(seeds);
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- CHAINS: declared paths
 * laplace_chains(roots, steps, programs, depths, inverse, cap): every root walks every declared program, one claim a
 * depth: step i allows middle steps[i] at depth depths[i] of program programs[i], read from the subject (the path
 * starts with the node) or, inverse, from the object. One set read a depth for the whole frontier; only complete paths
 * are returned, a row a step. A frontier or a depth's reading past cap is an error and returns nothing partial. */
typedef struct { lp_id id, claim, relation; int32 root, program, depth; int previous; double rating, deviation; int64 matches; } ChainNode;
typedef struct { ChainNode *v; int n, cap; } ChainNodes;
static int chain_add(ChainNodes *nodes, const ChainNode *node){
    if (nodes->n == nodes->cap) {
        if (nodes->cap >= INT_MAX / 2 || (Size)nodes->cap * 2 > MaxAllocHugeSize / sizeof(ChainNode)) ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_chains: paths exceed memory capacity")));
        nodes->cap = nodes->cap ? nodes->cap * 2 : 64;
        nodes->v = nodes->v ? repalloc_huge(nodes->v, sizeof(ChainNode) * (Size)nodes->cap) : palloc_extended(sizeof(ChainNode) * (Size)nodes->cap, MCXT_ALLOC_HUGE);
    }
    nodes->v[nodes->n] = *node; return nodes->n++;
}
static void *chain_array(ArrayType *a, Oid el, int16 len, bool byval, char align, int *n, const char *what){
    Datum *d; bool *nulls;
    if (ARR_NDIM(a) > 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_chains: %s must be a one-dimensional array", what)));
    deconstruct_array(a, el, len, byval, align, &d, &nulls, n);
    for (int i = 0; i < *n; i++) if (nulls[i]) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_chains: %s cannot contain NULL", what)));
    return d;
}
PG_FUNCTION_INFO_V1(laplace_chains);
Datum laplace_chains(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_chains", &td);
    for (int a = 0; a <= 5; a++) if (PG_ARGISNULL(a)) PG_RETURN_NULL();
    int64 cap = PG_GETARG_INT64(5);
    if (cap < 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_chains: cap must be positive")));
    int nr, nsteps, np, nd, ni;
    lp_id *roots = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nr), *steps = ids_of(PG_GETARG_ARRAYTYPE_P(1), &nsteps);
    Datum *programs = chain_array(PG_GETARG_ARRAYTYPE_P(2), INT4OID, 4, true, TYPALIGN_INT, &np, "programs");
    Datum *depths = chain_array(PG_GETARG_ARRAYTYPE_P(3), INT4OID, 4, true, TYPALIGN_INT, &nd, "depths");
    Datum *inverse = chain_array(PG_GETARG_ARRAYTYPE_P(4), BOOLOID, 1, true, TYPALIGN_CHAR, &ni, "inverse");
    if (np != nsteps || nd != nsteps || ni != nsteps) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_chains: steps, programs, depths and inverse must have equal lengths")));
    int nprog = 0;
    for (int s = 0; s < nsteps; s++) {
        int32 p = DatumGetInt32(programs[s]), d = DatumGetInt32(depths[s]);
        if (p < 1 || d < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_chains: programs are 1-based and depths 0-based")));
        if (p > nprog) nprog = p;
    }
    int32 *length = palloc0(sizeof(int32) * (size_t)(nprog + 1));
    for (int s = 0; s < nsteps; s++) { int32 p = DatumGetInt32(programs[s]), d = DatumGetInt32(depths[s]); if (d + 1 > length[p]) length[p] = d + 1; }
    ChainNodes nodes = { 0 }; int *frontier = NULL, nf = 0, *next = NULL; int64 path = 0;
    for (int r = 0; r < nr; r++) for (int p = 1; p <= nprog; p++) if (length[p]) {
        if ((int64)nf + 1 > cap) ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_chains: roots exceed the admitted frontier")));
        ChainNode node = { .id = roots[r], .root = r + 1, .program = p, .depth = 0, .previous = -1 };
        int at = chain_add(&nodes, &node);
        frontier = frontier ? repalloc(frontier, sizeof(int) * (size_t)(nf + 1)) : palloc(sizeof(int)); frontier[nf++] = at;
    }
    for (int32 depth = 0; nf > 0; depth++) {
        CHECK_FOR_INTERRUPTS();
        lp_id *ids = palloc(sizeof(lp_id) * (size_t)nf), *mids = palloc(sizeof(lp_id) * (size_t)(nsteps ? nsteps : 1)); int nm = 0;
        for (int i = 0; i < nf; i++) ids[i] = nodes.v[frontier[i]].id;
        for (int s = 0; s < nsteps; s++) if (DatumGetInt32(depths[s]) == depth) mids[nm++] = steps[s];
        int nn = 0; next = palloc(sizeof(int) * (size_t)Max(nf, 1)); int capnext = Max(nf, 1);
        if (nm > 0) {
            WalkClaimCollect col = { .rows = { 0 }, .seen = 0, .cap = cap, .check_cap = true };
            lp_pg_claims_each_direct(ids, nf, -1, UINT64_C(1),
                                     NULL, 0, 3,
                                     mids, nm, 3, NULL, 0,
                                     walk_claim_visit, &col);
            size_t nc = col.rows.n; const WalkClaim *cl = col.rows.v;
            for (size_t j = 0; j < nc; j++) {
                int e = (int)cl[j].i - 1; if (e < 0 || e >= nf) continue;
                int from = frontier[e]; ChainNode node = nodes.v[from];
                int other = -1;
                for (int s = 0; s < nsteps && other < 0; s++) {
                    if (DatumGetInt32(programs[s]) != node.program || DatumGetInt32(depths[s]) != depth || !lp_id_eq(&steps[s], &cl[j].part[1])) continue;
                    if (!DatumGetBool(inverse[s]) && lp_id_eq(&cl[j].part[0], &node.id)) other = 2;
                    else if (DatumGetBool(inverse[s]) && lp_id_eq(&cl[j].part[2], &node.id)) other = 0;
                }
                if (other < 0) continue;
                ChainNode step = { .id = cl[j].part[other], .claim = cl[j].claim, .relation = cl[j].part[1], .root = node.root,
                                   .program = node.program, .depth = depth + 1, .previous = from,
                                   .rating = cl[j].r.rating, .deviation = cl[j].r.deviation, .matches = cl[j].matches };
                int at = chain_add(&nodes, &step);
                if (step.depth < length[step.program]) {
                    if ((int64)nn + 1 > cap) ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_chains: paths exceed the admitted frontier; no partial path was returned")));
                    if (nn == capnext) { capnext *= 2; next = repalloc(next, sizeof(int) * (size_t)capnext); }
                    next[nn++] = at;
                    continue;
                }
                /* complete: a row a step, depth 0 the first */
                int32 n = step.depth; int *chain = palloc(sizeof(int) * (size_t)n), w = at;
                for (int32 z = n - 1; z >= 0; z--) { chain[z] = w; w = nodes.v[w].previous; }
                for (int32 z = 0; z < n; z++) {
                    const ChainNode *c = &nodes.v[chain[z]];
                    Datum o[10] = { Int32GetDatum(c->root), Int32GetDatum(c->program), Int64GetDatum(path), Int32GetDatum(z),
                                    id_datum(&c->claim), id_datum(&c->id), id_datum(&c->relation),
                                    Float8GetDatum(c->rating), Float8GetDatum(c->deviation), Int64GetDatum(c->matches) };
                    bool nl[10] = { 0 }; set_put(fcinfo, ts, td, o, nl);
                }
                path++; pfree(chain);
            }
            lp_vec_free(&col.rows);
        }
        pfree(ids); pfree(mids); pfree(frontier); frontier = next; nf = nn;
    }
    PG_RETURN_NULL();
}


/* ---------------------------------------------------------------- ABOVE: upward containment closure
 * laplace_above(ids, hops, cap): hop 1 every direct container of the IDs (the container index, no mask), each later hop
 * the direct containers of the hop before's new entities; every entity once, at the first hop it is met; the inputs
 * are not their own containers. One container read a hop, one tier read a call. At most cap rows, in hop order then
 * ID order (the container index visits parents in ascending ID order). */
typedef struct { lp_idmap *seen; lp_id *found; int32 *hop; int64 n, room, cap; int32 at; } AboveWalk;
static void above_row(const lp_id *id, Datum path, Datum mask, void *context){
    (void)path; (void)mask; AboveWalk *w = context; bool fresh;
    if (w->n >= w->cap) return;
    lp_idmap_put(w->seen, id, &fresh); if (!fresh) return;
    if (w->n == w->room) {
        w->room = Min(w->cap, w->room * 2);
        w->found = repalloc_huge(w->found, sizeof(lp_id) * (Size)w->room); w->hop = repalloc_huge(w->hop, sizeof(int32) * (Size)w->room);
    }
    w->found[w->n] = *id; w->hop[w->n] = w->at; w->n++;
}
PG_FUNCTION_INFO_V1(laplace_above);
Datum laplace_above(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_above", &td);
    int32 hops = PG_GETARG_INT32(1); int64 cap = PG_GETARG_INT64(2);
    if (hops < 0 || cap < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_above: hops and cap must not be negative")));
    int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (!n || !hops || !cap) PG_RETURN_NULL();
    cap = Min(cap, (int64)(MaxAllocHugeSize / sizeof(lp_id)));
    AboveWalk w = { .seen = lp_idmap_new(), .cap = cap, .room = Min(cap, (int64)1024) };
    PG_TRY(); {
        w.found = palloc_extended(sizeof(lp_id) * (Size)w.room, MCXT_ALLOC_HUGE); w.hop = palloc_extended(sizeof(int32) * (Size)w.room, MCXT_ALLOC_HUGE);
        lp_id *start = palloc(sizeof(lp_id) * (size_t)n); size_t nstart = 0;
        for (int i = 0; i < n; i++) { bool fresh; lp_idmap_put(w.seen, &ids[i], &fresh); if (fresh) start[nstart++] = ids[i]; }
        lp_id *frontier = start; size_t nfront = nstart;
        for (int32 h = 1; h <= hops && nfront > 0 && w.n < cap; h++) {
            CHECK_FOR_INTERRUPTS();
            int64 before = w.n; w.at = h;
            lp_pg_lookup_containers(frontier, nfront, false, false, above_row, &w);
            /* the next frontier is this hop's new entities, copied: the result arrays may move as they grow */
            nfront = (size_t)(w.n - before);
            frontier = palloc(sizeof(lp_id) * (nfront ? nfront : 1)); memcpy(frontier, w.found + before, sizeof(lp_id) * nfront);
        }
        int16 *tiers = palloc(sizeof(int16) * (Size)(w.n ? w.n : 1));
        if (w.n) lp_pg_lookup_entity_tiers(w.found, (size_t)w.n, tiers);
        for (int64 i = 0; i < w.n; i++) {
            Datum o[3] = { id_datum(&w.found[i]), Int16GetDatum(tiers[i]), Int32GetDatum(w.hop[i]) }; bool nl[3] = { false, tiers[i] < 0, false };
            set_put(fcinfo, ts, td, o, nl);
        }
    } PG_FINALLY(); { lp_idmap_free(w.seen); } PG_END_TRY();
    PG_RETURN_NULL();
}
