/* Forward sequence pass, gap fills, and 4D geometric/trajectory coupling
 * (laplace_fills, laplace_forward, laplace_couple). */
#include "pg_core.h"
#include "geo.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "laplace/consensus.h"
#include <math.h>

/* Every read of the web is a statement planned once per backend and kept, run as one executor pass over a set, its
 * rows returned through a tuplestore. */
typedef struct { const char *name; const char *sql; int nargs; SPIPlanPtr plan; uint64 generation; } Kept;
#define KEPT_ARGS 8
/* The statement's plan, made the first time it is run, with these argument types. Inside SPI. */
static SPIPlanPtr kept_plan(Kept *k, const Oid *types){
    uint64 generation=lp_pg_binding_generation();
    if(k->plan && k->generation!=generation){SPI_freeplan(k->plan);k->plan=NULL;}
    if (!k->plan) {
        char *bound=lp_pg_sql(k->sql);
        k->plan = SPI_prepare(bound, k->nargs, (Oid *)types);pfree(bound);
        if (!k->plan) ereport(ERROR, (errmsg("%s: SPI_prepare: %s", k->name, SPI_result_code_string(SPI_result))));
        SPI_keepplan(k->plan);
        k->generation=generation;
    }
    return k->plan;
}
static void kept_run(Kept *k, const Oid *types, Datum *args){
    if (SPI_execute_plan(kept_plan(k, types), args, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("%s: %s", k->name, SPI_result_code_string(SPI_result))));
}

/* Run a kept statement over the call's own arguments, its argument types the call's, and hand every row through. */
static void set_run(FunctionCallInfo fcinfo, Kept *k){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, k->name, &td);
    if (k->nargs > KEPT_ARGS || td->natts > KEPT_ARGS) ereport(ERROR, (errmsg("%s: more than %d arguments or columns", k->name, KEPT_ARGS)));
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("%s: SPI_connect", k->name)));
    Oid t[KEPT_ARGS]; Datum a[KEPT_ARGS]; char nulls[KEPT_ARGS];
    for (int i = 0; i < k->nargs; i++) {
        t[i] = get_fn_expr_argtype(fcinfo->flinfo, i);
        nulls[i] = PG_ARGISNULL(i) ? 'n' : ' ';
        a[i] = PG_ARGISNULL(i) ? (Datum)0 : PG_GETARG_DATUM(i);
    }
    if (SPI_execute_plan(kept_plan(k,t),a,nulls,true,0) != SPI_OK_SELECT)
        ereport(ERROR,(errmsg("%s: %s",k->name,SPI_result_code_string(SPI_result))));
    int nc = SPI_tuptable->tupdesc->natts;
    for (uint64 j = 0; j < SPI_processed; j++) {
        Datum v[KEPT_ARGS]; bool nl[KEPT_ARGS];
        for (int c = 0; c < nc; c++) v[c] = SPI_getbinval(SPI_tuptable->vals[j], SPI_tuptable->tupdesc, c + 1, &nl[c]);
        tuplestore_putvalues(ts, td, v, nl);
    }
    SPI_finish();
}
#define SET_FN(fn, kept) PG_FUNCTION_INFO_V1(fn); Datum fn(PG_FUNCTION_ARGS){ set_run(fcinfo, &kept); PG_RETURN_NULL(); }

/* laplace_fills(keys): every path above the lowest key that holds any of them, with the times each key is followed by
 * the next vertex of that path: what fills the gap after it. */
static Kept fills_kept = { "laplace_fills",
    "SELECT p.entity, t.id, t.times, p.tier FROM @extschema@.physicality p, @extschema@.laplace_path_times(p.path, $1) t "
    "WHERE p.tier > (SELECT min(e.tier) FROM @extschema@.entity e WHERE e.id OPERATOR(@extschema@.=) ANY($1)) AND p.path OPERATOR(@extschema@.&&) $1", 1, NULL, 0 };
SET_FN(laplace_fills, fills_kept)

/* laplace_forward(ids, fan): the forward pass over every contiguous segment of a prompt at once. For each segment
 * [i..j] of the prompt's constituents: the observations holding all of its parts (claims left out), how many hold it as
 * a run, and what follows the run in each, counted. A segment held by more than fan observations is a hub, reached and
 * not crossed: its row says paths fan + 1 (more than the fan), runs 0, and nothing follows it. Counting over the first
 * fan the index met would count a different set on another install of the same content. One row per continuation (next, times);
 * a segment held by nothing, or held where nothing follows the run, has a row with next null. SQL only fetches the
 * paths that hold a segment, through the container index (Architecture: SQL fetches and writes records); the runs are
 * matched and what follows them is counted here, by Laplace-Native: precedes, contains and co-occurrence from the
 * trajectories, no softmax, no window. */
static Kept forward_kept = { "laplace_forward",
    "SELECT h.ordinal, h.path, h.tier, h.hub FROM @extschema@.laplace_holders($1, $2, true, $3, $4, 2::smallint) h", 4, NULL, 0 };
typedef struct { AttrNumber att; int16 *tier; } ForwardTiers;
static void forward_tier_row(TupleTableSlot *slot, const PresenceKey *key, void *context){
    ForwardTiers *ft = context; bool nl; Datum d = slot_getattr(slot, ft->att, &nl);
    if (!nl) { int16 t = DatumGetInt16(d); if (t > ft->tier[key->ordinal - 1]) ft->tier[key->ordinal - 1] = t; }
}
/* A segment's observations: each path as it lies, and its tier. */
typedef struct { Datum path; int16 tier; } Held;
typedef lp_vec(Held) Helds;
PG_FUNCTION_INFO_V1(laplace_forward);
Datum laplace_forward(PG_FUNCTION_ARGS){
    TupleDesc td; Tuplestorestate *ts = set_begin(fcinfo, "laplace_forward", &td);
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0); Oid el = ARR_ELEMTYPE(arr), at = get_fn_expr_argtype(fcinfo->flinfo, 0), ftypes[4] = { at, at, INT2OID, INT8OID };
    int n; lp_id *ids = ids_of(arr, &n); int64 fan = PG_GETARG_INT64(1); if (n > 256) n = 256;
    int16 *tier = palloc(sizeof(int16) * (size_t)(n ? n : 1)); for (int i = 0; i < n; i++) tier[i] = -1;
    if (n > 0) {
        const char *sel[] = { "id", "tier" }, *col; Oid target = lp_pg_relation("entity");
        Relation ent = lp_pg_read_parent(target, &col, sel, lengthof(sel));
        ForwardTiers ft = { get_attnum(target, "tier"), tier };
        PresenceKey *pkeys = palloc_extended((Size)n * sizeof(*pkeys), MCXT_ALLOC_HUGE);
        for (int i = 0; i < n; i++) pkeys[i] = (PresenceKey){ .id = ids[i], .ordinal = (int64)i + 1 };
        lp_pg_indexed_keys(ent, col, pkeys, (size_t)n, forward_tier_row, &ft);
        pfree(pkeys); table_close(ent, NoLock);
    }
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_forward: SPI_connect")));
    lp_idmap *followed = lp_idmap_sized(sizeof(int64)); lp_vec(lp_id) out = { 0 };
    MemoryContext rowctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward rows", ALLOCSET_DEFAULT_SIZES),
                  keepctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward held", ALLOCSET_DEFAULT_SIZES),
                  wordctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_forward words", ALLOCSET_DEFAULT_SIZES);
    lp_id *w_ids = palloc(sizeof(lp_id) * (size_t)(n ? n : 1));
    int *w_at = palloc(sizeof(int) * (size_t)(n ? n : 1)), nw = 0;
    for (int i = 0; i < n; i++) {
        w_at[i] = -1; if (tier[i] <= 0) continue;
        for (int k = 0; k < nw; k++) if (lp_id_eq(&w_ids[k], &ids[i])) { w_at[i] = k; break; }
        if (w_at[i] < 0) { w_at[i] = nw; w_ids[nw++] = ids[i]; }
    }
    Helds *w_held = palloc0(sizeof(Helds) * (size_t)(nw ? nw : 1));
    bool *w_hub = palloc0(sizeof(bool) * (size_t)(nw ? nw : 1));
    if (nw > 0) {
        MemoryContext old = MemoryContextSwitchTo(rowctx);
        Datum a[4] = { PointerGetDatum(id_array_of(el, w_ids, nw)), PointerGetDatum(id_array_of(el, NULL, 0)), Int16GetDatum(0), Int64GetDatum(fan) };
        kept_run(&forward_kept, ftypes, a);
        MemoryContextSwitchTo(wordctx);
        for (uint64 r = 0; r < SPI_processed; r++) {
            bool nl; int64 ord = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)) - 1;
            if (ord < 0 || ord >= nw) continue;
            if (DatumGetBool(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 4, &nl))) { w_hub[ord] = true; continue; }
            Datum d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl);
            int16 t = DatumGetInt16(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 3, &nl));
            lp_push(&w_held[ord], (Held){ PointerGetDatum(PG_DETOAST_DATUM_COPY(d)), t });
        }
        SPI_freetuptable(SPI_tuptable); MemoryContextSwitchTo(old); MemoryContextReset(rowctx);
    }
    Helds held[2] = { { 0 }, { 0 } };
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
                        MemoryContext old = MemoryContextSwitchTo(rowctx);
                        Datum a[4] = { PointerGetDatum(id_array_of(el, ids + probe, 1)), PointerGetDatum(id_array_of(el, ids + i, np)), Int16GetDatum((int16)floor), Int64GetDatum(fan) };
                        kept_run(&forward_kept, ftypes, a);
                        MemoryContextSwitchTo(keepctx);
                        bool is_hub = false;
                        for (uint64 r = 0; r < SPI_processed; r++) {
                            bool nl; if (DatumGetBool(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 4, &nl))) { is_hub = true; continue; }
                            Datum d = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl);
                            int16 t = DatumGetInt16(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 3, &nl));
                            lp_push(&held[next], (Held){ PointerGetDatum(PG_DETOAST_DATUM_COPY(d)), t });
                        }
                        SPI_freetuptable(SPI_tuptable); MemoryContextSwitchTo(old); MemoryContextReset(rowctx);
                        have = !is_hub;
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
            /* what follows the run in each path, counted by ID */
            uint64 paths = held[next].n; int64 runs = 0; bool unfollowed = paths == 0;
            lp_idmap_clear(followed);
            for (size_t r = 0; r < held[next].n; r++) {
                Geo g = geo_of(held[next].v[r].path); size_t cap = g.p.n * 4 + 16; lp_vec_reserve(&out, cap);
                size_t found = lp_path_follows(g.p, ids + i, (size_t)np, out.v, cap);
                if (!found) { unfollowed = true; continue; }                     /* it holds the parts, and not as a run that anything follows */
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
    for (int k = 0; k < nw; k++) lp_vec_free(&w_held[k]);
    MemoryContextDelete(rowctx); MemoryContextDelete(keepctx); MemoryContextDelete(wordctx);
    lp_idmap_free(followed); lp_vec_free(&out); lp_vec_free(&held[0]); lp_vec_free(&held[1]);
    SPI_finish(); PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- COUPLE, one coarse native operator
 * laplace_couple(occ, fan, refuse, shape, shape_n, keep): the coupling field of an admitted observation
 * (Sequence 20.2), in one call: bounded indexed set access through SPI, everything else native.
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
/* Every ID's real coordinate, in one read: a map from ID to its four doubles, those with none left out. */
typedef struct { double x[4]; bool has; } Coord4;
static void coords_spi(Oid idarr, lp_idmap *at){
    size_t n = lp_idmap_count(at); if (!n) return;
    Datum a[1] = { PointerGetDatum(id_array_of(get_element_type(idarr), lp_idmap_keys(at), (int)n)) }; Oid t[1] = { idarr };
    if (lp_pg_execute_args("SELECT e.id, e.coord FROM @extschema@.entity e WHERE e.id OPERATOR(@extschema@.=) ANY($1)", 1, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: coordinates")));
    for (uint64 r = 0; r < SPI_processed; r++) {
        bool nl; Datum g = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl); if (nl) continue;
        Coord4 *c = lp_idmap_lookup(at, (const lp_id *)DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)));
        Geo gg = geo_of(g); if (!c || gg.p.n < 1) continue;
        memcpy(c->x, xyzm(&gg), 32); c->has = true;
    }
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
PG_FUNCTION_INFO_V1(laplace_couple);
Datum laplace_couple(PG_FUNCTION_ARGS){
    Field f = { fcinfo, NULL, NULL, InvalidOid }; f.ts = set_begin(fcinfo, "laplace_couple", &f.td);
    Oid idarr = get_fn_expr_argtype(fcinfo->flinfo, 0); f.el = get_element_type(idarr);
    ArrayType *occa = PG_GETARG_ARRAYTYPE_P(0); int nocc; lp_id *occ = ids_of(occa, &nocc);
    int64 fan = PG_GETARG_INT64(1); int16 shape = PG_GETARG_INT16(3); double shape_n = PG_GETARG_FLOAT8(4); int32 keep = PG_GETARG_INT32(5);
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("laplace_couple: SPI_connect")));
    Datum fanp1 = Int64GetDatum(fan + 1), fand = Int64GetDatum(fan);
    /* strands: every claim holding an occurrence, refusals out before the fan; the strand answers with its other end */
    { Datum a[3] = { PointerGetDatum(occa), fanp1, PG_GETARG_DATUM(2) }; Oid t[3] = { idarr, INT8OID, idarr };
      if (lp_pg_execute_args("SELECT u.i, c.path, c.rating, c.deviation, c.volatility FROM pg_catalog.unnest($1) WITH ORDINALITY AS u(id, i) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id "
            "CROSS JOIN LATERAL (SELECT p.path, s.rating, s.deviation, s.volatility FROM @extschema@.physicality p JOIN @extschema@.consensus s ON s.claim OPERATOR(@extschema@.=) p.entity WHERE p.tier > e.tier AND p.path OPERATOR(@extschema@.@>) ARRAY[u.id] "
            "AND p.mask OPERATOR(@extschema@.?) 0::smallint AND NOT @extschema@.laplace_middle_any(p.path, $3) LIMIT $2) c", 3, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: strands")));
      int *held = palloc0(sizeof(int) * (size_t)(nocc + 1));                /* an occurrence more than the fan holds is a hub: what the scan met first is not its strands */
      for (uint64 r = 0; r < SPI_processed; r++) { bool nl; int i = (int)DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)) - 1; if (i >= 0 && i < nocc) held[i]++; }
      for (uint64 r = 0; r < SPI_processed; r++) { HeapTuple tup = SPI_tuptable->vals[r]; TupleDesc d = SPI_tuptable->tupdesc; bool nl;
          int i = (int)DatumGetInt64(SPI_getbinval(tup, d, 1, &nl)) - 1; if (i < 0 || i >= nocc || held[i] > fan) continue;
          Geo g = geo_of(SPI_getbinval(tup, d, 2, &nl)); lp_id part[64]; size_t np = lp_path_expand(g.p, part, 64); if (np < 2 || np > 64) continue;
          int other = lp_tuple_other(part, np, &occ[i]); if (other < 0) continue;
          lp_rating rt = { DatumGetFloat8(SPI_getbinval(tup, d, 3, &nl)), DatumGetFloat8(SPI_getbinval(tup, d, 4, &nl)), DatumGetFloat8(SPI_getbinval(tup, d, 5, &nl)) };
          emit(&f, &part[other], i + 1, ROUTE_STRAND, &rt, &occ[i], np >= 3 ? &part[1] : &occ[i], 0, 0.0, NULL, 0); } }
    /* containment: what holds each occurrence, the paths kept as the shape's GIN nominations */
    lp_idmap *seen = lp_idmap_new(); Curves cv = { 0 };
    { Datum a[2] = { PointerGetDatum(occa), fanp1 }; Oid t[2] = { idarr, INT8OID };
      if (lp_pg_execute_args("SELECT u.i, c.entity, c.tier, c.path FROM pg_catalog.unnest($1) WITH ORDINALITY AS u(id, i) JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) u.id "
            "CROSS JOIN LATERAL (SELECT p.entity, p.tier, p.path FROM @extschema@.physicality p WHERE p.tier > e.tier AND p.path OPERATOR(@extschema@.@>) ARRAY[u.id] AND NOT (p.mask OPERATOR(@extschema@.?) 0::smallint) LIMIT $2) c", 2, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: containment")));
      int *held = palloc0(sizeof(int) * (size_t)(nocc + 1));                /* an occurrence more than the fan holds is a hub: reached, never crossed */
      for (uint64 r = 0; r < SPI_processed; r++) { bool nl; int i = (int)DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)) - 1; if (i >= 0 && i < nocc) held[i]++; }
      for (uint64 r = 0; r < SPI_processed; r++) { HeapTuple tup = SPI_tuptable->vals[r]; TupleDesc d = SPI_tuptable->tupdesc; bool nl;
          int i = (int)DatumGetInt64(SPI_getbinval(tup, d, 1, &nl)) - 1; if (i < 0 || i >= nocc || held[i] > fan) continue;
          lp_id id; memcpy(id.b, DatumGetPointer(SPI_getbinval(tup, d, 2, &nl)), 16);
          emit(&f, &id, i + 1, ROUTE_CONTAIN, NULL, NULL, NULL, DatumGetInt16(SPI_getbinval(tup, d, 3, &nl)), 0.0, NULL, 0);
          if (shape >= 0) nominate(&cv, seen, &id, SPI_getbinval(tup, d, 4, &nl)); } }
    if (shape >= 0 && keep > 0) {
        /* the observation's own curve, and its centroid for the GiST */
        lp_idmap *oc = lp_idmap_sized(sizeof(Coord4)); for (int i = 0; i < nocc; i++) lp_idmap_put(oc, &occ[i], NULL); coords_spi(idarr, oc);
        double *pa = palloc(sizeof(double) * 4 * (size_t)nocc), cen[4] = { 0 }; int npa = 0;
        for (int i = 0; i < nocc; i++) { const Coord4 *c = lp_idmap_lookup(oc, &occ[i]); if (c && c->has) memcpy(pa + 4 * npa++, c->x, 32); }
        lp_idmap_free(oc);
        for (int i = 0; i < npa; i++) for (int k = 0; k < 4; k++) cen[k] += pa[4 * i + k] / npa;
        if (npa >= 2) {
            /* the GiST's nominations: what lies nearest the centroid, at a tier that composes */
            uint8 pt[64]; size_t pl = lp_ewkb_point4(cen, pt, sizeof pt);
            Datum a[2] = { PointerGetDatum(bytes_datum(pt, pl)), fand }; Oid t[2] = { BYTEAOID, INT8OID };
            if (lp_pg_execute_args("SELECT e.id, p.path FROM (SELECT e.id FROM @extschema@.entity e WHERE e.tier >= 3 ORDER BY e.coord OPERATOR(@extschema@.<~>) @postgis@.ST_GeomFromEWKB($1) LIMIT $2) e JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) e.id", 2, t, a, NULL, true, 0) != SPI_OK_SELECT) ereport(ERROR, (errmsg("laplace_couple: nearest")));
            for (uint64 r = 0; r < SPI_processed; r++) { bool nl; lp_id id; memcpy(id.b, DatumGetPointer(SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1, &nl)), 16);
                nominate(&cv, seen, &id, SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 2, &nl)); }
            /* every child's real coordinate, once; each curve realized and measured */
            lp_idmap *kc = lp_idmap_sized(sizeof(Coord4)); for (size_t i = 0; i < cv.n; i++) for (int k = 0; k < cv.v[i].nv; k++) lp_idmap_put(kc, &cv.v[i].v[k], NULL);
            coords_spi(idarr, kc);
            double *b = palloc(sizeof(double) * 4 * 1024);
            for (size_t i = 0; i < cv.n; i++) { int nb = 0;
                for (int k = 0; k < cv.v[i].nv; k++) { const Coord4 *c = lp_idmap_lookup(kc, &cv.v[i].v[k]); if (c && c->has) memcpy(b + 4 * nb++, c->x, 32); }
                cv.v[i].d = nb < 2 ? INFINITY : measure(shape, shape_n, pa, (size_t)npa, b, (size_t)nb); }
            lp_idmap_free(kc);
            lp_sort(cv.v, cv.n, sizeof(Curve), by_distance);
            for (size_t i = 0; i < cv.n && (int)i < keep; i++) if (isfinite(cv.v[i].d)) emit(&f, &cv.v[i].id, 0, ROUTE_SHAPE, NULL, NULL, NULL, 0, cv.v[i].d, cv.v[i].v, cv.v[i].nv);
        }
    }
    lp_idmap_free(seen); lp_vec_free(&cv);
    SPI_finish();
    PG_RETURN_NULL();
}
