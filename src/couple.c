/* Forward sequence pass, gap fills, and 4D geometric/trajectory coupling
 * (laplace_fills, laplace_forward, laplace_couple, laplace_curves). */
#include "pg_core.h"
#include "geo.h"
#include "catalog/pg_type.h"
#include "laplace/consensus.h"
#include <math.h>

/* ---------------------------------------------------------------- fills
 * laplace_fills(keys): every path above the lowest key that holds any of them, with the times each key is held by
 * that path (including runs). */
typedef struct {
    FunctionCallInfo fcinfo;
    Tuplestorestate *ts;
    TupleDesc td;
    lp_id *unique_ids;
    int n_unique;
    uint64_t *times;
} FillsVisit;
static void fills_hit(const PgHolderHit *hit, void *context){
    FillsVisit *v = context;
    if (hit->hub || !hit->path) return;
    Geo g = geo_of(hit->path);
    lp_path_times(g.p, v->unique_ids, (size_t)v->n_unique, v->times);
    for (int i = 0; i < v->n_unique; i++) {
        if (!v->times[i]) continue;
        Datum o[4] = { id_datum(&hit->entity), id_datum(&v->unique_ids[i]),
                       Int64GetDatum((int64)v->times[i]), Int16GetDatum(hit->tier) };
        bool nl[4] = { false, false, false, false };
        set_put(v->fcinfo, v->ts, v->td, o, nl);
    }
}
PG_FUNCTION_INFO_V1(laplace_fills);
Datum laplace_fills(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_fills", &td);
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    int n = 0; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (n <= 0) { pfree(ids); PG_RETURN_NULL(); }
    int n_unique = (int)lp_ids_unique(ids, (size_t)n);
    int16 *tiers = palloc(sizeof(int16) * (size_t)n_unique);
    lp_pg_lookup_entity_tiers(ids, (size_t)n_unique, tiers);
    int16 min_tier = INT16_MAX;
    for (int i = 0; i < n_unique; i++) if (tiers[i] >= 0 && tiers[i] < min_tier) min_tier = tiers[i];
    pfree(tiers);
    if (min_tier == INT16_MAX) { pfree(ids); PG_RETURN_NULL(); }
    FillsVisit ctx = { fcinfo, ts, td, ids, n_unique, palloc(sizeof(uint64_t) * (size_t)n_unique) };
    lp_pg_holders_direct(ids, n_unique, NULL, 0, false, min_tier, -1, 3, 0, 0, fills_hit, &ctx);
    pfree(ctx.times); pfree(ids);
    PG_RETURN_NULL();
}

/* laplace_forward(ids, fan): the forward pass over every contiguous segment of a prompt at once. For each segment
 * [i..j] of the prompt's constituents: the observations holding all of its parts (claims left out), how many hold it as
 * a run, and what follows the run in each, counted. A segment held by more than fan observations is a hub, reached and
 * not crossed: its row says paths fan + 1 (more than the fan), runs 0, and nothing follows it. Counting over the first
 * fan the index met would count a different set on another install of the same content. One row per continuation (next, times);
 * a segment held by nothing, or held where nothing follows the run, has a row with next null. Direct index access
 * fetches the paths that hold a segment through the GIN container index; the runs are matched and what follows them is
 * counted here, by Laplace-Native: precedes, contains and co-occurrence from the trajectories, no softmax, no window. */
/* A segment's observations: each path as it lies, and its tier. */
typedef struct { Datum path; int16 tier; } Held;
typedef lp_vec(Held) Helds;
typedef struct { Helds *w_held; bool *w_hub; int nw; MemoryContext wordctx; } ForwardBatchCtx;
static void forward_batch_hit(const PgHolderHit *hit, void *context){
    ForwardBatchCtx *c = context;
    int64 ord = hit->ordinal - 1;
    if (ord < 0 || ord >= c->nw) return;
    if (hit->hub) { c->w_hub[ord] = true; return; }
    if (!hit->path) return;
    MemoryContext old = MemoryContextSwitchTo(c->wordctx);
    Datum copied = PointerGetDatum(PG_DETOAST_DATUM_COPY(hit->path));
    lp_push(&c->w_held[ord], (Held){ copied, hit->tier });
    MemoryContextSwitchTo(old);
}
typedef struct { Helds *dst; bool is_hub; MemoryContext keepctx; } ForwardSegCtx;
static void forward_seg_hit(const PgHolderHit *hit, void *context){
    ForwardSegCtx *c = context;
    if (hit->hub) { c->is_hub = true; return; }
    if (!hit->path) return;
    MemoryContext old = MemoryContextSwitchTo(c->keepctx);
    Datum copied = PointerGetDatum(PG_DETOAST_DATUM_COPY(hit->path));
    lp_push(c->dst, (Held){ copied, hit->tier });
    MemoryContextSwitchTo(old);
}
typedef struct {
    int32 i, j;
    int64 paths, runs, times;
    lp_id next;
} ForwardSuffixRow;

static int cmp_forward_suffix(const void *a, const void *b){
    const ForwardSuffixRow *x = a, *y = b;
    int64 sx = (int64)(x->j - x->i), sy = (int64)(y->j - y->i);
    if (sx != sy) return sx > sy ? -1 : 1;
    if (x->times != y->times) return x->times > y->times ? -1 : 1;
    return memcmp(x->next.b, y->next.b, sizeof(lp_id));
}

static Datum run_laplace_forward(PG_FUNCTION_ARGS, bool suffix_only){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, suffix_only ? "laplace_forward_suffix" : "laplace_forward", &td);
    int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n); int64 fan = PG_GETARG_INT64(1); if (n > 256) n = 256;
    if (n < 2) { pfree(ids); PG_RETURN_NULL(); }
    int16 *tier = palloc(sizeof(int16) * (size_t)n);
    for (int i = 0; i < n; i++) tier[i] = -1;
    lp_pg_lookup_entity_tiers(ids, (size_t)n, tier);
    lp_idmap *followed = lp_idmap_sized(sizeof(int64)); lp_vec(lp_id) out = { 0 };
    MemoryContext keepctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward held", ALLOCSET_DEFAULT_SIZES),
                  wordctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward words", ALLOCSET_DEFAULT_SIZES);
    lp_id *w_ids = palloc(sizeof(lp_id) * (size_t)n);
    int *w_at = palloc(sizeof(int) * (size_t)n), nw = 0;
    for (int i = 0; i < n; i++) {
        w_at[i] = -1; if (tier[i] <= 0) continue;
        for (int k = 0; k < nw; k++) if (lp_id_eq(&w_ids[k], &ids[i])) { w_at[i] = k; break; }
        if (w_at[i] < 0) { w_at[i] = nw; w_ids[nw++] = ids[i]; }
    }
    Helds *w_held = palloc0(sizeof(Helds) * (size_t)(nw ? nw : 1));
    bool *w_hub = palloc0(sizeof(bool) * (size_t)(nw ? nw : 1));
    if (nw > 0) {
        ForwardBatchCtx bctx = { w_held, w_hub, nw, wordctx };
        lp_pg_holders_direct(w_ids, nw, NULL, 0, true, 0, fan, 2, 0, 0, forward_batch_hit, &bctx);
    }
    Helds held[2] = { { 0 }, { 0 } };
    if (suffix_only) {
        lp_vec(ForwardSuffixRow) srows = { 0 };
        int cur = 0, have = 0, j = n - 1, floor = tier[j], words = tier[j] > 0;
        for (int i = n - 2; i >= 0; i--) {
            words += tier[i] > 0; if (tier[i] > floor) floor = tier[i];
            int next = cur ^ 1, np = j - i + 1; held[next].n = 0;
            if (!words) { have = 0; continue; }
            if (!have) {
                int best_w = -1;
                for (int k = i; k <= j; k++) if (w_at[k] >= 0 && !w_hub[w_at[k]]) {
                    if (best_w < 0 || w_held[w_at[k]].n < w_held[best_w].n) best_w = w_at[k];
                }
                if (best_w >= 0) {
                    const Helds *src = &w_held[best_w];
                    for (size_t r = 0; r < src->n; r++) {
                        const Held *h = &src->v[r]; Geo g = geo_of(h->path);
                        if (h->tier > floor && lp_path_holds(g.p, ids + i, (size_t)np, true)) lp_push(&held[next], *h);
                    }
                    have = 1;
                } else {
                    int probe = i; for (int k = i; k <= j; k++) if (tier[k] == floor) { probe = k; break; }
                    ForwardSegCtx sctx = { &held[next], false, keepctx };
                    lp_pg_holders_direct(ids + probe, 1, ids + i, np, true, (int16)floor, fan, 2, 0, 0, forward_seg_hit, &sctx);
                    have = !sctx.is_hub;
                }
                if (!have) { cur = next; continue; }
            } else {
                for (size_t r = 0; r < held[cur].n; r++) {
                    const Held *h = &held[cur].v[r]; Geo g = geo_of(h->path);
                    if (h->tier > floor && lp_path_holds(g.p, ids + i, (size_t)np, true)) lp_push(&held[next], *h);
                }
            }
            if (held[next].n == 0) break;
            uint64 paths = held[next].n; int64 runs = 0;
            lp_idmap_clear(followed);
            for (size_t r = 0; r < held[next].n; r++) {
                Geo g = geo_of(held[next].v[r].path); size_t cap = g.p.n * 4 + 16; lp_vec_reserve(&out, cap);
                size_t found = lp_path_follows(g.p, ids + i, (size_t)np, out.v, cap);
                if (!found) continue;
                runs++; if (found > cap) found = cap;
                for (size_t x = 0; x < found; x++) (*(int64 *)lp_idmap_get(followed, &out.v[x], NULL))++;
            }
            for (size_t x = 0; x < lp_idmap_count(followed); x++) {
                ForwardSuffixRow sr = { i + 1, j + 1, (int64)paths, runs, *(int64 *)lp_idmap_at(followed, x), *lp_idmap_key(followed, x) };
                lp_push(&srows, sr);
            }
            cur = next;
        }
        lp_sort(srows.v, srows.n, sizeof(ForwardSuffixRow), cmp_forward_suffix);
        for (size_t k = 0; k < srows.n; k++) {
            Datum v[6] = {
                Int32GetDatum(srows.v[k].i), Int32GetDatum(srows.v[k].j),
                Int64GetDatum(srows.v[k].paths), Int64GetDatum(srows.v[k].runs),
                id_datum(&srows.v[k].next), Int64GetDatum(srows.v[k].times)
            };
            bool nl[6] = { false };
            set_put(fcinfo, ts, td, v, nl);
            pfree(DatumGetPointer(v[4]));
        }
        lp_vec_free(&srows);
    } else {
        for (int i = 0; i < n; i++) {
            MemoryContextReset(keepctx);
            int cur = 0, have = 0, floor = tier[i], words = tier[i] > 0;
            for (int j = i + 1; j < n; j++) {
                int prev_words = words, prev_floor = floor;
                words += tier[j] > 0; if (tier[j] > floor) floor = tier[j];
                int next = cur ^ 1, np = j - i + 1; held[next].n = 0;
                if (!words) { have = 0; continue; }
                if (!have) {
                    int best_w = -1;
                    for (int k = i; k <= j; k++) if (w_at[k] >= 0 && !w_hub[w_at[k]]) {
                        if (best_w < 0 || w_held[w_at[k]].n < w_held[best_w].n) best_w = w_at[k];
                    }
                    if (best_w >= 0) {
                        const Helds *src = &w_held[best_w];
                        for (size_t r = 0; r < src->n; r++) {
                            const Held *h = &src->v[r]; Geo g = geo_of(h->path);
                            if (h->tier > floor && lp_path_holds(g.p, ids + i, (size_t)np, true)) lp_push(&held[next], *h);
                        }
                        have = 1;
                    } else {
                        bool dup_part = false;
                        if (j > i + 1 && prev_words > 0 && floor == prev_floor) {
                            for (int k = i; k < j; k++) if (lp_id_eq(&ids[k], &ids[j])) { dup_part = true; break; }
                        }
                        if (!dup_part) {
                            int probe = i; for (int k = i; k <= j; k++) if (tier[k] == floor) { probe = k; break; }
                            ForwardSegCtx sctx = { &held[next], false, keepctx };
                            lp_pg_holders_direct(ids + probe, 1, ids + i, np, true, (int16)floor, fan, 2, 0, 0, forward_seg_hit, &sctx);
                            have = !sctx.is_hub;
                        }
                    }
                    if (!have) {
                        Datum v[6]; bool nl[6] = { false, false, false, false, true, false };
                        v[0] = Int32GetDatum(i + 1); v[1] = Int32GetDatum(j + 1); v[2] = Int64GetDatum(fan + 1); v[3] = Int64GetDatum(0); v[4] = (Datum)0; v[5] = Int64GetDatum(0);
                        set_put(fcinfo, ts, td, v, nl); cur = next; continue;
                    }
                } else
                    for (size_t r = 0; r < held[cur].n; r++) { const Held *h = &held[cur].v[r]; Geo g = geo_of(h->path);
                        if (h->tier > floor && lp_path_holds(g.p, &ids[j], 1, true)) lp_push(&held[next], *h); }
                uint64 paths = held[next].n; int64 runs = 0; bool unfollowed = paths == 0;
                lp_idmap_clear(followed);
                for (size_t r = 0; r < held[next].n; r++) {
                    Geo g = geo_of(held[next].v[r].path); size_t cap = g.p.n * 4 + 16; lp_vec_reserve(&out, cap);
                    size_t found = lp_path_follows(g.p, ids + i, (size_t)np, out.v, cap);
                    if (!found) { unfollowed = true; continue; }
                    runs++; if (found > cap) found = cap;
                    for (size_t x = 0; x < found; x++) (*(int64 *)lp_idmap_get(followed, &out.v[x], NULL))++;
                }
                Datum v[6]; bool nl[6] = { false, false, false, false, false, false };
                v[0] = Int32GetDatum(i + 1); v[1] = Int32GetDatum(j + 1); v[2] = Int64GetDatum((int64)paths); v[3] = Int64GetDatum(runs);
                for (size_t x = 0; x < lp_idmap_count(followed); x++) { v[4] = id_datum(lp_idmap_key(followed, x)); v[5] = Int64GetDatum(*(int64 *)lp_idmap_at(followed, x)); set_put(fcinfo, ts, td, v, nl); }
                if (unfollowed) { v[4] = (Datum)0; nl[4] = true; v[5] = Int64GetDatum(0); set_put(fcinfo, ts, td, v, nl); }
                cur = next;
            }
        }
    }
    for (int k = 0; k < nw; k++) lp_vec_free(&w_held[k]);
    pfree(w_held); pfree(w_hub); pfree(w_ids); pfree(w_at); pfree(tier); pfree(ids);
    MemoryContextDelete(keepctx); MemoryContextDelete(wordctx);
    lp_idmap_free(followed); lp_vec_free(&out); lp_vec_free(&held[0]); lp_vec_free(&held[1]);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_forward);
Datum laplace_forward(PG_FUNCTION_ARGS){ return run_laplace_forward(fcinfo, false); }

PG_FUNCTION_INFO_V1(laplace_forward_suffix);
Datum laplace_forward_suffix(PG_FUNCTION_ARGS){ return run_laplace_forward(fcinfo, true); }

/* ---------------------------------------------------------------- COUPLE, one coarse native operator
 * laplace_couple(occ, fan, refuse, shape, shape_n, keep): the coupling field of an admitted observation
 * (Sequence 20.2), in one call: bounded direct-AM indexed set access, zero SQL/SPI.
 *   strand       every claim holding an occurrence, refusals out before the fan (15.5): the claim's other end
 *   containment  every observation holding an occurrence (19.3), at most the fan an occurrence
 *   shape        the observed curves nearest the observation's own (19.6, 19.7): nominated by the GIN (what holds the
 *                occurrences) and the GiST (what lies nearest its centroid), each realized from its children's real
 *                coordinates and measured natively under the firmware's shape; the keep nearest, with their vertices
 * One row a response, its route kept apart (the field is typed state, not one scalar): route 0 strand, 1 containment,
 * 2 shape. occ: the occurrence it answers (1-based), 0 for shape. */
enum { ROUTE_STRAND = 0, ROUTE_CONTAIN = 1, ROUTE_SHAPE = 2 };
typedef struct { FunctionCallInfo fcinfo; Tuplestorestate *ts; TupleDesc td; Oid el; } Field;
static void emit(Field *f, const lp_id *id, int occ, int route, const lp_rating *r, const lp_id *via, const lp_id *rel, int tier, double distance, const lp_id *vert, int nvert){
    Datum o[11]; bool nl[11] = { 0 };
    o[0] = id_datum(id); o[1] = Int32GetDatum(occ); o[2] = Int16GetDatum(route);
    if (route == ROUTE_STRAND) { o[3] = Float8GetDatum(r->rating); o[4] = Float8GetDatum(r->deviation); o[5] = Float8GetDatum(r->volatility); o[6] = id_datum(via); o[7] = id_datum(rel); }
    else for (int k = 3; k <= 7; k++) { nl[k] = true; o[k] = 0; }
    o[8] = Int16GetDatum(tier);
    if (route == ROUTE_SHAPE) o[9] = Float8GetDatum(distance); else { nl[9] = true; o[9] = 0; }
    if (vert) o[10] = PointerGetDatum(id_array_of(f->el, vert, nvert)); else { nl[10] = true; o[10] = 0; }
    set_put(f->fcinfo, f->ts, f->td, o, nl);
}
/* Every ID's real coordinate: an atom's from the Tier-0 floor, every other one from its entity row in one direct read;
 * a map from ID to its four doubles, those with neither left out. */
typedef struct { double x[4]; bool has; } Coord4;
static void coord_visit(int64 ordinal, const lp_id *id, int16 tier, Datum coord, int64 hilbert, void *context){
    (void)ordinal; (void)tier; (void)hilbert;
    if (!coord) return;
    Coord4 *c = lp_idmap_lookup((lp_idmap *)context, id);
    if (!c) return;
    Geo gg = geo_of(coord);
    if (gg.p.n >= 1) { memcpy(c->x, xyzm(&gg), 32); c->has = true; }
}
static void coords_load(lp_idmap *at){
    size_t n = lp_idmap_count(at); if (!n) return;
    const lp_tier0_record *floor = lp_pg_tier0(); lp_id *rest = palloc(sizeof(lp_id) * n); size_t nr = 0;
    for (size_t i = 0; i < n; i++) {
        Coord4 *c = lp_idmap_at(at, i);
        const lp_tier0_record *atom = lp_tier0_find(floor, lp_idmap_key(at, i));
        if (atom) { lp_coord k; memcpy(k.m, atom->m, sizeof k.m); lp_coord_xyzm(&k, c->x); c->has = true; }
        else rest[nr++] = *lp_idmap_key(at, i);
    }
    if (nr) lp_pg_lookup_entity_coords(rest, nr, coord_visit, at);
    pfree(rest);
}
/* An observed curve: the entity, its constituents, and its distance from the observation's own. */
typedef struct { lp_id id; lp_id *v; int nv; double d; } Curve;
static int by_distance(const void *a, const void *b){ double x = ((const Curve *)a)->d, y = ((const Curve *)b)->d; return x < y ? -1 : x > y; }
typedef lp_vec(Curve) Curves;
/* A nominated curve, once: its constituents, when there are between 2 and 1024 of them. */
static void nominate(Curves *cv, lp_idmap *seen, const lp_id *id, Datum path){
    bool fresh; lp_idmap_put(seen, id, &fresh); if (!fresh) return;
    Geo g = geo_of(path); size_t n = lp_path_len(g.p); if (n < 2 || n > 1024) return;
    Curve *c = lp_vec_add(cv); c->id = *id; c->v = palloc(sizeof(lp_id) * n); c->nv = (int)lp_path_expand(g.p, c->v, n);
}
static double measure(int shape, double shape_n, const double *a, size_t na, const double *b, size_t nb){
    switch (shape) {
        case 1: return lp_frechet4_outliers(a, na, b, nb, (unsigned)shape_n);
        case 2: { size_t s; double d = lp_dtw4(a, na, b, nb, &s); return s ? d / (double)s : d; }
        case 3: return (double)lp_edr4(a, na, b, nb, shape_n);
        default: return lp_frechet4(a, na, b, nb);
    }
}
typedef struct { int idx; lp_rating rt; lp_id target; lp_id rel; } StrandHit;
typedef struct {
    const lp_id *occ;
    int nocc;
    int64 fan;
    int *held;
    lp_vec(StrandHit) hits;
} StrandCollect;
static void couple_strand_hit(const PgClaimHit *hit, void *context){
    StrandCollect *c = context;
    int idx = (int)hit->i - 1;
    if (idx < 0 || idx >= c->nocc) return;
    if (++c->held[idx] > c->fan || !hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[64];
    size_t np = lp_path_expand(g.p, part, 64);
    if (np < 2 || np > 64) return;
    int other = lp_tuple_other(part, np, &c->occ[idx]);
    if (other < 0) return;
    StrandHit *s = lp_vec_add(&c->hits);
    s->idx = idx;
    s->rt = (lp_rating){ hit->rating, hit->deviation, hit->volatility };
    s->target = part[other];
    s->rel = np >= 3 ? part[1] : c->occ[idx];
}
typedef struct {
    Field *f;
    const int *orig_idx;
    int group_n;
    int16 shape;
    Curves *cv;
    lp_idmap *seen;
} ContainCollect;
static void couple_contain_hit(const PgHolderHit *hit, void *context){
    ContainCollect *c = context;
    if (hit->hub || !hit->path) return;
    int64 gidx = hit->ordinal - 1;
    if (gidx < 0 || gidx >= c->group_n) return;
    int orig = c->orig_idx[gidx];
    emit(c->f, &hit->entity, orig + 1, ROUTE_CONTAIN, NULL, NULL, NULL, hit->tier, 0.0, NULL, 0);
    if (c->shape >= 0) nominate(c->cv, c->seen, &hit->entity, hit->path);
}
typedef struct { Curves *cv; lp_idmap *seen; } KnnNominate;
static void couple_knn_path(const lp_id *id, Datum path, Datum mask, void *context){
    (void)mask;
    KnnNominate *k = context;
    if (path) nominate(k->cv, k->seen, id, path);
}
PG_FUNCTION_INFO_V1(laplace_couple);
Datum laplace_couple(PG_FUNCTION_ARGS){
    Field f = { fcinfo, NULL, NULL, InvalidOid }; f.ts = set_begin(fcinfo, "laplace_couple", &f.td);
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0); f.el = get_element_type(idarr);
    int nocc; lp_id *occ = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nocc);
    int64 fan = PG_GETARG_INT64(1);
    int nrefuse; lp_id *refuse = ids_of(PG_GETARG_ARRAYTYPE_P(2), &nrefuse);
    int16 shape = PG_GETARG_INT16(3); double shape_n = PG_GETARG_FLOAT8(4); int32 keep = PG_GETARG_INT32(5);
    /* strands: every claim holding an occurrence, refusals out before the fan; the strand answers with its other end */
    if (nocc > 0 && fan > 0) {
        StrandCollect sc = { occ, nocc, fan, palloc0(sizeof(int) * (size_t)(nocc + 1)), { 0 } };
        lp_pg_claims_each_direct(occ, nocc, fan + 1, UINT64_C(1),
                                 refuse, nrefuse, 0,
                                 NULL, 0, 0, NULL, 0,
                                 couple_strand_hit, &sc);
        for (size_t r = 0; r < sc.hits.n; r++) {
            const StrandHit *s = &sc.hits.v[r];
            if (sc.held[s->idx] > fan) continue;
            emit(&f, &s->target, s->idx + 1, ROUTE_STRAND, &s->rt, &occ[s->idx], &s->rel, 0, 0.0, NULL, 0);
        }
        pfree(sc.held);
        lp_vec_free(&sc.hits);
    }
    pfree(refuse);
    /* containment: what holds each occurrence, the paths kept as the shape's GIN nominations */
    lp_idmap *seen = lp_idmap_new(); Curves cv = { 0 };
    if (nocc > 0 && fan >= 0) {
        int16 *occ_tier = palloc(sizeof(int16) * (size_t)nocc);
        lp_pg_lookup_entity_tiers(occ, (size_t)nocc, occ_tier);
        bool *done = palloc0(sizeof(bool) * (size_t)nocc);
        lp_id *g_ids = palloc(sizeof(lp_id) * (size_t)nocc);
        int *g_orig = palloc(sizeof(int) * (size_t)nocc);
        for (int i = 0; i < nocc; i++) {
            if (done[i] || occ_tier[i] < 0) continue;
            int16 t = occ_tier[i];
            int gn = 0;
            for (int j = i; j < nocc; j++) {
                if (!done[j] && occ_tier[j] == t) {
                    done[j] = true;
                    g_ids[gn] = occ[j];
                    g_orig[gn] = j;
                    gn++;
                }
            }
            ContainCollect cc = { &f, g_orig, gn, shape, &cv, seen };
            lp_pg_holders_direct(g_ids, gn, NULL, 0, true, t, fan, 2, 0, 0, couple_contain_hit, &cc);
        }
        pfree(g_orig); pfree(g_ids); pfree(done); pfree(occ_tier);
    }
    if (shape >= 0 && keep > 0 && nocc > 0) {
        /* the observation's own curve, and its centroid for the GiST */
        lp_idmap *oc = lp_idmap_sized(sizeof(Coord4)); for (int i = 0; i < nocc; i++) lp_idmap_put(oc, &occ[i], NULL); coords_load(oc);
        double *pa = palloc(sizeof(double) * 4 * (size_t)nocc), cen[4] = { 0 }; int npa = 0;
        for (int i = 0; i < nocc; i++) { const Coord4 *c = lp_idmap_lookup(oc, &occ[i]); if (c && c->has) memcpy(pa + 4 * npa++, c->x, 32); }
        lp_idmap_free(oc);
        for (int i = 0; i < npa; i++) for (int k = 0; k < 4; k++) cen[k] += pa[4 * i + k] / npa;
        if (npa >= 2) {
            /* the GiST's nominations: what lies nearest the centroid, at a tier that composes */
            if (fan > 0) {
                int k = fan > PG_INT32_MAX ? PG_INT32_MAX : (int)fan;
                PgKnnHit *hits = palloc(sizeof(PgKnnHit) * (size_t)k);
                int nk = lp_pg_knn4d(cen, k, false, 3, false, 0, NULL, hits, CurrentMemoryContext);
                if (nk > 0) {
                    lp_id *knn_ids = palloc(sizeof(lp_id) * (size_t)nk);
                    for (int i = 0; i < nk; i++) knn_ids[i] = hits[i].id;
                    KnnNominate kc = { &cv, seen };
                    lp_pg_lookup_paths(knn_ids, (size_t)nk, false, couple_knn_path, &kc);
                    pfree(knn_ids);
                }
                pfree(hits);
            }
            /* every child's real coordinate, once; each curve realized and measured */
            lp_idmap *kc = lp_idmap_sized(sizeof(Coord4)); for (size_t i = 0; i < cv.n; i++) for (int k = 0; k < cv.v[i].nv; k++) lp_idmap_put(kc, &cv.v[i].v[k], NULL);
            coords_load(kc);
            double *b = palloc(sizeof(double) * 4 * 1024);
            for (size_t i = 0; i < cv.n; i++) { int nb = 0;
                for (int k = 0; k < cv.v[i].nv; k++) { const Coord4 *c = lp_idmap_lookup(kc, &cv.v[i].v[k]); if (c && c->has) memcpy(b + 4 * nb++, c->x, 32); }
                cv.v[i].d = nb < 2 ? INFINITY : measure(shape, shape_n, pa, (size_t)npa, b, (size_t)nb); }
            pfree(b);
            lp_idmap_free(kc);
            lp_sort(cv.v, cv.n, sizeof(Curve), by_distance);
            for (size_t i = 0; i < cv.n && (int)i < keep; i++) if (isfinite(cv.v[i].d)) emit(&f, &cv.v[i].id, 0, ROUTE_SHAPE, NULL, NULL, NULL, 0, cv.v[i].d, cv.v[i].v, cv.v[i].nv);
        }
        pfree(pa);
    }
    lp_idmap_free(seen); lp_vec_free(&cv); pfree(occ);
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- CURVES, the shape a trajectory realizes
 * laplace_curves(ids): each entity's realized curve, the LINESTRING ZM through its constituents' real coordinates in
 * path order, every repetition kept, as COUPLE realizes the curves it measures (2 to 1024 constituents; a constituent
 * with no coordinate is left out). The curve is what the shape measures (laplace_frechet4d, laplace_hausdorff4d,
 * laplace_dtw4d, laplace_edr4d) are given; the packed path's carrier doubles are identity, never shape. One row an
 * input occurrence, in input order; curve_ewkb is null for an atom, an absent entity, or fewer than two realized
 * vertices. */
typedef struct { lp_id *v; int nv; } CurveSlot;
typedef struct { lp_idmap *slots; MemoryContext keep; } CurveRead;
static void curve_path(const lp_id *id, Datum path, Datum mask, void *context){
    (void)mask; CurveRead *r = context; CurveSlot *s = lp_idmap_lookup(r->slots, id); if (!s || s->v) return;
    Geo g = geo_of(path); size_t n = lp_path_len(g.p); if (n < 2 || n > 1024) return;
    MemoryContext previous = MemoryContextSwitchTo(r->keep);
    s->v = palloc(sizeof(lp_id) * n); s->nv = (int)lp_path_expand(g.p, s->v, n);
    MemoryContextSwitchTo(previous);
}
PG_FUNCTION_INFO_V1(laplace_curves);
Datum laplace_curves(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_curves", &td);
    int n; lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    CurveRead r = { lp_idmap_sized(sizeof(CurveSlot)), CurrentMemoryContext };
    for (int i = 0; i < n; i++) lp_idmap_put(r.slots, &ids[i], NULL);
    lp_pg_lookup_paths(lp_idmap_keys(r.slots), lp_idmap_count(r.slots), false, curve_path, &r);
    lp_idmap *kc = lp_idmap_sized(sizeof(Coord4)); size_t most = 0;
    for (size_t s = 0; s < lp_idmap_count(r.slots); s++) { CurveSlot *c = lp_idmap_at(r.slots, s);
        for (int k = 0; k < c->nv; k++) lp_idmap_put(kc, &c->v[k], NULL); if ((size_t)c->nv > most) most = (size_t)c->nv; }
    coords_load(kc);
    double *b = palloc(sizeof(double) * 4 * (most ? most : 1));
    for (int i = 0; i < n; i++) {
        const CurveSlot *c = lp_idmap_lookup(r.slots, &ids[i]); int nb = 0;
        for (int k = 0; c && k < c->nv; k++) { const Coord4 *x = lp_idmap_lookup(kc, &c->v[k]); if (x && x->has) memcpy(b + 4 * nb++, x->x, 32); }
        Datum o[3]; bool nl[3] = { false, false, nb < 2 };
        o[0] = Int64GetDatum((int64)i + 1); o[1] = id_datum(&ids[i]); o[2] = (Datum)0;
        if (nb >= 2) { size_t need = lp_ewkb_line_zm(b, (size_t)nb, NULL, NULL, 0); bytea *e = palloc(VARHDRSZ + need); SET_VARSIZE(e, VARHDRSZ + need);
            if (lp_ewkb_line_zm(b, (size_t)nb, NULL, (uint8 *)VARDATA(e), need) != need) ereport(ERROR, (errmsg("laplace_curves: curve encoding failed")));
            o[2] = PointerGetDatum(e); }
        set_put(fcinfo, ts, td, o, nl);
    }
    pfree(b); lp_idmap_free(kc); lp_idmap_free(r.slots); pfree(ids);
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- batched pair roles (pull_degrees.c roles_of) */
typedef struct { lp_id entity; Datum path; } RoleHit;
typedef lp_vec(RoleHit) RoleHitVec;
typedef struct { size_t start, count; } RoleSpan;
typedef struct {
    RoleHitVec *hits;
    lp_idmap *seen;
    MemoryContext ctx;
} RolePairCollect;

static void role_pair_hit(const PgHolderHit *hit, void *context){
    RolePairCollect *c = context;
    if (hit->hub || !hit->path) return;
    bool fresh;
    lp_idmap_get(c->seen, &hit->entity, &fresh);
    if (!fresh) return;
    MemoryContext old = MemoryContextSwitchTo(c->ctx);
    RoleHit rh = { hit->entity, PointerGetDatum(PG_DETOAST_DATUM_COPY(hit->path)) };
    lp_push(c->hits, rh);
    MemoryContextSwitchTo(old);
}

PG_FUNCTION_INFO_V1(laplace_roles_of);
Datum laplace_roles_of(PG_FUNCTION_ARGS){
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_roles_of", &td);
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_NULL();
    int nl_count = 0, nr_count = 0;
    lp_id *left = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nl_count);
    lp_id *right = ids_of(PG_GETARG_ARRAYTYPE_P(1), &nr_count);
    int64 fan = PG_ARGISNULL(2) ? -1 : PG_GETARG_INT64(2);
    int16 kinds = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT16(3) : 3;
    if (kinds < 1 || kinds > 3) {
        pfree(left); pfree(right);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_roles_of: invalid kinds")));
    }
    if (nl_count != nr_count) {
        pfree(left); pfree(right);
        ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("laplace_roles_of: left and right arrays must match length")));
    }
    if (nl_count <= 0) { pfree(left); pfree(right); PG_RETURN_NULL(); }

    MemoryContext pairctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_roles_of", ALLOCSET_DEFAULT_SIZES);
    lp_strmap *cache = lp_strmap_sized(sizeof(RoleSpan));
    RoleHitVec hits = { 0 };
    lp_idmap *seen = lp_idmap_new();

    PG_TRY(); {
        for (int k = 0; k < nl_count; k++) {
            CHECK_FOR_INTERRUPTS();
            lp_id pair[2] = { left[k], right[k] };
            if (lp_id_cmp(&pair[0], &pair[1]) > 0) { lp_id tmp = pair[0]; pair[0] = pair[1]; pair[1] = tmp; }
            bool fresh;
            RoleSpan *span = lp_strmap_get(cache, pair, sizeof pair, &fresh);
            if (fresh) {
                span->start = hits.n;
                lp_idmap_clear(seen);
                RolePairCollect c = { &hits, seen, pairctx };
                if (lp_id_eq(&pair[0], &pair[1]))
                    lp_pg_holders_direct(&pair[0], 1, NULL, 0, true, -1, fan, kinds, 0, 0, role_pair_hit, &c);
                else
                    lp_pg_holders_direct(&pair[0], 1, &pair[1], 1, true, -1, fan, kinds, 0, 0, role_pair_hit, &c);
                span->count = hits.n - span->start;
            }
            for (size_t i = 0; i < span->count; i++) {
                const RoleHit *rh = &hits.v[span->start + i];
                Datum o[3] = { Int64GetDatum((int64)k + 1), id_datum(&rh->entity), rh->path };
                bool nl[3] = { false, false, false };
                set_put(fcinfo, ts, td, o, nl);
                pfree(DatumGetPointer(o[1]));
            }
        }
    } PG_FINALLY(); {
        lp_strmap_free(cache);
        lp_idmap_free(seen);
        lp_vec_free(&hits);
        MemoryContextDelete(pairctx);
        pfree(left); pfree(right);
    } PG_END_TRY();
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- multi-tier upward multiplicity closure (fills.c) */
typedef struct { int64 times; int16 tier; int32 level; } FillsAcc;
typedef struct {
    const lp_id *front_ids;
    const int16 *front_tiers;
    const int64 *front_delta;
    size_t nf;
    uint64_t *times_buf;
    int32 level;
    lp_idmap *level_seen;
    lp_idmap *acc;        /* lp_id -> FillsAcc */
    lp_idmap *next_delta; /* lp_id -> int64 */
} FillsClosureCtx;

static inline int64 sat_mul_i64(int64 a, uint64_t b){
    if (a <= 0 || b == 0) return 0;
    if (b > (uint64_t)(PG_INT64_MAX / a)) return PG_INT64_MAX;
    return a * (int64)b;
}
static inline int64 sat_add_i64(int64 a, int64 b){
    if (a > PG_INT64_MAX - b) return PG_INT64_MAX;
    return a + b;
}

static void fills_closure_hit(const PgHolderHit *hit, void *context){
    FillsClosureCtx *c = context;
    if (hit->hub || !hit->path) return;
    bool fresh_lvl;
    lp_idmap_get(c->level_seen, &hit->entity, &fresh_lvl);
    if (!fresh_lvl) return;

    Geo g = geo_of(hit->path);
    lp_path_times(g.p, c->front_ids, c->nf, c->times_buf);
    int64 gain = 0;
    for (size_t i = 0; i < c->nf; i++) {
        if (!c->times_buf[i]) continue;
        if (c->front_tiers[i] >= 0 && hit->tier <= c->front_tiers[i]) continue;
        gain = sat_add_i64(gain, sat_mul_i64(c->front_delta[i], c->times_buf[i]));
    }
    if (gain <= 0) return;

    bool fresh;
    FillsAcc *acc = lp_idmap_get(c->acc, &hit->entity, &fresh);
    if (fresh) *acc = (FillsAcc){ gain, hit->tier, c->level };
    else {
        acc->times = sat_add_i64(acc->times, gain);
        if (hit->tier > acc->tier) acc->tier = hit->tier;
    }
    int64 *nd = lp_idmap_get(c->next_delta, &hit->entity, &fresh);
    *nd = fresh ? gain : sat_add_i64(*nd, gain);
}

PG_FUNCTION_INFO_V1(laplace_fills_closure);
Datum laplace_fills_closure(PG_FUNCTION_ARGS){
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_fills_closure", &td);
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    int n = 0; lp_id *seeds = ids_of(PG_GETARG_ARRAYTYPE_P(0), &n);
    if (n <= 0) { pfree(seeds); PG_RETURN_NULL(); }

    int64 *seed_times = NULL;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1)) {
        ArrayType *ta = PG_GETARG_ARRAYTYPE_P(1);
        Datum *td_vals; bool *td_null; int ntimes = 0;
        deconstruct_array(ta, INT8OID, 8, FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &td_vals, &td_null, &ntimes);
        if (ntimes != n) {
            pfree(td_vals); pfree(td_null); pfree(seeds);
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("laplace_fills_closure: seeds and seed_times must match length")));
        }
        seed_times = palloc(sizeof(int64) * (size_t)n);
        for (int i = 0; i < n; i++) seed_times[i] = td_null[i] ? 0 : DatumGetInt64(td_vals[i]);
        pfree(td_vals); pfree(td_null);
    }
    int32 max_levels = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 64;
    if (max_levels < 1) max_levels = 1;

    lp_idmap *acc = lp_idmap_sized(sizeof(FillsAcc));
    lp_idmap *front = lp_idmap_sized(sizeof(int64));
    lp_idmap *next = lp_idmap_sized(sizeof(int64));
    lp_idmap *level_seen = lp_idmap_new();

    PG_TRY(); {
        for (int i = 0; i < n; i++) {
            int64 w = seed_times ? seed_times[i] : 1;
            if (w <= 0) continue;
            bool fresh;
            int64 *cur = lp_idmap_get(front, seeds + i, &fresh);
            *cur = fresh ? w : sat_add_i64(*cur, w);
        }
        for (int32 lvl = 1; lvl <= max_levels && lp_idmap_count(front) > 0; lvl++) {
            CHECK_FOR_INTERRUPTS();
            size_t nf = lp_idmap_count(front);
            const lp_id *fids = lp_idmap_keys(front);
            int16 *ftiers = palloc(sizeof(int16) * nf);
            int64 *fdelta = palloc(sizeof(int64) * nf);
            uint64_t *tbuf = palloc(sizeof(uint64_t) * nf);
            lp_pg_lookup_entity_tiers(fids, nf, ftiers);
            int16 min_tier = INT16_MAX;
            for (size_t i = 0; i < nf; i++) {
                fdelta[i] = *(int64 *)lp_idmap_at(front, i);
                if (ftiers[i] >= 0 && ftiers[i] < min_tier) min_tier = ftiers[i];
            }
            if (min_tier != INT16_MAX) {
                lp_idmap_clear(next);
                lp_idmap_clear(level_seen);
                FillsClosureCtx ctx = { fids, ftiers, fdelta, nf, tbuf, lvl, level_seen, acc, next };
                lp_pg_holders_direct(fids, (int)nf, NULL, 0, false, min_tier, -1, 3, 0, 0, fills_closure_hit, &ctx);
                lp_idmap_clear(front);
                const lp_id *nx = lp_idmap_keys(next);
                size_t nxn = lp_idmap_count(next);
                for (size_t i = 0; i < nxn; i++)
                    *(int64 *)lp_idmap_get(front, &nx[i], NULL) = *(int64 *)lp_idmap_at(next, i);
            } else {
                lp_idmap_clear(front);
            }
            pfree(ftiers); pfree(fdelta); pfree(tbuf);
        }
        for (size_t i = 0; i < lp_idmap_count(acc); i++) {
            const FillsAcc *a = lp_idmap_at(acc, i);
            Datum o[4] = {
                id_datum(lp_idmap_key(acc, i)),
                Int64GetDatum(a->times),
                Int16GetDatum(a->tier),
                Int32GetDatum(a->level)
            };
            bool nl[4] = { false, false, false, false };
            set_put(fcinfo, ts, td, o, nl);
            pfree(DatumGetPointer(o[0]));
        }
    } PG_FINALLY(); {
        lp_idmap_free(acc); lp_idmap_free(front);
        lp_idmap_free(next); lp_idmap_free(level_seen);
        if (seed_times) pfree(seed_times);
        pfree(seeds);
    } PG_END_TRY();
    PG_RETURN_NULL();
}
