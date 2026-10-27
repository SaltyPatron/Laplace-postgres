/* One downward set traversal for consumers of registered canonical structure. */
#include "pg_core.h"
#include "geo.h"
#include "catalog/pg_type.h"
#include "nodes/params.h"

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
static void walk_root(TupleTableSlot *row, void *context)
{
    WitnessWalk *walk = context; bool missing;
    Datum id = slot_getattr(row, 1, &missing);
    if (missing) ereport(ERROR, (errmsg("registered witness has a null identity")));
    walk_remember(walk, &walk->frontier, (const lp_id *)DatumGetPointer(id));
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
    Oid types[] = {get_array_type(get_atttype(lp_pg_relation("witness"), get_attnum(lp_pg_relation("witness"), "id")))};
    ParamListInfo params = makeParamList(1);
    params->params[0] = (ParamExternData){.ptype = types[0], .value = PointerGetDatum(roots), .isnull = roots == NULL};
    if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("witness traversal: SPI connect failed")));
    SPIPlanPtr plan = lp_pg_prepare_cursor(
        "SELECT id FROM @extschema@.witness WHERE ($1 IS NULL OR id OPERATOR(@extschema@.=) ANY($1)) ORDER BY id",
        1, types, CURSOR_OPT_CUSTOM_PLAN);
    if (!plan) ereport(ERROR, (errmsg("witness traversal: cannot prepare root selection")));
    lp_pg_stream(plan, params, walk_root, &walk);
    SPI_freeplan(plan); SPI_finish(); pfree(params);
    while (walk.frontier.n) {
        CHECK_FOR_INTERRUPTS();
        lp_pg_lookup_paths(walk.frontier.v, walk.frontier.n, false, walk_path, &walk);
        WalkIds consumed = walk.frontier;
        walk.frontier = walk.next; walk.next = consumed; walk.next.n = 0;
    }
    lp_vec_free(&walk.frontier); lp_vec_free(&walk.next); lp_idmap_free(walk.seen);
}
