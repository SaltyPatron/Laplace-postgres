/* Canonical projection from an immutable content path. SQL supplies complete
 * child-coordinate sets; Native owns IDs, run weights, centroids and Hilbert. */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "varatt.h"
#include "laplace/tier0.h"
#include "geo.h"

PG_FUNCTION_INFO_V1(laplace_project_path);
Datum laplace_project_path(PG_FUNCTION_ARGS){
    const lp_id *expected = (const lp_id *)PG_GETARG_POINTER(0);
    Geo path = geo_of(PG_GETARG_DATUM(1));
    ArrayType *keys = PG_GETARG_ARRAYTYPE_P(2), *points = PG_GETARG_ARRAYTYPE_P(3);
    Datum *ids, *coords; bool *id_nulls, *coord_nulls; int ni, nc;
    int16 point_len; bool point_byval; char point_align;
    get_typlenbyvalalign(ARR_ELEMTYPE(points), &point_len, &point_byval, &point_align);
    deconstruct_array(keys, ARR_ELEMTYPE(keys), 16, false, TYPALIGN_CHAR, &ids, &id_nulls, &ni);
    deconstruct_array(points, ARR_ELEMTYPE(points), point_len, point_byval, point_align, &coords, &coord_nulls, &nc);
    if (ni != nc || ARR_NDIM(keys) > 1 || ARR_NDIM(points) > 1 || !path.p.n || path.p.n > MaxAllocSize / sizeof(lp_ref))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_project_path: invalid child bindings or empty path")));
    const lp_tier0_record *floor = lp_tier0_map(GetConfigOption("laplace.tier0", false, false));
    if (!floor) ereport(ERROR, (errmsg("laplace_project_path: cannot map canonical Tier0")));
    lp_idmap *bindings = lp_idmap_sized(sizeof(lp_ref));
    for (int i = 0; i < ni; i++) {
        if (id_nulls[i]) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_project_path: null child ID")));
        const lp_id *id = (const lp_id *)DatumGetPointer(ids[i]); bool fresh;
        lp_ref *ref = lp_idmap_get(bindings, id, &fresh);
        if (!fresh) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_project_path: duplicate child binding")));
        memset(ref, 0, sizeof *ref); ref->id = *id;
        const lp_tier0_record *atom = lp_tier0_find(floor, id);
        if (atom) { memcpy(ref->c.m, atom->m, sizeof ref->c.m); continue; }
        if (coord_nulls[i]) ereport(ERROR, (errcode(ERRCODE_FOREIGN_KEY_VIOLATION), errmsg("laplace_project_path: missing non-Tier0 child")));
        Geo point = geo_of(coords[i]); double value[4];
        if (point.type != 1 || point.p.n != 1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_project_path: child coordinate must be POINT ZM")));
        memcpy(value, point.p.v, sizeof value);
        if (!lp_coord_of_xyzm(value, &ref->c) || !lp_coord_inside(&ref->c))
            ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_project_path: child coordinate is off the canonical grid or outside the wall")));
    }
    lp_ref *children = palloc(path.p.n * sizeof *children);
    uint64_t *runs = palloc(path.p.n * sizeof *runs);
    for (size_t i = 0; i < path.p.n; i++) {
        lp_id id = lp_path_id(path.p, i);
        int64 at = lp_idmap_find(bindings, &id);
        if (at >= 0) children[i] = *(lp_ref *)lp_idmap_at(bindings, (size_t)at);
        else {
            const lp_tier0_record *atom = lp_tier0_find(floor, &id);
            if (!atom) ereport(ERROR, (errcode(ERRCODE_FOREIGN_KEY_VIOLATION), errmsg("laplace_project_path: path child has no coordinate binding")));
            memset(&children[i], 0, sizeof children[i]); children[i].id = id; memcpy(children[i].c.m, atom->m, sizeof children[i].c.m);
        }
        runs[i] = lp_path_run(path.p, i);
    }
    lp_ref result;
    if (!lp_ref_compose_runs(children, runs, path.p.n, 0, &result) || !lp_id_eq(expected, &result.id))
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_project_path: stored ID does not match its ordered child path")));
    lp_idmap_free(bindings); pfree(children); pfree(runs);
    bytea *encoded = palloc(VARHDRSZ + 37); SET_VARSIZE(encoded, VARHDRSZ + 37);
    if (lp_ewkb_coord(&result.c, (uint8 *)VARDATA(encoded), 37) != 37)
        ereport(ERROR, (errmsg("laplace_project_path: canonical coordinate encoding failed")));
    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_project_path requires its declared record result")));
    Datum values[2] = {PointerGetDatum(encoded), Int64GetDatum(lp_hilbert_key(lp_hilbert4(&result.c)))};
    bool nulls[2] = {false, false};
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}
