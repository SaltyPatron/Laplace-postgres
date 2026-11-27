/* Canonical projection from an immutable content path. SQL supplies complete
 * child-coordinate sets; Native owns IDs, run weights, centroids and Hilbert. */
#include "pg_core.h"
#include "geo.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "varatt.h"
#include "access/xlog.h"
#include "catalog/pg_extension.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "laplace/highway.h"
#include "laplace/tier0.h"
#include <math.h>

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

enum { RECOORD_HAS_PATH = 1u, RECOORD_FORCE_EDRIFT = 2u };

typedef struct {
    lp_id id;
    lp_coord c;
    int64 e_hilbert;
    ItemPointerData e_tid;
    uint32 first_edge, n_edges;
    uint16 part_idx;
    int16 tier;
    uint8 flags;
} RecoordNode;

typedef struct {
    uint32 idx;
    uint32 run;
} RecoordEdge;

typedef struct {
    uint32 node_idx;
    int64 hilbert;
    lp_coord c;
} RecoordEUpd;

typedef lp_vec(lp_ref) LpRefVec;

static uint32 recoord_extra_tier0(lp_idmap *extra_map, LpRefVec *extra_refs,
                                  const lp_tier0_record *atom, const lp_id *cid)
{
    bool fresh = false;
    size_t pos = lp_idmap_put(extra_map, cid, &fresh);
    uint32 *eidx = lp_idmap_at(extra_map, pos);
    if (fresh) {
        lp_ref r = {0};
        r.id = *cid;
        memcpy(r.c.m, atom->m, sizeof r.c.m);
        *eidx = (uint32)extra_refs->n;
        lp_push(extra_refs, r);
    }
    return 0x80000000u | *eidx;
}

static bool recoord_coord_ok(Datum coord_d, lp_coord *out_c)
{
    const uint8 *raw = (const uint8 *)PG_DETOAST_DATUM(coord_d);
    bool ext = (raw[7] & G2_EXTENDED) != 0;
    Geo eg = geo_of(PointerGetDatum(raw));
    if (eg.type != 1 || eg.p.n != 1)
        return false;
    const double *v = xyzm(&eg);
    for (int d = 0; d < 4; d++) {
        if (v[d] == 0.0 && signbit(v[d]))
            return false;
    }
    if (!lp_coord_of_xyzm(v, out_c) || !lp_coord_inside(out_c))
        return false;
    if (ext) {
        bytea *ewkb = lp_pg_path_export(PointerGetDatum(raw));
        bool len_ok = VARSIZE_ANY_EXHDR(ewkb) == 37;
        pfree(ewkb);
        if (!len_ok)
            return false;
    }
    return true;
}

static void recoord_update_partition(Oid part_oid, Snapshot snap,
                                     const RecoordNode *nodes,
                                     const RecoordEUpd *eu, size_t nupds)
{
    if (!nupds)
        return;
    Relation part = table_open(part_oid, NoLock);
    TupleDesc td = RelationGetDescr(part);
    AttrNumber a_hilbert = get_attnum(part_oid, "hilbert");
    AttrNumber a_coord = get_attnum(part_oid, "coord");
    if (a_hilbert <= 0 || a_coord <= 0)
        ereport(ERROR, (errmsg("laplace_recoordinate: canonical partition columns are missing")));

    EState *estate = CreateExecutorState();
    ResultRelInfo *rri = makeNode(ResultRelInfo);
    InitResultRelInfo(rri, part, 0, NULL, 0);
    ExecOpenIndices(rri, false);

    TupleTableSlot *old_slot = table_slot_create(part, NULL);
    TupleTableSlot *new_slot = table_slot_create(part, NULL);
    MemoryContext rowctx = AllocSetContextCreate(CurrentMemoryContext, "recoord row", ALLOCSET_DEFAULT_SIZES);
    CommandId cid = GetCurrentCommandId(true);

    for (size_t i = 0; i < nupds; i++) {
        if ((i & 1023u) == 0)
            CHECK_FOR_INTERRUPTS();
        MemoryContextReset(rowctx);
        MemoryContext oldctx = MemoryContextSwitchTo(rowctx);
        uint32 nidx = eu[i].node_idx;
        int64 new_hilbert = eu[i].hilbert;
        uint8 ewkb[64];
        if (lp_ewkb_coord(&eu[i].c, ewkb, sizeof ewkb) != 37)
            ereport(ERROR, (errmsg("laplace_recoordinate: canonical coordinate encoding failed")));
        Datum coord_val = lp_pg_geom_from_ewkb(ewkb, 37, 0);

        ItemPointerData tid = nodes[nidx].e_tid;
        ExecClearTuple(old_slot);
        if (!table_tuple_fetch_row_version(part, &tid, snap, old_slot)) {
            if (IsolationUsesXactSnapshot())
                ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                                errmsg("could not serialize access due to concurrent update")));
            MemoryContextSwitchTo(oldctx);
            continue;
        }
        for (;;) {
            slot_getallattrs(old_slot);
            ExecClearTuple(new_slot);
            for (int c = 0; c < td->natts; c++) {
                new_slot->tts_values[c] = old_slot->tts_values[c];
                new_slot->tts_isnull[c] = old_slot->tts_isnull[c];
            }
            new_slot->tts_values[a_hilbert - 1] = Int64GetDatum(new_hilbert);
            new_slot->tts_isnull[a_hilbert - 1] = false;
            new_slot->tts_values[a_coord - 1] = coord_val;
            new_slot->tts_isnull[a_coord - 1] = false;
            ExecStoreVirtualTuple(new_slot);

            TM_FailureData tmfd;
            LockTupleMode lockmode;
            TU_UpdateIndexes update_indexes = TU_None;
            TM_Result res = table_tuple_update(part, &tid, new_slot, cid, snap, InvalidSnapshot,
                                               true, &tmfd, &lockmode, &update_indexes);
            if (res == TM_Updated) {
                if (IsolationUsesXactSnapshot())
                    ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                                    errmsg("could not serialize access due to concurrent update")));
                ExecClearTuple(old_slot);
                if (table_tuple_lock(part, &tid, snap, old_slot, cid, lockmode, LockWaitBlock,
                                     TUPLE_LOCK_FLAG_FIND_LAST_VERSION, &tmfd) == TM_Ok)
                    continue;
                break;
            }
            if (res == TM_Ok && update_indexes != TU_None) {
                List *recheck = ExecInsertIndexTuples(rri, new_slot, estate, true, false, NULL, NIL,
                                                      update_indexes == TU_Summarizing);
                list_free(recheck);
            }
            break;
        }
        ExecClearTuple(old_slot);
        MemoryContextSwitchTo(oldctx);
    }

    MemoryContextDelete(rowctx);
    ExecDropSingleTupleTableSlot(new_slot);
    ExecDropSingleTupleTableSlot(old_slot);
    ExecCloseIndices(rri);
    FreeExecutorState(estate);
    table_close(part, NoLock);
}

PG_FUNCTION_INFO_V1(laplace_recoordinate);
Datum laplace_recoordinate(PG_FUNCTION_ARGS)
{
    LOCKTAG tag;
    SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 1279348803u, (uint32)-2, 2);
    (void)LockAcquire(&tag, ExclusiveLock, false, false);

    bool apply = !PG_ARGISNULL(0) && PG_GETARG_BOOL(0);
    LOCKMODE lmode = apply ? RowExclusiveLock : AccessShareLock;
    Snapshot snap = GetActiveSnapshot();
    const lp_tier0_record *floor = lp_pg_tier0();

    const Oid *eparts = NULL, *pparts = NULL;
    int neparts = lp_pg_canonical_partitions("entity", &eparts, NULL, NULL, NULL);
    int npparts = lp_pg_canonical_partitions("physicality", &pparts, NULL, NULL, NULL);
    if (neparts <= 0 || neparts != npparts)
        ereport(ERROR, (errmsg("laplace_recoordinate: canonical partition directories are inconsistent")));

    MemoryContext caller = CurrentMemoryContext;
    MemoryContext ctx = AllocSetContextCreate(caller, "laplace recoordinate", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(ctx);

    lp_idmap *by_id = lp_idmap_sized(sizeof(uint32));
    lp_idmap *extra_map = lp_idmap_sized(sizeof(uint32));
    LpRefVec extra_refs = {0};

    Size node_cap = 1024;
    size_t n_nodes = 0;
    RecoordNode *nodes = palloc_extended(node_cap * sizeof(RecoordNode), MCXT_ALLOC_HUGE);
    Size edge_cap = 2048;
    size_t n_edges = 0;
    RecoordEdge *edges = palloc_extended(edge_cap * sizeof(RecoordEdge), MCXT_ALLOC_HUGE);
    size_t tier_counts[256] = {0};

    int64 entity_rows = 0, physicality_rows = 0;

    PG_TRY(); {
        for (int p = 0; p < neparts; p++) {
            Relation part = table_open(eparts[p], lmode);
            AttrNumber a_id = get_attnum(eparts[p], "id");
            AttrNumber a_tier = get_attnum(eparts[p], "tier");
            AttrNumber a_coord = get_attnum(eparts[p], "coord");
            AttrNumber a_hilb = get_attnum(eparts[p], "hilbert");
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);

            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool nl = false;
                Datum d_id = slot_getattr(slot, a_id, &nl);
                if (nl) ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));
                lp_id id;
                memcpy(id.b, DatumGetPointer(d_id), sizeof id);

                Datum d_tier = slot_getattr(slot, a_tier, &nl);
                if (nl) ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));
                int16 tier = DatumGetInt16(d_tier);
                if (tier < 0 || tier > 255)
                    ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));

                Datum d_coord = slot_getattr(slot, a_coord, &nl);
                Datum d_hilb = slot_getattr(slot, a_hilb, &nl);
                if (nl) ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: null coordinate or hilbert")));

                if (n_nodes == node_cap) {
                    node_cap *= 2;
                    nodes = repalloc_huge(nodes, node_cap * sizeof(RecoordNode));
                }
                RecoordNode *node = &nodes[n_nodes];
                memset(node, 0, sizeof *node);
                node->id = id;
                node->tier = tier;
                node->part_idx = (uint16)p;
                node->e_hilbert = DatumGetInt64(d_hilb);
                node->e_tid = slot->tts_tid;
                if (!recoord_coord_ok(d_coord, &node->c))
                    node->flags |= RECOORD_FORCE_EDRIFT;

                bool fresh = false;
                size_t mpos = lp_idmap_put(by_id, &id, &fresh);
                if (!fresh)
                    ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: duplicate entity identity")));
                *(uint32 *)lp_idmap_at(by_id, mpos) = (uint32)n_nodes;
                tier_counts[tier]++;
                n_nodes++;
                ExecClearTuple(slot);
            }

            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }

        for (int p = 0; p < npparts; p++) {
            Relation part = table_open(pparts[p], lmode);
            AttrNumber a_ent = get_attnum(pparts[p], "entity");
            AttrNumber a_path = get_attnum(pparts[p], "path");
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);

            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool nl = false;
                Datum d_ent = slot_getattr(slot, a_ent, &nl);
                if (nl) ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));
                lp_id id;
                memcpy(id.b, DatumGetPointer(d_ent), sizeof id);

                const uint32 *slot_idx = lp_idmap_lookup(by_id, &id);
                if (!slot_idx)
                    ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));
                RecoordNode *node = &nodes[*slot_idx];
                if (node->flags & RECOORD_HAS_PATH)
                    ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));

                Datum d_path = slot_getattr(slot, a_path, &nl);
                if (nl) ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_recoordinate: null path")));

                node->flags |= RECOORD_HAS_PATH;

                Geo g = geo_of(d_path);
                if (node->tier == 0) {
                    const lp_tier0_record *atom = lp_tier0_find(floor, &id);
                    lp_id v0 = g.p.n == 1 ? lp_path_id(g.p, 0) : (lp_id){{0}};
                    if (!atom || g.type != 1 || g.p.n != 1 || lp_path_len(g.p) != 1 || !lp_id_eq(&v0, &id))
                        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                                        errmsg("laplace_recoordinate: tier0 row is not a canonical codepoint self-path")));
                }

                if (g.p.n > PG_UINT32_MAX)
                    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_recoordinate: path too long")));
                node->first_edge = (uint32)n_edges;
                node->n_edges = (uint32)g.p.n;
                if (n_edges + g.p.n > edge_cap) {
                    while (edge_cap < n_edges + g.p.n) edge_cap *= 2;
                    edges = repalloc_huge(edges, edge_cap * sizeof(RecoordEdge));
                }

                for (size_t i = 0; i < g.p.n; i++) {
                    lp_id cid = lp_path_id(g.p, i);
                    uint32 run = lp_path_run(g.p, i);
                    const uint32 *cidx = lp_idmap_lookup(by_id, &cid);
                    if (cidx) {
                        if (nodes[*cidx].tier >= node->tier && !(node->tier == 0 && lp_id_eq(&cid, &id)))
                            ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                                            errmsg("laplace_recoordinate: path violates strict child-to-parent tier ordering")));
                        edges[n_edges++] = (RecoordEdge){.idx = *cidx, .run = run};
                    } else {
                        const lp_tier0_record *atom = lp_tier0_find(floor, &cid);
                        if (!atom)
                            ereport(ERROR, (errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
                                            errmsg("laplace_project_path: missing non-Tier0 child")));
                        edges[n_edges++] = (RecoordEdge){.idx = recoord_extra_tier0(extra_map, &extra_refs, atom, &cid), .run = run};
                    }
                }
                ExecClearTuple(slot);
            }

            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }

        for (size_t i = 0; i < n_nodes; i++) {
            if (!(nodes[i].flags & RECOORD_HAS_PATH))
                ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                                errmsg("laplace_recoordinate: unpaired entity/path or invalid tier")));
        }

        size_t tier_off[256];
        size_t running = 0;
        for (int t = 0; t < 256; t++) {
            tier_off[t] = running;
            running += tier_counts[t];
        }
        uint32 *order = n_nodes ? palloc_extended(n_nodes * sizeof(uint32), MCXT_ALLOC_HUGE) : NULL;
        for (size_t i = 0; i < n_nodes; i++)
            order[tier_off[nodes[i].tier]++] = (uint32)i;

        typedef lp_vec(RecoordEUpd) RecoordEUpdVec;
        RecoordEUpdVec *e_upds = apply ? palloc0((size_t)neparts * sizeof(RecoordEUpdVec)) : NULL;

        size_t child_cap = 64;
        lp_ref *children = palloc(child_cap * sizeof(lp_ref));
        uint64 *runs = palloc(child_cap * sizeof(uint64));

        for (size_t o = 0; o < n_nodes; o++) {
            if ((o & 4095u) == 0)
                CHECK_FOR_INTERRUPTS();
            uint32 idx = order[o];
            RecoordNode *node = &nodes[idx];
            if (node->n_edges > child_cap) {
                while (child_cap < node->n_edges) child_cap *= 2;
                children = repalloc(children, child_cap * sizeof(lp_ref));
                runs = repalloc(runs, child_cap * sizeof(uint64));
            }
            for (uint32 k = 0; k < node->n_edges; k++) {
                RecoordEdge e = edges[node->first_edge + k];
                if (e.idx & 0x80000000u) {
                    children[k] = extra_refs.v[e.idx & 0x7fffffffu];
                } else {
                    const RecoordNode *cn = &nodes[e.idx];
                    memset(&children[k], 0, sizeof children[k]);
                    children[k].id = cn->id;
                    const lp_tier0_record *atom = cn->tier == 0 ? lp_tier0_find(floor, &cn->id) : NULL;
                    if (atom) memcpy(children[k].c.m, atom->m, sizeof children[k].c.m);
                    else children[k].c = cn->c;
                }
                runs[k] = e.run;
            }
            lp_ref res;
            if (!lp_ref_compose_runs(children, runs, node->n_edges, 0, &res) || !lp_id_eq(&node->id, &res.id))
                ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                                errmsg("laplace_project_path: stored ID does not match its ordered child path")));

            int64 proj_hilbert = lp_hilbert_key(lp_hilbert4(&res.c));
            bool e_drift = (node->flags & RECOORD_FORCE_EDRIFT) != 0
                || memcmp(node->c.m, res.c.m, sizeof res.c.m) != 0
                || node->e_hilbert != proj_hilbert;

            node->c = res.c;
            if (e_drift) {
                entity_rows++;
                if (apply) {
                    RecoordEUpd u = {.node_idx = idx, .hilbert = proj_hilbert, .c = res.c};
                    lp_push(&e_upds[node->part_idx], u);
                }
            }
        }

        if (apply) {
            for (int p = 0; p < neparts; p++)
                recoord_update_partition(eparts[p], snap, nodes, e_upds[p].v, e_upds[p].n);
        }
    } PG_FINALLY(); {
        lp_idmap_free(by_id);
        lp_idmap_free(extra_map);
        lp_vec_free(&extra_refs);
        MemoryContextSwitchTo(caller);
        MemoryContextDelete(ctx);
    } PG_END_TRY();

    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_recoordinate requires its declared record result")));
    Datum values[2] = {Int64GetDatum(entity_rows), Int64GetDatum(physicality_rows)};
    bool nulls[2] = {false, false};
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

typedef struct {
    lp_id id;
    int16 tier;
} AuditEntity;

typedef struct {
    AuditEntity *v;
    size_t n;
    size_t cap;
} AuditBucket;

static int cmp_audit_entity(const void *a, const void *b)
{
    return memcmp(((const AuditEntity *)a)->id.b, ((const AuditEntity *)b)->id.b, sizeof(lp_id));
}

static int cmp_audit_key(const void *key, const void *elem)
{
    return memcmp(((const lp_id *)key)->b, ((const AuditEntity *)elem)->id.b, sizeof(lp_id));
}

static void audit_jb_key(JsonbParseState **st, const char *k)
{
    JsonbValue v = { .type = jbvString };
    v.val.string.len = (int)strlen(k);
    v.val.string.val = pstrdup(k);
    pushJsonbValue(st, WJB_KEY, &v);
}

static void audit_jb_str(JsonbParseState **st, const char *k, const char *s)
{
    audit_jb_key(st, k);
    if (!s) {
        JsonbValue n = { .type = jbvNull };
        pushJsonbValue(st, WJB_VALUE, &n);
        return;
    }
    JsonbValue v = { .type = jbvString };
    v.val.string.len = (int)strlen(s);
    v.val.string.val = pstrdup(s);
    pushJsonbValue(st, WJB_VALUE, &v);
}

static void audit_jb_i64(JsonbParseState **st, const char *k, int64 val)
{
    audit_jb_key(st, k);
    JsonbValue v = { .type = jbvNumeric };
    v.val.numeric = int64_to_numeric(val);
    pushJsonbValue(st, WJB_VALUE, &v);
}

typedef lp_vec(lp_id) AuditWitnessVec;

static int cmp_witness_id(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(lp_id));
}

static void audit_collect_witness(const lp_id *id, const lp_id *lineage, double trust, void *ctx)
{
    (void)lineage; (void)trust;
    AuditWitnessVec *out = ctx;
    lp_push(out, *id);
}

static char *hex_id_str(const lp_id *id)
{
    static const char hx[] = "0123456789abcdef";
    char *s = palloc(33);
    for (int i = 0; i < 16; i++) {
        s[2 * i] = hx[id->b[i] >> 4];
        s[2 * i + 1] = hx[id->b[i] & 15];
    }
    s[32] = '\0';
    return s;
}

static char *hex32_str(const uint8 fp[32])
{
    static const char hx[] = "0123456789abcdef";
    char *s = palloc(65);
    for (int i = 0; i < 32; i++) {
        s[2 * i] = hx[fp[i] >> 4];
        s[2 * i + 1] = hx[fp[i] & 15];
    }
    s[64] = '\0';
    return s;
}

static int16 audit_find_entity_tier(const AuditBucket buckets[256], int nparts, const lp_id *id)
{
    (void)nparts;
    int b = (int)id->b[0];
    const AuditBucket *bk = &buckets[b];
    if (!bk->n) return -1;
    const AuditEntity *hit = bsearch(id, bk->v, bk->n, sizeof(AuditEntity), cmp_audit_key);
    return hit ? hit->tier : -1;
}

PG_FUNCTION_INFO_V1(laplace_audit_admitted);
Datum laplace_audit_admitted(PG_FUNCTION_ARGS)
{
    char *probe = (PG_NARGS() > 0 && !PG_ARGISNULL(0)) ? text_to_cstring(PG_GETARG_TEXT_PP(0)) : pstrdup("mississippi");

    const lp_tier0_record *t0 = lp_pg_tier0();
    uint8 t0_fp[32];
    lp_tier0_fingerprint(t0, t0_fp);
    char *t0_hex = hex32_str(t0_fp);

    char *hw_hex = NULL;
    const char *hw_opt = GetConfigOption("laplace.highway", true, false);
    if ((hw_opt && *hw_opt) || getenv("LAPLACE_HIGHWAY")) {
        uint8 hw_fp[32];
        lp_highway_fingerprint(lp_pg_highway(), hw_fp);
        hw_hex = hex32_str(hw_fp);
    }

    lp_ref probe_ref = *probe ? trunk_of(cstring_to_text(probe), NULL, 0, NULL) : (lp_ref){0};
    char *probe_id_hex = hex_id_str(&probe_ref.id);

    TimestampTz now = GetCurrentTimestamp();
    struct pg_tm tm;
    fsec_t fsec = 0;
    char captured_at[64] = "";
    if (timestamp2tm(now, NULL, &tm, &fsec, NULL, NULL) == 0) {
        snprintf(captured_at, sizeof captured_at, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
                 tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)fsec);
    }

    char *dbname = get_database_name(MyDatabaseId);
    const char *server_ver = PG_VERSION_STR;
    int64 db_bytes = DatumGetInt64(DirectFunctionCall1(pg_database_size_oid, ObjectIdGetDatum(MyDatabaseId)));

    char *extversion = NULL;
    Oid ext_oid = get_extension_oid("laplace", true);
    if (OidIsValid(ext_oid)) {
        HeapTuple etup = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext_oid));
        if (HeapTupleIsValid(etup)) {
            bool isnull = true;
            Datum d = SysCacheGetAttr(EXTENSIONOID, etup, 6, &isnull);
            if (!isnull) extversion = TextDatumGetCString(d);
            ReleaseSysCache(etup);
        }
    }

    Oid ent_oid = lp_pg_relation("entity");
    Oid path_oid = lp_pg_relation("physicality");
    AttrNumber a_eid = get_attnum(ent_oid, "id");
    AttrNumber a_etier = get_attnum(ent_oid, "tier");
    AttrNumber a_pent = get_attnum(path_oid, "entity");
    AttrNumber a_ptier = get_attnum(path_oid, "tier");
    AttrNumber a_ppath = get_attnum(path_oid, "path");

    const Oid *ent_parts = NULL, *path_parts = NULL;
    int n_ent = lp_pg_canonical_partitions("entity", &ent_parts, NULL, NULL, NULL);
    int n_path = lp_pg_canonical_partitions("physicality", &path_parts, NULL, NULL, NULL);
    Snapshot snap = GetActiveSnapshot();

    MemoryContext caller = CurrentMemoryContext;
    MemoryContext audit_ctx = AllocSetContextCreate(caller, "laplace_audit_admitted", ALLOCSET_DEFAULT_SIZES);
    AuditBucket buckets[256] = {0};

    int64 entities = 0;
    int64 max_tier = 0;
    int64 tier_counts[256] = {0};
    int64 total_edges = 0;
    int64 height_violations = 0;
    int64 missing_entity_rows = 0;
    int64 virtual_codepoints = 0;
    int64 unresolved_references = 0;
    int64 paths = 0;
    int64 paths_without_entity = 0;
    int64 path_height_mismatches = 0;
    lp_idmap *missing_ids = lp_idmap_new();
    AuditWitnessVec witness_ids = {0};
    char **witness_hexes = NULL;
    size_t n_witness = 0;

    PG_TRY();
    {
        MemoryContextSwitchTo(audit_ctx);

        for (int p = 0; p < n_ent; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(ent_parts[p], AccessShareLock);
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool ni = true, nt = true;
                Datum di = slot_getattr(slot, a_eid, &ni);
                Datum dt = slot_getattr(slot, a_etier, &nt);
                if (!ni && !nt) {
                    const lp_id *id = (const lp_id *)DatumGetPointer(di);
                    int16 tier = DatumGetInt16(dt);
                    int b = (int)id->b[0];
                    AuditBucket *bk = &buckets[b];
                    if (bk->n == bk->cap) {
                        size_t ncap = bk->cap ? bk->cap * 2 : 1024;
                        bk->v = bk->v ? repalloc_huge(bk->v, ncap * sizeof(AuditEntity))
                                      : palloc_extended(ncap * sizeof(AuditEntity), MCXT_ALLOC_HUGE);
                        bk->cap = ncap;
                    }
                    bk->v[bk->n++] = (AuditEntity){ *id, tier };
                    entities++;
                    if (tier > max_tier) max_tier = tier;
                    if (tier >= 0 && tier <= 255) tier_counts[tier]++;
                }
                ExecClearTuple(slot);
            }
            table_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
            table_close(part, NoLock);
        }

        for (int b = 0; b < 256; b++) {
            if (buckets[b].n > 1)
                lp_sort(buckets[b].v, buckets[b].n, sizeof(AuditEntity), cmp_audit_entity);
        }

        MemoryContext row_ctx = AllocSetContextCreate(audit_ctx, "audit path row", ALLOCSET_DEFAULT_SIZES);
        for (int p = 0; p < n_path; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(path_parts[p], AccessShareLock);
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool ne = true, nt = true, np = true;
                Datum de = slot_getattr(slot, a_pent, &ne);
                Datum dt = slot_getattr(slot, a_ptier, &nt);
                Datum dp = slot_getattr(slot, a_ppath, &np);
                if (!ne && !nt && !np) {
                    paths++;
                    const lp_id *pid = (const lp_id *)DatumGetPointer(de);
                    int16 ptier = DatumGetInt16(dt);
                    int16 etier = audit_find_entity_tier(buckets, n_ent, pid);
                    if (etier < 0) paths_without_entity++;
                    else if (ptier != etier) path_height_mismatches++;

                    MemoryContextSwitchTo(row_ctx);
                    int nv = 0;
                    lp_id *vids = lp_pg_path_keys(lp_pg_path_ewkb(dp), &nv);
                    for (int i = 0; i < nv; i++) {
                        int16 ctier = audit_find_entity_tier(buckets, n_ent, &vids[i]);
                        if (ptier > 0) {
                            total_edges++;
                            if (ctier >= 0 && ctier >= ptier) height_violations++;
                        }
                        if (ctier < 0)
                            lp_idmap_put(missing_ids, &vids[i], NULL);
                    }
                    MemoryContextSwitchTo(audit_ctx);
                    MemoryContextReset(row_ctx);
                }
                ExecClearTuple(slot);
            }
            table_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
            table_close(part, NoLock);
        }

        missing_entity_rows = (int64)lp_idmap_count(missing_ids);
        for (size_t i = 0; i < (size_t)missing_entity_rows; i++) {
            const lp_id *mid = lp_idmap_key(missing_ids, i);
            if (lp_tier0_codepoint(t0, mid) >= 0) virtual_codepoints++;
            else unresolved_references++;
        }

        lp_pg_scan_all_witnesses(audit_collect_witness, &witness_ids);
        if (witness_ids.n > 1) {
            lp_sort(witness_ids.v, witness_ids.n, sizeof(lp_id), cmp_witness_id);
            size_t uq = 1;
            for (size_t i = 1; i < witness_ids.n; i++) {
                if (!lp_id_eq(&witness_ids.v[i], &witness_ids.v[uq - 1]))
                    witness_ids.v[uq++] = witness_ids.v[i];
            }
            witness_ids.n = uq;
        }
        MemoryContextSwitchTo(caller);
        if (witness_ids.n > 0) {
            n_witness = witness_ids.n;
            witness_hexes = palloc(sizeof(char *) * n_witness);
            for (size_t i = 0; i < n_witness; i++)
                witness_hexes[i] = hex_id_str(&witness_ids.v[i]);
        }
    }
    PG_FINALLY();
    {
        lp_idmap_free(missing_ids);
        lp_vec_free(&witness_ids);
        MemoryContextSwitchTo(caller);
        MemoryContextDelete(audit_ctx);
    }
    PG_END_TRY();

    JsonbParseState *st = NULL;
    pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
    audit_jb_str(&st, "captured_at", captured_at);
    audit_jb_str(&st, "database", dbname);
    audit_jb_str(&st, "server", server_ver);
    audit_jb_str(&st, "extension", extversion);
    audit_jb_str(&st, "tier0", t0_hex);
    audit_jb_str(&st, "highway", hw_hex);

    audit_jb_key(&st, "canonical_text_probe");
    pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
    audit_jb_str(&st, "id", probe_id_hex);
    audit_jb_i64(&st, "tier", (int64)probe_ref.tier);
    pushJsonbValue(&st, WJB_END_OBJECT, NULL);

    audit_jb_i64(&st, "database_bytes", db_bytes);
    audit_jb_i64(&st, "entities", entities);
    if (entities == 0) {
        audit_jb_key(&st, "maximum_tier");
        JsonbValue nv = {.type = jbvNull};
        pushJsonbValue(&st, WJB_VALUE, &nv);
    } else {
        audit_jb_i64(&st, "maximum_tier", max_tier);
    }

    audit_jb_key(&st, "tiers");
    pushJsonbValue(&st, WJB_BEGIN_ARRAY, NULL);
    for (int t = 0; t <= 255; t++) {
        if (!tier_counts[t]) continue;
        pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
        audit_jb_i64(&st, "tier", (int64)t);
        audit_jb_i64(&st, "entities", tier_counts[t]);
        pushJsonbValue(&st, WJB_END_OBJECT, NULL);
    }
    pushJsonbValue(&st, WJB_END_ARRAY, NULL);

    audit_jb_key(&st, "edges");
    pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
    audit_jb_i64(&st, "edges", total_edges);
    audit_jb_i64(&st, "height_violations", height_violations);
    pushJsonbValue(&st, WJB_END_OBJECT, NULL);

    audit_jb_key(&st, "closure");
    pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
    audit_jb_i64(&st, "missing_entity_rows", missing_entity_rows);
    audit_jb_i64(&st, "virtual_codepoints", virtual_codepoints);
    audit_jb_i64(&st, "unresolved_references", unresolved_references);
    pushJsonbValue(&st, WJB_END_OBJECT, NULL);

    audit_jb_key(&st, "physicality");
    pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
    audit_jb_i64(&st, "paths", paths);
    audit_jb_i64(&st, "paths_without_entity", paths_without_entity);
    audit_jb_i64(&st, "path_height_mismatches", path_height_mismatches);
    pushJsonbValue(&st, WJB_END_OBJECT, NULL);

    audit_jb_key(&st, "witnesses");
    pushJsonbValue(&st, WJB_BEGIN_ARRAY, NULL);
    for (size_t i = 0; i < n_witness; i++) {
        JsonbValue wv = {
            .type = jbvString,
            .val.string.len = 32,
            .val.string.val = witness_hexes[i]
        };
        pushJsonbValue(&st, WJB_ELEM, &wv);
    }
    pushJsonbValue(&st, WJB_END_ARRAY, NULL);
    JsonbValue *root = pushJsonbValue(&st, WJB_END_OBJECT, NULL);
    PG_RETURN_JSONB_P(JsonbValueToJsonb(root));
}
