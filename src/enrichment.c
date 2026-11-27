/* Stage presence, carrier compatibility, load leases, staging merge,
 * partition-directed metadata enrichment, and GIN pending-list maintenance. */
#include "pg_core.h"
#include "geo.h"
#include "laplace/highway.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_description.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type_d.h"
#include "commands/comment.h"
#include "commands/extension.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "partitioning/partdesc.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>

typedef struct {
    Oid partition;
    lp_id id;
    int64 index;
} EnrichmentKey;

void lp_pg_truncate_rel(Oid rel1, Oid rel2)
{
    TruncateStmt *stmt = makeNode(TruncateStmt);
    stmt->relations = list_make1(makeRangeVar(get_namespace_name(get_rel_namespace(rel1)), get_rel_name(rel1), -1));
    if (OidIsValid(rel2))
        stmt->relations = lappend(stmt->relations, makeRangeVar(get_namespace_name(get_rel_namespace(rel2)), get_rel_name(rel2), -1));
    stmt->restart_seqs = false;
    stmt->behavior = DROP_RESTRICT;
    ExecuteTruncate(stmt);
}

static long enrichment_fetch_rows(void)
{
    Size tuple = MAXALIGN(sizeof(HeapTupleData)) + MAXALIGN(SizeofHeapTupleHeader)
        + sizeof(HeapTuple) + MAXALIGN(sizeof(Oid) + sizeof(lp_id) + sizeof(ItemPointerData));
    Size live = Max(tuple + sizeof(EnrichmentKey),
                    sizeof(EnrichmentKey) + sizeof(Datum) + 3 * (sizeof(lp_id) + sizeof(ItemPointerData)));
    uint64 budget = (uint64)work_mem * 1024;
    Size count = Max((uint64)1, budget / live);
    count = Min(count, MaxAllocSize / sizeof(EnrichmentKey));
    count = Min(count, MaxAllocSize / sizeof(Datum));
    count = Min(count, (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id));
    return (long)Min(count, (Size)INT_MAX);
}

static int lp_oid_cmp(const void *a, const void *b)
{
    Oid x = *(const Oid *)a, y = *(const Oid *)b;
    return (x > y) - (x < y);
}

static bool stage_is_private_unlogged(Relation rel, bool allow_temp)
{
    if (rel->rd_rel->relkind != RELKIND_RELATION) return false;
    Oid ns = RelationGetNamespace(rel);
    Oid stage_ns = get_namespace_oid("stage", true);
    if (OidIsValid(stage_ns) && ns == stage_ns && rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
        return true;
    if (allow_temp && isTempNamespace(ns) && rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
        return true;
    return false;
}

static bool stage_name_matches_token(const char *relname, int64 token)
{
    if (!relname || strncmp(relname, "lp_", 3) != 0) return false;
    const char *p = relname + 3;
    if (*p < 'a' || *p > 'z') return false;
    char tokbuf[48];
    int toklen = snprintf(tokbuf, sizeof(tokbuf), "_%lld", (long long)token);
    if (toklen <= 1 || toklen >= (int)sizeof(tokbuf)) return false;
    size_t len = strlen(relname);
    const char *end = relname + len;
    const char *scan_end = end;
    /* Strip optional _[0-9]+ lane suffix before checking _<token>. */
    const char *q = end;
    while (q > p && isdigit((unsigned char)q[-1])) q--;
    if (q < end && q > p + 1 && q[-1] == '_')
        scan_end = q - 1;
    if ((size_t)(scan_end - (p + 1)) >= (size_t)toklen &&
        memcmp(scan_end - toklen, tokbuf, (size_t)toklen) == 0) {
        const char *mid_end = scan_end - toklen;
        for (const char *c = p + 1; c < mid_end; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
                goto try_unsuffixed;
        return true;
    }
try_unsuffixed:
    if (scan_end != end && (size_t)(end - (p + 1)) >= (size_t)toklen &&
        memcmp(end - toklen, tokbuf, (size_t)toklen) == 0) {
        const char *mid_end = end - toklen;
        for (const char *c = p + 1; c < mid_end; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
                return false;
        return true;
    }
    return false;
}

static bool parse_stage_comment(const char *desc, int32 *owner_pid, int64 *load_token)
{
    static const char prefix[] = "laplace:stage-load:1:";
    size_t plen = sizeof(prefix) - 1;
    if (!desc || strncmp(desc, prefix, plen) != 0) return false;
    const char *p = desc + plen;
    if (*p < '1' || *p > '9') return false;
    int64 pid = 0;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p++ - '0');
        if (pid > PG_INT32_MAX) return false;
    }
    if (*p != ':') return false;
    p++;
    if (*p < '1' || *p > '9') return false;
    int64 tok = 0;
    while (*p >= '0' && *p <= '9') {
        if (tok > (PG_INT64_MAX - 9) / 10) return false;
        tok = tok * 10 + (*p++ - '0');
    }
    if (*p != '\0') return false;
    *owner_pid = (int32)pid;
    *load_token = tok;
    return true;
}

/* ---------------------------------------------------------------- stage presence & compatibility */
typedef struct {
    FunctionCallInfo call;
    Tuplestorestate *rows;
    TupleDesc descriptor;
    Relation parent;
    const char *column;
    PresenceKey *keys;
    int used, capacity;
    int64 ordinal;
} PresenceBatch;

static void presence_flush(PresenceBatch *batch)
{
    if (!batch->used) return;
    lp_pg_presence_keys(batch->call, batch->rows, batch->descriptor, batch->parent, batch->column, batch->keys, batch->used);
    batch->used = 0;
}

PG_FUNCTION_INFO_V1(laplace_present_stage);
Datum laplace_present_stage(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;
    Tuplestorestate *output = set_begin(fcinfo, "laplace_present_stage", &descriptor);
    Oid input = PG_GETARG_OID(0), target = PG_GETARG_OID(1);
    const char *id_column = NameStr(*PG_GETARG_NAME(2));
    const char *present_column = NameStr(*PG_GETARG_NAME(3));
    Relation stage = table_open(input, ShareLock);
    AttrNumber a_id = InvalidAttrNumber, a_pres = InvalidAttrNumber;
    if (stage_is_private_unlogged(stage, true)) {
        a_id = get_attnum(input, id_column);
        a_pres = get_attnum(input, present_column);
    }
    if (!stage_is_private_unlogged(stage, true) || a_pres == InvalidAttrNumber ||
        get_atttype(input, a_pres) != BOOLOID || a_id == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_present_stage requires a private stage ID set and boolean presence column")));
    const char *column;
    Relation parent = lp_pg_presence_parent(target, &column, 1);
    if (get_atttype(input, a_id) != get_atttype(target, get_attnum(target, column)))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_present_stage requires Native content IDs")));

    int capacity = (int)enrichment_fetch_rows();
    PresenceBatch batch = {fcinfo, output, descriptor, parent, column, palloc(sizeof(PresenceKey) * (Size)capacity), 0, capacity, 0};
    TableScanDesc scan = table_beginscan(stage, GetActiveSnapshot(), 0, NULL);
    TupleTableSlot *slot = table_slot_create(stage, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool null_p = false, null_id = false;
        Datum pres = slot_getattr(slot, a_pres, &null_p);
        if (!null_p && DatumGetBool(pres)) { ExecClearTuple(slot); continue; }
        Datum id = slot_getattr(slot, a_id, &null_id);
        if (null_id)
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_present_stage: null Native ID")));
        batch.keys[batch.used++] = (PresenceKey){.id = *(const lp_id *)DatumGetPointer(id), .ordinal = ++batch.ordinal};
        if (batch.used == batch.capacity) presence_flush(&batch);
        ExecClearTuple(slot);
    }
    presence_flush(&batch);
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    pfree(batch.keys);
    table_close(parent, NoLock);
    table_close(stage, NoLock);
    PG_RETURN_NULL();
}

static bool geom_fast_equal(bool is_paths, Datum a, Datum b)
{
    Geo ga = geo_of(a), gb = geo_of(b);
    if (ga.type != gb.type || ga.p.n != gb.p.n) return false;
    return memcmp(ga.p.v, gb.p.v, ga.p.n * (size_t)32) == 0;
}

static void check_carrier_compatible(bool is_paths, const lp_id *did, const lp_id *sid,
                                     Datum d_carrier, Datum s_carrier, bool s_carrier_null,
                                     int64 d_hilbert, bool d_hilbert_null,
                                     int64 s_hilbert, bool s_hilbert_null)
{
    if (!lp_id_eq(did, sid)) return;
    if (!s_hilbert_null && (d_hilbert_null || d_hilbert != s_hilbert))
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_merge_stage: conflicting identity Hilbert key")));
    if (s_carrier_null) return;
    if (geom_fast_equal(is_paths, d_carrier, s_carrier)) return;
    if (!is_paths)
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_merge_stage: conflicting identity data or incompatible path annotations")));
    bytea *a = lp_pg_path_export(d_carrier), *b = lp_pg_path_export(s_carrier);
    bool valid = (VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b) &&
                  !memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a))) ||
                 lp_ewkb_enrich_path(lp_path_of((const uint8 *)VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a)),
                                     lp_path_of((const uint8 *)VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b)), NULL, 0);
    pfree(a);
    pfree(b);
    if (!valid)
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_merge_stage: conflicting identity data or incompatible path annotations")));
}

PG_FUNCTION_INFO_V1(laplace_stage_compatible);
Datum laplace_stage_compatible(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) PG_RETURN_BOOL(false);
    if (!lp_id_eq((const lp_id *)PG_GETARG_POINTER(0), (const lp_id *)PG_GETARG_POINTER(1))) PG_RETURN_BOOL(true);
    if (!PG_ARGISNULL(5) && (PG_ARGISNULL(4) || PG_GETARG_INT64(4) != PG_GETARG_INT64(5)))
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_merge_stage: conflicting identity Hilbert key")));
    if (PG_ARGISNULL(3)) PG_RETURN_BOOL(true);
    if (PG_ARGISNULL(2) || PG_ARGISNULL(6))
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_merge_stage: invalid carrier comparison")));
    FmgrInfo *send = fcinfo->flinfo->fn_extra;
    if (!send) {
        Oid function; bool variable;
        getTypeBinaryOutputInfo(get_fn_expr_argtype(fcinfo->flinfo, 2), &function, &variable);
        send = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof *send);
        fmgr_info_cxt(function, send, fcinfo->flinfo->fn_mcxt);
        fcinfo->flinfo->fn_extra = send;
    }
    bytea *a = SendFunctionCall(send, PG_GETARG_DATUM(2)), *b = SendFunctionCall(send, PG_GETARG_DATUM(3));
    bool equal = VARSIZE_ANY_EXHDR(a) == VARSIZE_ANY_EXHDR(b)
        && !memcmp(VARDATA_ANY(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(a));
    bool valid = equal || (PG_GETARG_BOOL(6) && lp_ewkb_enrich_path(
        lp_path_of((const uint8 *)VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a)),
        lp_path_of((const uint8 *)VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b)), NULL, 0));
    pfree(a); pfree(b);
    if (!valid)
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_merge_stage: conflicting identity data or incompatible path annotations")));
    PG_RETURN_BOOL(true);
}

/* ---------------------------------------------------------------- ACL helpers */
static void require_table_or_col_acl(Oid relid, AclMode mode, const char *const *cols, int ncols)
{
    Oid uid = GetUserId();
    if (pg_class_aclcheck(relid, uid, mode) == ACLCHECK_OK) return;
    char *relname = get_rel_name(relid);
    for (int i = 0; i < ncols; i++) {
        AttrNumber att = get_attnum(relid, cols[i]);
        if (att == InvalidAttrNumber || pg_attribute_aclcheck(relid, att, uid, mode) != ACLCHECK_OK)
            aclcheck_error_col(ACLCHECK_NO_PRIV, OBJECT_TABLE, relname ? relname : "relation", cols[i]);
    }
    if (relname) pfree(relname);
}

static void require_enrichment_acls(Oid target, bool paths, bool check_compat)
{
    if (check_enable_rls(target, InvalidOid, false) == RLS_ENABLED)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("laplace_enrich_stage does not support RLS on canonical tables")));
    if (paths) {
        const char *base_sel[4] = {"entity", "tier", "mask", "path"};
        require_table_or_col_acl(target, ACL_SELECT, base_sel, check_compat ? 4 : 3);
    } else {
        const char *base_sel[5] = {"id", "tier", "mask", "coord", "hilbert"};
        require_table_or_col_acl(target, ACL_SELECT, base_sel, check_compat ? 5 : 3);
    }
    const char *upd_cols[2] = {"tier", "mask"};
    require_table_or_col_acl(target, ACL_UPDATE, upd_cols, 2);
}

/* ---------------------------------------------------------------- enrichment kernel */
typedef struct {
    lp_id id;
    Oid partition;
    int16 tier;
    lp_mask mask;
    bool carrier_null, hilbert_null;
    Datum carrier;
    int64 hilbert;
} StageEnrichRow;

static int stage_enrich_cmp(const void *a, const void *b)
{
    const StageEnrichRow *x = a, *y = b;
    int c = (x->partition > y->partition) - (x->partition < y->partition);
    return c ? c : memcmp(x->id.b, y->id.b, sizeof(lp_id));
}

static Oid find_partition_pk(int nparts, const Oid *parts, const Oid *pks, Relation part, Oid partOid)
{
    for (int p = 0; p < nparts; p++)
        if (parts[p] == partOid) return pks[p];
    Oid pk = RelationGetPrimaryKeyIndex(part, false);
    if (!OidIsValid(pk)) ereport(ERROR, (errmsg("Native enrichment: partition %u has no primary index", partOid)));
    return pk;
}

static bool canonical_is_by_tier(Oid target, PartitionKey pkey)
{
    AttrNumber a_tier = get_attnum(target, "tier");
    return pkey && pkey->strategy == PARTITION_STRATEGY_RANGE &&
           pkey->partnatts == 1 && a_tier != InvalidAttrNumber &&
           pkey->partattrs[0] == a_tier;
}

static Oid route_canonical_partition(Oid target, PartitionKey pkey, PartitionDesc pdesc,
                                     const lp_id *id, int16 tier)
{
    if (canonical_is_by_tier(target, pkey))
        return lp_pg_enrichment_partition(target, pkey, pdesc, Int16GetDatum(tier));
    return lp_pg_enrichment_partition(target, pkey, pdesc, PointerGetDatum(id));
}

static uint64 enrich_partition_rows(Oid partOid, Oid pkOid, Oid idtype, bool paths, bool check_compat,
                                    bool update_path_acl_checked, Oid parent_target,
                                    const StageEnrichRow *rows, size_t n, Snapshot snap, bool *touched,
                                    size_t *matched_unique)
{
    const lp_highway *hw = lp_pg_highway();
    Relation part = table_open(partOid, RowExclusiveLock);
    Relation idx = index_open(pkOid, AccessShareLock);
    AttrNumber a_id = get_attnum(partOid, paths ? "entity" : "id");
    AttrNumber a_tier = get_attnum(partOid, "tier");
    AttrNumber a_mask = get_attnum(partOid, "mask");
    AttrNumber a_hilbert = paths ? InvalidAttrNumber : get_attnum(partOid, "hilbert");
    AttrNumber a_carrier = get_attnum(partOid, paths ? "path" : "coord");

    Datum *vals = palloc((Size)n * sizeof(Datum));
    for (size_t i = 0; i < n; i++) vals[i] = PointerGetDatum(&rows[i].id);
    ArrayType *wanted = construct_array(vals, (int)n, idtype, sizeof(lp_id), false, TYPALIGN_CHAR);
    pfree(vals);

    Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], idtype, idtype, BTEqualStrategyNumber);
    ScanKeyData sk;
    ScanKeyEntryInitialize(&sk, SK_SEARCHARRAY, 1, BTEqualStrategyNumber, idtype,
        idx->rd_indcollation[0], get_opcode(eq_op), PointerGetDatum(wanted));
    IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 1, 0);
    index_rescan(scan, &sk, 1, NULL, 0);

    TupleDesc part_td = RelationGetDescr(part);
    TupleTableSlot *old_slot = table_slot_create(part, NULL);
    TupleTableSlot *new_slot = table_slot_create(part, NULL);
    EState *estate = NULL;
    ResultRelInfo *rri = NULL;
    uint64 updated = 0;
    size_t matched = 0;
    size_t at = 0;
    CommandId cid = GetCurrentCommandId(true);
    bool path_acl_ok = update_path_acl_checked || !paths;
    bool checked_tier_partition = false, is_by_tier = false;

    while (index_getnext_slot(scan, ForwardScanDirection, old_slot)) {
        CHECK_FOR_INTERRUPTS();
        bool missing = false;
        Datum d_id = slot_getattr(old_slot, a_id, &missing);
        if (missing) ereport(ERROR, (errmsg("Native enrichment: null destination ID")));
        const lp_id *did = (const lp_id *)DatumGetPointer(d_id);
        while (at < n && memcmp(rows[at].id.b, did->b, sizeof(lp_id)) < 0) at++;
        if (at >= n || !lp_id_eq(&rows[at].id, did)) { ExecClearTuple(old_slot); continue; }
        matched++;
        /* Collapse duplicate stage rows matching the same ID sequentially against updated tuple state. */
        ItemPointerData tid = old_slot->tts_tid;
        for (; at < n && lp_id_eq(&rows[at].id, did); at++) {
            const StageEnrichRow *s = &rows[at];
            retry_lock:
            slot_getallattrs(old_slot);
            bool d_null_t = false, d_null_m = false, d_null_c = false, d_null_h = true;
            Datum d_tier_d = slot_getattr(old_slot, a_tier, &d_null_t);
            Datum d_mask_d = slot_getattr(old_slot, a_mask, &d_null_m);
            Datum d_carrier_d = slot_getattr(old_slot, a_carrier, &d_null_c);
            Datum d_hilb_d = (a_hilbert != InvalidAttrNumber) ? slot_getattr(old_slot, a_hilbert, &d_null_h) : (Datum)0;
            int16 d_tier = DatumGetInt16(d_tier_d);
            lp_mask d_mask = lp_pg_mask_native(d_mask_d);
            int64 d_hilbert = d_null_h ? 0 : DatumGetInt64(d_hilb_d);

            if (check_compat)
                check_carrier_compatible(paths, did, &s->id, d_carrier_d, s->carrier, s->carrier_null,
                                         d_hilbert, d_null_h, s->hilbert, paths || s->hilbert_null);

            int16 new_tier = Min(d_tier, s->tier);
            if (new_tier != d_tier) {
                Relation parent = table_open(parent_target, AccessShareLock);
                PartitionKey pkey = RelationGetPartitionKey(parent);
                if (!checked_tier_partition) {
                    is_by_tier = canonical_is_by_tier(parent_target, pkey);
                    checked_tier_partition = true;
                }
                if (is_by_tier) {
                    PartitionDesc pdesc = RelationGetPartitionDesc(parent, true);
                    Oid dest_part = lp_pg_enrichment_partition(parent_target, pkey, pdesc, Int16GetDatum(new_tier));
                    if (dest_part != partOid) {
                        table_close(parent, NoLock);
                        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                                        errmsg("laplace_merge_stage: conflicting entity tier")));
                    }
                }
                table_close(parent, NoLock);
            }
            lp_mask merged_mask = 0;
            if (!lp_mask_merge(hw, d_mask, s->mask, &merged_mask))
                ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("laplace: conflicting enum values or reserved bits in carrier mask merge")));

            Datum new_path = d_carrier_d;
            bool path_changed = false;
            if (paths && !s->carrier_null && !geom_fast_equal(true, d_carrier_d, s->carrier)) {
                Datum cand = lp_pg_path_enrich_datum(d_carrier_d, s->carrier);
                if (cand && !geom_fast_equal(true, d_carrier_d, cand)) {
                    new_path = cand;
                    path_changed = true;
                }
            }
            if (new_tier == d_tier && merged_mask == d_mask && !path_changed) continue;
            if (path_changed && !path_acl_ok) {
                const char *pcols[1] = {"path"};
                require_table_or_col_acl(parent_target, ACL_UPDATE, pcols, 1);
                path_acl_ok = true;
            }
            if (!estate) {
                estate = CreateExecutorState();
                rri = makeNode(ResultRelInfo);
                InitResultRelInfo(rri, part, 0, NULL, 0);
                ExecOpenIndices(rri, false);
            }

            ExecClearTuple(new_slot);
            for (int c = 0; c < part_td->natts; c++) {
                new_slot->tts_values[c] = old_slot->tts_values[c];
                new_slot->tts_isnull[c] = old_slot->tts_isnull[c];
            }
            new_slot->tts_values[a_tier - 1] = Int16GetDatum(new_tier);
            new_slot->tts_isnull[a_tier - 1] = false;
            new_slot->tts_values[a_mask - 1] = lp_pg_mask_sql(merged_mask);
            new_slot->tts_isnull[a_mask - 1] = false;
            if (path_changed) {
                new_slot->tts_values[a_carrier - 1] = new_path;
                new_slot->tts_isnull[a_carrier - 1] = false;
            }
            ExecStoreVirtualTuple(new_slot);

            TM_FailureData tmfd;
            LockTupleMode lockmode;
            TU_UpdateIndexes update_indexes = TU_None;
            TM_Result res = table_tuple_update(part, &tid, new_slot, cid, snap, InvalidSnapshot,
                                               true, &tmfd, &lockmode, &update_indexes);
            if (res == TM_Updated) {
                ExecClearTuple(new_slot);
                if (IsolationUsesXactSnapshot())
                    ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                                    errmsg("could not serialize access due to concurrent update")));
                ExecClearTuple(old_slot);
                if (table_tuple_lock(part, &tid, snap, old_slot, cid, lockmode, LockWaitBlock,
                                     TUPLE_LOCK_FLAG_FIND_LAST_VERSION, &tmfd) == TM_Ok)
                    goto retry_lock;
                break;
            }
            if (res == TM_Ok) {
                updated++;
                if (touched) *touched = true;
                if (update_indexes != TU_None) {
                    List *recheck = ExecInsertIndexTuples(rri, new_slot, estate, true, false, NULL, NIL,
                                                          update_indexes == TU_Summarizing);
                    list_free(recheck);
                }
            }
            ExecClearTuple(new_slot);
        }
        ExecClearTuple(old_slot);
    }

    if (rri) ExecCloseIndices(rri);
    if (estate) FreeExecutorState(estate);
    ExecDropSingleTupleTableSlot(new_slot);
    ExecDropSingleTupleTableSlot(old_slot);
    index_endscan(scan);
    pfree(wanted);
    index_close(idx, NoLock);
    table_close(part, NoLock);
    if (matched_unique) *matched_unique = matched;
    return updated;
}

static uint64 flush_enrich_batch(Oid parent_target, Relation parent, int nparts, const Oid *parts, const Oid *pks,
                                 Oid idtype, bool paths, bool check_compat, StageEnrichRow *rows, size_t n, Snapshot snap)
{
    if (!n) return 0;
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;
    for (size_t i = 0; i < n; i++)
        rows[i].partition = route_canonical_partition(parent_target, pkey, pdesc, &rows[i].id, rows[i].tier);
    lp_sort(rows, n, sizeof(*rows), stage_enrich_cmp);

    uint64 updated = 0;
    for (size_t begin = 0; begin < n;) {
        size_t end = begin + 1;
        while (end < n && rows[end].partition == rows[begin].partition) end++;
        Relation part = table_open(rows[begin].partition, RowExclusiveLock);
        Oid pkOid = find_partition_pk(nparts, parts, pks, part, rows[begin].partition);
        table_close(part, NoLock);
        updated += enrich_partition_rows(rows[begin].partition, pkOid, idtype, paths, check_compat,
                                         false, parent_target, rows + begin, end - begin, snap, NULL, NULL);
        begin = end;
    }
    return updated;
}

PG_FUNCTION_INFO_V1(laplace_enrich_stage);
Datum laplace_enrich_stage(PG_FUNCTION_ARGS)
{
    Oid input = PG_GETARG_OID(0), target = PG_GETARG_OID(1);
    Oid schema = lp_pg_schema();
    bool paths = target == get_relname_relid("physicality", schema);
    if (!paths && target != get_relname_relid("entity", schema))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_enrich_stage: target must be the installed entity or physicality relation")));
    Relation stage = table_open(input, AccessShareLock);
    Oid stage_ns = get_namespace_oid("stage", false);
    if (RelationGetNamespace(stage) != stage_ns || stage->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED ||
        stage->rd_rel->relkind != RELKIND_RELATION)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_enrich_stage requires an UNLOGGED stage input table")));
    List *inputs = find_all_inheritors(input, ShareLock, NULL);
    list_free(inputs);

    const char *key = paths ? "entity" : "id";
    Oid idtype = get_atttype(target, get_attnum(target, key));
    Oid idarray = get_array_type(idtype);
    int16 key_bytes; bool key_byval; char key_align;
    get_typlenbyvalalign(idtype, &key_bytes, &key_byval, &key_align);
    AttrNumber a_id = get_attnum(input, key);
    if (a_id == InvalidAttrNumber || get_atttype(input, a_id) != idtype ||
        key_bytes != sizeof(lp_id) || key_byval || !OidIsValid(idarray))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_enrich_stage requires matching fixed-width Native IDs")));

    bool has_proof = PG_NARGS() > 2;
    bool direct_proof = false;
    lp_idmap *proof_ids = NULL;
    AttrNumber a_pres = InvalidAttrNumber;
    if (has_proof) {
        Oid selected = PG_GETARG_OID(2);
        Relation proof = table_open(selected, ShareLock);
        bool ok = stage_is_private_unlogged(proof, true);
        direct_proof = (selected == input);
        const char *flag = direct_proof ? "present" : paths ? "path_present" : "entity_present";
        AttrNumber pk_att = get_attnum(selected, direct_proof ? key : "id");
        AttrNumber fl_att = get_attnum(selected, flag);
        if (!ok || pk_att == InvalidAttrNumber || fl_att == InvalidAttrNumber ||
            get_atttype(selected, pk_att) != idtype || get_atttype(selected, fl_att) != BOOLOID ||
            PG_GETARG_INT64(3) < 0)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("laplace_enrich_stage requires a private classified ID set")));
        if (direct_proof) {
            a_pres = fl_att;
        } else {
            proof_ids = lp_idmap_new();
            TableScanDesc ps = table_beginscan(proof, GetActiveSnapshot(), 0, NULL);
            TupleTableSlot *pslot = table_slot_create(proof, NULL);
            while (table_scan_getnextslot(ps, ForwardScanDirection, pslot)) {
                bool nf = false, ni = false;
                Datum fd = slot_getattr(pslot, fl_att, &nf);
                if (!nf && DatumGetBool(fd)) {
                    Datum idd = slot_getattr(pslot, pk_att, &ni);
                    if (!ni) lp_idmap_put(proof_ids, (const lp_id *)DatumGetPointer(idd), NULL);
                }
                ExecClearTuple(pslot);
            }
            ExecDropSingleTupleTableSlot(pslot);
            table_endscan(ps);
        }
        table_close(proof, NoLock);
    }

    require_enrichment_acls(target, paths, has_proof);

    Relation parent = table_open(target, AccessShareLock);
    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions(paths ? "physicality" : "entity", &parts, &pks, NULL, NULL);

    AttrNumber a_tier = get_attnum(input, "tier");
    AttrNumber a_mask = get_attnum(input, "mask");
    AttrNumber a_hilbert = paths ? InvalidAttrNumber : get_attnum(input, "hilbert");
    AttrNumber a_carrier = get_attnum(input, paths ? "path" : "coord");
    if (a_tier == InvalidAttrNumber || a_mask == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_enrich_stage: stage table missing tier/mask")));

    MemoryContext batch_ctx = AllocSetContextCreate(CurrentMemoryContext, "Native enrich batch", ALLOCSET_DEFAULT_SIZES);
    size_t capacity = (size_t)enrichment_fetch_rows();
    StageEnrichRow *rows = palloc(sizeof(StageEnrichRow) * capacity);
    size_t used = 0;
    uint64 updated = 0;

    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan(stage, snap, 0, NULL);
    TupleTableSlot *slot = table_slot_create(stage, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool ni = false, nt = false, nm = false;
        Datum id_d = slot_getattr(slot, a_id, &ni);
        if (ni) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_enrich_stage: null Native ID")));
        if (has_proof) {
            if (direct_proof) {
                bool np = false; Datum pd = slot_getattr(slot, a_pres, &np);
                if (np || !DatumGetBool(pd)) { ExecClearTuple(slot); continue; }
            } else if (lp_idmap_find(proof_ids, (const lp_id *)DatumGetPointer(id_d)) < 0) {
                ExecClearTuple(slot); continue;
            }
        }
        StageEnrichRow r = {0};
        memcpy(r.id.b, DatumGetPointer(id_d), sizeof(lp_id));
        Datum tier_d = slot_getattr(slot, a_tier, &nt);
        Datum mask_d = slot_getattr(slot, a_mask, &nm);
        if (nt || nm) ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION), errmsg("laplace_enrich_stage: null tier or mask")));
        r.tier = DatumGetInt16(tier_d);
        r.mask = lp_pg_mask_native(mask_d);
        r.hilbert_null = true;
        r.carrier_null = true;
        if ((has_proof || paths) && a_hilbert != InvalidAttrNumber) {
            Datum hd = slot_getattr(slot, a_hilbert, &r.hilbert_null);
            if (!r.hilbert_null) r.hilbert = DatumGetInt64(hd);
        }
        if ((has_proof || paths) && a_carrier != InvalidAttrNumber) {
            Datum cd = slot_getattr(slot, a_carrier, &r.carrier_null);
            if (!r.carrier_null) {
                bytea *raw = (bytea *)PG_DETOAST_DATUM(cd);
                Size sz = VARSIZE_ANY(raw);
                bytea *cp = MemoryContextAlloc(batch_ctx, sz);
                memcpy(cp, raw, sz);
                r.carrier = PointerGetDatum(cp);
            }
        }
        rows[used++] = r;
        if (used == capacity) {
            updated += flush_enrich_batch(target, parent, nparts, parts, pks, idtype, paths, has_proof, rows, used, snap);
            used = 0;
            MemoryContextReset(batch_ctx);
        }
        ExecClearTuple(slot);
    }
    if (used > 0)
        updated += flush_enrich_batch(target, parent, nparts, parts, pks, idtype, paths, has_proof, rows, used, snap);

    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    MemoryContextDelete(batch_ctx);
    pfree(rows);
    if (proof_ids) lp_idmap_free(proof_ids);
    table_close(parent, NoLock);
    table_close(stage, NoLock);
    PG_RETURN_INT64((int64)updated);
}

/* ---------------------------------------------------------------- GIN pending-list maintenance */
static Oid resolve_physicality_parent(void)
{
    return lp_pg_relation("physicality");
}

static int64 clean_gin_index_oid(Oid idxOid, Oid tableOid, Oid parentOid, Oid priv_role)
{
    if (pg_class_aclcheck(tableOid, priv_role, ACL_INSERT) != ACLCHECK_OK &&
        pg_class_aclcheck(parentOid, priv_role, ACL_INSERT) != ACLCHECK_OK)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("Laplace ingestion write privileges are required")));
    if (pg_class_aclcheck(tableOid, priv_role, ACL_UPDATE) != ACLCHECK_OK &&
        pg_class_aclcheck(parentOid, priv_role, ACL_UPDATE) != ACLCHECK_OK)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("Laplace ingestion write privileges are required")));

    Relation idx = index_open(idxOid, AccessShareLock);
    Oid owner = idx->rd_rel->relowner;
    index_close(idx, NoLock);

    Oid save_uid; int save_sec;
    GetUserIdAndSecContext(&save_uid, &save_sec);
    int64 cleaned = 0;
    PG_TRY(); {
        SetUserIdAndSecContext(owner, save_sec | SECURITY_LOCAL_USERID_CHANGE);
        cleaned = DatumGetInt64(DirectFunctionCall1(gin_clean_pending_list, ObjectIdGetDatum(idxOid)));
    } PG_FINALLY(); {
        SetUserIdAndSecContext(save_uid, save_sec);
    } PG_END_TRY();
    return cleaned;
}

PG_FUNCTION_INFO_V1(laplace_clean_pending_list);
Datum laplace_clean_pending_list(PG_FUNCTION_ARGS)
{
    Oid index_id = PG_GETARG_OID(0);
    Relation idx = try_relation_open(index_id, AccessShareLock);
    if (!idx)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("index is not an extension-owned Laplace physicality GIN index")));
    bool is_gin = (idx->rd_rel->relkind == RELKIND_INDEX && idx->rd_rel->relam == GIN_AM_OID);
    index_close(idx, NoLock);
    Oid table_id = is_gin ? IndexGetRelation(index_id, true) : InvalidOid;
    const Oid *parts = NULL;
    int nparts = lp_pg_canonical_partitions("physicality", &parts, NULL, NULL, NULL);
    bool is_part = false;
    for (int p = 0; p < nparts; p++)
        if (parts[p] == table_id) { is_part = true; break; }
    Oid ext_oid = get_extension_oid("laplace", true);
    if (!is_gin || !is_part || !OidIsValid(ext_oid) ||
        getExtensionOfObject(RelationRelationId, table_id) != ext_oid)
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                        errmsg("index is not an extension-owned Laplace physicality GIN index")));

    Oid role = GetCurrentRoleId();
    Oid caller = OidIsValid(role) ? role : GetSessionUserId();
    PG_RETURN_INT64(clean_gin_index_oid(index_id, table_id, resolve_physicality_parent(), caller));
}

static ArrayType *stage_partition_ids(Oid input)
{
    Relation stage = table_open(input, AccessShareLock);
    Oid target = resolve_physicality_parent();
    Relation parent = table_open(target, AccessShareLock);
    Oid idtype = get_atttype(target, get_attnum(target, "entity"));
    AttrNumber a_ent = get_attnum(input, "entity");
    if (RelationGetNamespace(stage) != get_namespace_oid("stage", false)
        || stage->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED
        || stage->rd_rel->relkind != RELKIND_RELATION
        || a_ent == InvalidAttrNumber || get_atttype(input, a_ent) != idtype)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_clean_stage requires an UNLOGGED stage table with Native entity IDs")));
    List *inputs = find_all_inheritors(input, ShareLock, NULL);
    list_free(inputs);
    PartitionKey key = RelationGetPartitionKey(parent);
    AttrNumber a_ent_target = get_attnum(target, "entity");
    AttrNumber a_tier_target = get_attnum(target, "tier");
    bool by_tier = (key && key->strategy == PARTITION_STRATEGY_RANGE && key->partnatts == 1 &&
                    a_tier_target != InvalidAttrNumber && key->partattrs[0] == a_tier_target);
    bool by_ent = (key && key->strategy == PARTITION_STRATEGY_RANGE && key->partnatts == 1 &&
                   a_ent_target != InvalidAttrNumber && key->partattrs[0] == a_ent_target);
    if (!by_tier && !by_ent)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_clean_stage requires Native physicality range partitions")));
    AttrNumber a_tier_stage = by_tier ? get_attnum(input, "tier") : InvalidAttrNumber;
    bool stage_has_tier = (a_tier_stage != InvalidAttrNumber && get_atttype(input, a_tier_stage) == INT2OID);
    PartitionDesc pd = RelationGetPartitionDesc(parent, true);
    HASHCTL control = {.keysize = sizeof(Oid), .entrysize = sizeof(Oid)};
    HTAB *touched = hash_create("Native copied path partitions", Max(pd->nparts, 1), &control, HASH_ELEM | HASH_BLOBS);

    size_t id_cap = 0, id_count = 0;
    lp_id *lookup_ids = NULL;
    TableScanDesc scan = table_beginscan(stage, GetActiveSnapshot(), 0, NULL);
    TupleTableSlot *slot = table_slot_create(stage, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool missing = false;
        Datum id = slot_getattr(slot, a_ent, &missing);
        if (missing) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_clean_stage: null Native ID")));
        if (by_tier) {
            if (stage_has_tier) {
                bool tmissing = false;
                Datum t = slot_getattr(slot, a_tier_stage, &tmissing);
                if (tmissing) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_clean_stage: null tier")));
                Oid partition = lp_pg_enrichment_partition(target, key, pd, t);
                hash_search(touched, &partition, HASH_ENTER, NULL);
            } else {
                if (id_count == id_cap) {
                    id_cap = id_cap ? id_cap * 2 : 256;
                    lookup_ids = lookup_ids ? repalloc(lookup_ids, id_cap * sizeof(lp_id))
                                            : palloc(id_cap * sizeof(lp_id));
                }
                memcpy(lookup_ids[id_count++].b, DatumGetPointer(id), sizeof(lp_id));
            }
        } else {
            Oid partition = lp_pg_enrichment_partition(target, key, pd, id);
            hash_search(touched, &partition, HASH_ENTER, NULL);
        }
        ExecClearTuple(slot);
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    if (id_count > 0) {
        int16 *tiers = palloc(id_count * sizeof(int16));
        lp_pg_lookup_entity_tiers(lookup_ids, id_count, tiers);
        for (size_t i = 0; i < id_count; i++) {
            if (tiers[i] < 0) continue;
            Oid partition = lp_pg_enrichment_partition(target, key, pd, Int16GetDatum(tiers[i]));
            hash_search(touched, &partition, HASH_ENTER, NULL);
        }
        pfree(tiers);
        pfree(lookup_ids);
    } else if (lookup_ids) {
        pfree(lookup_ids);
    }

    int count = hash_get_num_entries(touched), at = 0;
    Datum *values = palloc(sizeof(Datum) * Max(count, 1));
    HASH_SEQ_STATUS entries;
    hash_seq_init(&entries, touched);
    Oid *partition;
    while ((partition = hash_seq_search(&entries))) values[at++] = ObjectIdGetDatum(*partition);
    hash_destroy(touched);
    table_close(parent, NoLock);
    table_close(stage, NoLock);
    ArrayType *ids = construct_array(values, count, OIDOID, sizeof(Oid), true, TYPALIGN_INT);
    pfree(values);
    return ids;
}

static int64 clean_partition_oid_list(const Oid *part_oids, int n)
{
    Oid parent_oid = resolve_physicality_parent();
    const Oid *parts = NULL;
    int nparts = lp_pg_canonical_partitions("physicality", &parts, NULL, NULL, NULL);
    HASHCTL control = {.keysize = sizeof(Oid), .entrysize = sizeof(Oid)};
    HTAB *allowed = hash_create("Native maintenance partitions", Max(nparts, 1), &control, HASH_ELEM | HASH_BLOBS);
    for (int i = 0; i < nparts; i++) hash_search(allowed, &parts[i], HASH_ENTER, NULL);
    for (int i = 0; i < n; i++) {
        if (!OidIsValid(part_oids[i]) || !hash_search(allowed, &part_oids[i], HASH_FIND, NULL))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_clean_partitions requires Native physicality partitions")));
    }
    hash_destroy(allowed);

    Oid uid = GetUserId();
    int64 cleaned = 0;
    for (int i = 0; i < n; i++) {
        Oid poid = part_oids[i];
        Relation part = table_open(poid, AccessShareLock);
        List *idxs = RelationGetIndexList(part);
        table_close(part, NoLock);

        Oid *gin_oids = palloc((Size)Max(list_length(idxs), 1) * sizeof(Oid));
        int ngins = 0;
        ListCell *lc;
        foreach(lc, idxs) {
            Oid ioid = lfirst_oid(lc);
            Relation idx = index_open(ioid, AccessShareLock);
            if (idx->rd_rel->relam == GIN_AM_OID && idx->rd_index && idx->rd_index->indislive)
                gin_oids[ngins++] = ioid;
            index_close(idx, NoLock);
        }
        list_free(idxs);
        if (ngins > 1) qsort(gin_oids, (size_t)ngins, sizeof(Oid), lp_oid_cmp);
        for (int j = 0; j < ngins; j++)
            cleaned += clean_gin_index_oid(gin_oids[j], poid, parent_oid, uid);
        pfree(gin_oids);
    }
    return cleaned;
}

static int64 clean_partition_ids(ArrayType *ids)
{
    int cap = ArrayGetNItems(ARR_NDIM(ids), ARR_DIMS(ids));
    Oid *oids = palloc((Size)Max(cap, 1) * sizeof(Oid));
    int n = 0;
    ArrayIterator it = array_create_iterator(ids, 0, NULL);
    Datum item; bool missing;
    while (array_iterate(it, &item, &missing)) {
        if (missing) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_clean_partitions requires Native physicality partitions")));
        oids[n++] = DatumGetObjectId(item);
    }
    array_free_iterator(it);
    int64 cleaned = clean_partition_oid_list(oids, n);
    pfree(oids);
    return cleaned;
}

PG_FUNCTION_INFO_V1(laplace_stage_partitions);
Datum laplace_stage_partitions(PG_FUNCTION_ARGS)
{
    PG_RETURN_ARRAYTYPE_P(stage_partition_ids(PG_GETARG_OID(0)));
}

PG_FUNCTION_INFO_V1(laplace_clean_partitions);
Datum laplace_clean_partitions(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(clean_partition_ids(PG_GETARG_ARRAYTYPE_P(0)));
}

PG_FUNCTION_INFO_V1(laplace_clean_stage);
Datum laplace_clean_stage(PG_FUNCTION_ARGS)
{
    ArrayType *ids = stage_partition_ids(PG_GETARG_OID(0));
    int64 cleaned = clean_partition_ids(ids);
    pfree(ids);
    PG_RETURN_INT64(cleaned);
}

/* ---------------------------------------------------------------- stage load ownership & retirement */
PG_FUNCTION_INFO_V1(laplace_stage_own);
Datum laplace_stage_own(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_own requires unclaimed private load tables owned by the caller")));
    int64 load_token = PG_GETARG_INT64(0);
    if (load_token <= 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_own requires unclaimed private load tables owned by the caller")));
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(1);
    Datum *elems; bool *nulls; int n;
    deconstruct_array(arr, OIDOID, sizeof(Oid), true, TYPALIGN_INT, &elems, &nulls, &n);
    if (n <= 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_stage_own requires unclaimed private load tables owned by the caller")));

    Oid stage_ns = get_namespace_oid("stage", true);
    Oid uid = GetUserId();
    Oid *oids = palloc((Size)n * sizeof(Oid));
    for (int i = 0; i < n; i++) {
        if (nulls[i])
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("laplace_stage_own requires unclaimed private load tables owned by the caller")));
        Oid oid = DatumGetObjectId(elems[i]);
        Relation rel = try_relation_open(oid, AccessShareLock);
        if (!rel || !OidIsValid(stage_ns) || RelationGetNamespace(rel) != stage_ns ||
            rel->rd_rel->relkind != RELKIND_RELATION || rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED ||
            !has_privs_of_role(uid, rel->rd_rel->relowner) ||
            !stage_name_matches_token(RelationGetRelationName(rel), load_token)) {
            if (rel) table_close(rel, NoLock);
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("laplace_stage_own requires unclaimed private load tables owned by the caller")));
        }
        table_close(rel, NoLock);
        char *existing = GetComment(oid, RelationRelationId, 0);
        if (existing != NULL) {
            pfree(existing);
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("laplace_stage_own requires unclaimed private load tables owned by the caller")));
        }
        oids[i] = oid;
    }
    pfree(elems);
    pfree(nulls);
    qsort(oids, (size_t)n, sizeof(Oid), lp_oid_cmp);
    int uniq = 0;
    for (int i = 0; i < n; i++)
        if (uniq == 0 || oids[uniq - 1] != oids[i]) oids[uniq++] = oids[i];

    int32 owner_pid = (int32)MyProcPid;
    LOCKTAG tag;
    SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, (uint32)1279348804, (uint32)owner_pid, 2);
    LockAcquire(&tag, ExclusiveLock, true /* sessionLock */, false);
    PG_TRY(); {
        char *cmt = psprintf("laplace:stage-load:1:%d:%lld", owner_pid, (long long)load_token);
        for (int i = 0; i < uniq; i++)
            CreateComments(oids[i], RelationRelationId, 0, cmt);
        pfree(cmt);
        CommandCounterIncrement();
    } PG_CATCH(); {
        LockRelease(&tag, ExclusiveLock, true);
        PG_RE_THROW();
    } PG_END_TRY();
    pfree(oids);
    PG_RETURN_VOID();
}

typedef struct {
    char *desc;
    int32 pid;
    int64 token;
    Oid oid;
} AbandonedLeaseRow;

static int abandoned_cmp(const void *a, const void *b)
{
    const AbandonedLeaseRow *x = a, *y = b;
    int c = strcmp(x->desc, y->desc);
    return c ? c : lp_oid_cmp(&x->oid, &y->oid);
}

PG_FUNCTION_INFO_V1(laplace_stage_retire);
Datum laplace_stage_retire(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *output = set_begin(fcinfo, "laplace_stage_retire", &td);
    Oid stage_ns = get_namespace_oid("stage", true);
    if (!OidIsValid(stage_ns)) PG_RETURN_NULL();
    Oid uid = GetUserId();

    lp_vec(AbandonedLeaseRow) list = {0};
    Relation drel = table_open(DescriptionRelationId, AccessShareLock);
    TableScanDesc scan = table_beginscan(drel, GetActiveSnapshot(), 0, NULL);
    TupleTableSlot *slot = table_slot_create(drel, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        bool n1 = false, n2 = false, n3 = false, n4 = false;
        Oid objoid = DatumGetObjectId(slot_getattr(slot, Anum_pg_description_objoid, &n1));
        Oid classoid = DatumGetObjectId(slot_getattr(slot, Anum_pg_description_classoid, &n2));
        int32 objsubid = DatumGetInt32(slot_getattr(slot, Anum_pg_description_objsubid, &n3));
        Datum dd = slot_getattr(slot, Anum_pg_description_description, &n4);
        if (n1 || n2 || n3 || n4 || classoid != RelationRelationId || objsubid != 0) {
            ExecClearTuple(slot); continue;
        }
        char *desc = TextDatumGetCString(dd);
        int32 pid = 0; int64 tok = 0;
        if (!parse_stage_comment(desc, &pid, &tok)) { pfree(desc); ExecClearTuple(slot); continue; }
        HeapTuple ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(objoid));
        if (!HeapTupleIsValid(ctup)) { pfree(desc); ExecClearTuple(slot); continue; }
        Form_pg_class cls = (Form_pg_class)GETSTRUCT(ctup);
        bool keep = (cls->relnamespace == stage_ns && cls->relkind == RELKIND_RELATION &&
                     cls->relpersistence == RELPERSISTENCE_UNLOGGED &&
                     has_privs_of_role(uid, cls->relowner) &&
                     stage_name_matches_token(NameStr(cls->relname), tok));
        ReleaseSysCache(ctup);
        if (!keep) { pfree(desc); ExecClearTuple(slot); continue; }
        AbandonedLeaseRow row = {.desc = desc, .pid = pid, .token = tok, .oid = objoid};
        lp_push(&list, row);
        ExecClearTuple(slot);
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    table_close(drel, AccessShareLock);

    if (list.n > 1) qsort(list.v, list.n, sizeof(AbandonedLeaseRow), abandoned_cmp);

    for (size_t i = 0; i < list.n;) {
        size_t j = i + 1;
        while (j < list.n && strcmp(list.v[j].desc, list.v[i].desc) == 0) j++;
        int32 owner_pid = list.v[i].pid;
        int64 token = list.v[i].token;
        LOCKTAG tag;
        SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, (uint32)1279348804, (uint32)owner_pid, 2);
        if (owner_pid != (int32)MyProcPid &&
            LockAcquire(&tag, ExclusiveLock, false /* xact */, true /* dontWait */) != LOCKACQUIRE_NOT_AVAIL) {
            ObjectAddresses *objs = new_object_addresses();
            int64 total_bytes = 0, rel_count = 0;
            for (size_t k = i; k < j; k++) {
                Relation rel = try_relation_open(list.v[k].oid, AccessExclusiveLock);
                if (!rel) continue;
                total_bytes += DatumGetInt64(DirectFunctionCall1(pg_total_relation_size, ObjectIdGetDatum(list.v[k].oid)));
                rel_count++;
                ObjectAddress addr = {RelationRelationId, list.v[k].oid, 0};
                add_exact_object_address(&addr, objs);
                table_close(rel, NoLock);
            }
            if (rel_count > 0) {
                performMultipleDeletions(objs, DROP_RESTRICT, 0);
                CommandCounterIncrement();
                Datum v[3] = {Int64GetDatum(token), Int64GetDatum(rel_count), Int64GetDatum(total_bytes)};
                bool nl[3] = {false, false, false};
                set_put(fcinfo, output, td, v, nl);
            }
            free_object_addresses(objs);
        }
        i = j;
    }

    for (size_t i = 0; i < list.n; i++) pfree(list.v[i].desc);
    lp_vec_free(&list);
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- stage merge */
typedef struct {
    lp_id id;
    Oid partition;
    int16 tier;
    lp_mask mask;
    int64 hilbert;
    bool hilbert_null;
    bool carrier_null;
    Datum carrier;
    bool present;
} MergeRow;
typedef lp_vec(MergeRow) MergeRowVec;

typedef struct {
    int e_idx, p_idx;
} MergePair;

static int merge_insert_cmp(const void *a, const void *b)
{
    const MergeRow *x = a, *y = b;
    int c = lp_oid_cmp(&x->partition, &y->partition);
    if (c) return c;
    c = (x->hilbert > y->hilbert) - (x->hilbert < y->hilbert);
    return c ? c : memcmp(x->id.b, y->id.b, sizeof(lp_id));
}

static bool stage_load_and_validate(Relation stage_e, Relation stage_p, Oid ent_oid, Oid path_oid,
                                    MemoryContext ctx, MergeRowVec *erows, MergeRowVec *prows)
{
    Oid e_oid = RelationGetRelid(stage_e), p_oid = RelationGetRelid(stage_p);
    AttrNumber ea_pres = get_attnum(e_oid, "present"), pa_pres = get_attnum(p_oid, "present");
    if (ea_pres != InvalidAttrNumber && get_atttype(e_oid, ea_pres) != BOOLOID)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("entity presence classification must be boolean")));
    if (pa_pres != InvalidAttrNumber && get_atttype(p_oid, pa_pres) != BOOLOID)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("path presence classification must be boolean")));
    bool use_pres = (ea_pres != InvalidAttrNumber && pa_pres != InvalidAttrNumber);

    AttrNumber ea_id = get_attnum(e_oid, "id"), ea_tier = get_attnum(e_oid, "tier"),
               ea_coord = get_attnum(e_oid, "coord"), ea_hilb = get_attnum(e_oid, "hilbert"),
               ea_mask = get_attnum(e_oid, "mask");
    AttrNumber pa_id = get_attnum(p_oid, "entity"), pa_tier = get_attnum(p_oid, "tier"),
               pa_path = get_attnum(p_oid, "path"), pa_mask = get_attnum(p_oid, "mask");
    const lp_highway *hw = lp_pg_highway();
    Snapshot snap = GetActiveSnapshot();
    lp_idmap *pairs = lp_idmap_sized(sizeof(MergePair));
    bool invalid = false;

    PG_TRY(); {
        TableScanDesc es = table_beginscan(stage_e, snap, 0, NULL);
        TupleTableSlot *slot = table_slot_create(stage_e, NULL);
        while (!invalid && table_scan_getnextslot(es, ForwardScanDirection, slot)) {
            CHECK_FOR_INTERRUPTS();
            bool ni = false, nt = false, nc = false, nh = false, nm = false, np = false;
            Datum di = slot_getattr(slot, ea_id, &ni);
            Datum dt = slot_getattr(slot, ea_tier, &nt);
            Datum dc = slot_getattr(slot, ea_coord, &nc);
            Datum dh = slot_getattr(slot, ea_hilb, &nh);
            Datum dm = slot_getattr(slot, ea_mask, &nm);
            bool pres = false;
            if (use_pres) {
                Datum dp = slot_getattr(slot, ea_pres, &np);
                pres = !np && DatumGetBool(dp);
            }
            int16 tier = nt ? -1 : DatumGetInt16(dt);
            lp_mask mask = nm ? 0 : lp_pg_mask_native(dm), dummy;
            if (ni || nt || nm || tier < 1 || tier > 255 || !lp_mask_merge(hw, mask, 0, &dummy) ||
                (!pres && (nc || nh))) {
                invalid = true; ExecClearTuple(slot); break;
            }
            MergeRow row = {
                .tier = tier, .mask = mask, .hilbert = nh ? 0 : DatumGetInt64(dh),
                .hilbert_null = nh, .carrier_null = nc, .present = pres
            };
            memcpy(row.id.b, DatumGetPointer(di), sizeof(lp_id));
            if (!nc) {
                bytea *raw = (bytea *)PG_DETOAST_DATUM(dc);
                Size sz = VARSIZE_ANY(raw);
                bytea *cp = MemoryContextAlloc(ctx, sz);
                memcpy(cp, raw, sz);
                row.carrier = PointerGetDatum(cp);
            }
            bool fresh = false;
            size_t at = lp_idmap_put(pairs, &row.id, &fresh);
            MergePair *pair = lp_idmap_at(pairs, at);
            if (!fresh) { invalid = true; ExecClearTuple(slot); break; }
            pair->e_idx = (int)erows->n;
            pair->p_idx = -1;
            lp_push(erows, row);
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);
        table_endscan(es);

        TableScanDesc ps = table_beginscan(stage_p, snap, 0, NULL);
        TupleTableSlot *pslot = table_slot_create(stage_p, NULL);
        while (!invalid && table_scan_getnextslot(ps, ForwardScanDirection, pslot)) {
            CHECK_FOR_INTERRUPTS();
            bool ni = false, nt = false, nc = false, nm = false, np = false;
            Datum di = slot_getattr(pslot, pa_id, &ni);
            Datum dt = slot_getattr(pslot, pa_tier, &nt);
            Datum dc = slot_getattr(pslot, pa_path, &nc);
            Datum dm = slot_getattr(pslot, pa_mask, &nm);
            bool pres = false;
            if (use_pres) {
                Datum dp = slot_getattr(pslot, pa_pres, &np);
                pres = !np && DatumGetBool(dp);
            }
            int16 tier = nt ? -1 : DatumGetInt16(dt);
            lp_mask mask = nm ? 0 : lp_pg_mask_native(dm), dummy;
            if (ni || nt || nm || tier < 1 || tier > 255 || !lp_mask_merge(hw, mask, 0, &dummy) ||
                (!pres && nc)) {
                invalid = true; ExecClearTuple(pslot); break;
            }
            MergeRow row = {
                .tier = tier, .mask = mask, .hilbert = 0,
                .hilbert_null = true, .carrier_null = nc, .present = pres
            };
            memcpy(row.id.b, DatumGetPointer(di), sizeof(lp_id));
            if (!nc) {
                bytea *raw = (bytea *)PG_DETOAST_DATUM(dc);
                Size sz = VARSIZE_ANY(raw);
                bytea *cp = MemoryContextAlloc(ctx, sz);
                memcpy(cp, raw, sz);
                row.carrier = PointerGetDatum(cp);
            }
            int64 found = lp_idmap_find(pairs, &row.id);
            if (found < 0) { invalid = true; ExecClearTuple(pslot); break; }
            MergePair *pair = lp_idmap_at(pairs, (size_t)found);
            if (pair->p_idx >= 0) { invalid = true; ExecClearTuple(pslot); break; }
            row.hilbert = erows->v[pair->e_idx].hilbert;
            pair->p_idx = (int)prows->n;
            lp_push(prows, row);
            ExecClearTuple(pslot);
        }
        ExecDropSingleTupleTableSlot(pslot);
        table_endscan(ps);

        if (!invalid && erows->n != prows->n) invalid = true;
    } PG_FINALLY(); {
        lp_idmap_free(pairs);
    } PG_END_TRY();
    if (invalid)
        ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                        errmsg("laplace_merge_stage: duplicate IDs, invalid carriers or unpaired entity/path input")));
    return use_pres;
}

#define MERGE_MULTI_BATCH 512

static uint64 merge_insert_table(Relation parent, const char *relname, bool is_paths, Oid idtype,
                                 int nparts, const Oid *parts, const Oid *pks,
                                 MergeRow *rows, size_t nrows,
                                 MergeRowVec *concurrent_landed, HTAB *touched_parts,
                                 bool fast_multi)
{
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;
    lp_vec(MergeRow) ins = {0};
    for (size_t i = 0; i < nrows; i++) {
        if (!rows[i].present) {
            rows[i].partition = route_canonical_partition(RelationGetRelid(parent), pkey, pdesc, &rows[i].id, rows[i].tier);
            lp_push(&ins, rows[i]);
        }
    }
    if (!ins.n) return 0;
    lp_sort(ins.v, ins.n, sizeof(MergeRow), merge_insert_cmp);

    uint64 inserted = 0;
    CommandId cid = GetCurrentCommandId(true);
    for (size_t begin = 0; begin < ins.n;) {
        size_t end = begin + 1;
        while (end < ins.n && ins.v[end].partition == ins.v[begin].partition) end++;
        Oid partOid = ins.v[begin].partition;
        Relation part = table_open(partOid, RowExclusiveLock);
        Oid pkOid = find_partition_pk(nparts, parts, pks, part, partOid);
        List *arbiter = list_make1_oid(pkOid);

        EState *estate = CreateExecutorState();
        ResultRelInfo *rri = makeNode(ResultRelInfo);
        InitResultRelInfo(rri, part, 0, NULL, 0);
        ExecOpenIndices(rri, !fast_multi /* speculative */);

        AttrNumber a_id = get_attnum(partOid, is_paths ? "entity" : "id");
        AttrNumber a_tier = get_attnum(partOid, "tier");
        AttrNumber a_carrier = get_attnum(partOid, is_paths ? "path" : "coord");
        AttrNumber a_hilb = is_paths ? InvalidAttrNumber : get_attnum(partOid, "hilbert");
        AttrNumber a_mask = get_attnum(partOid, "mask");
        BulkInsertState bistate = GetBulkInsertState();
        bool part_inserted = false;

        if (fast_multi) {
            int natts = RelationGetDescr(part)->natts;
            int nslots = (int)Min((size_t)MERGE_MULTI_BATCH, end - begin);
            TupleTableSlot **slots = palloc(sizeof(TupleTableSlot *) * (size_t)nslots);
            for (int k = 0; k < nslots; k++) slots[k] = table_slot_create(part, NULL);

            PG_TRY(); {
                for (size_t off = begin; off < end;) {
                    CHECK_FOR_INTERRUPTS();
                    int nbatch = (int)Min((size_t)nslots, end - off);
                    for (int k = 0; k < nbatch; k++) {
                        MergeRow *r = &ins.v[off + (size_t)k];
                        TupleTableSlot *s = slots[k];
                        ExecClearTuple(s);
                        memset(s->tts_isnull, 0, (size_t)natts * sizeof(bool));
                        s->tts_values[a_id - 1] = PointerGetDatum(&r->id);
                        s->tts_values[a_tier - 1] = Int16GetDatum(r->tier);
                        s->tts_values[a_carrier - 1] = r->carrier;
                        if (!is_paths) s->tts_values[a_hilb - 1] = Int64GetDatum(r->hilbert);
                        s->tts_values[a_mask - 1] = lp_pg_mask_sql(r->mask);
                        ExecStoreVirtualTuple(s);
                    }
                    table_multi_insert(part, slots, nbatch, cid, 0, bistate);
                    for (int k = 0; k < nbatch; k++) {
                        List *recheck = ExecInsertIndexTuples(rri, slots[k], estate, false, false, NULL, NIL, false);
                        list_free(recheck);
                        ExecClearTuple(slots[k]);
                    }
                    inserted += (uint64)nbatch;
                    part_inserted = true;
                    off += (size_t)nbatch;
                }
                if (part_inserted) table_finish_bulk_insert(part, 0);
            } PG_FINALLY(); {
                for (int k = 0; k < nslots; k++) ExecDropSingleTupleTableSlot(slots[k]);
                pfree(slots);
                FreeBulkInsertState(bistate);
                ExecCloseIndices(rri);
                FreeExecutorState(estate);
                list_free(arbiter);
                table_close(part, NoLock);
            } PG_END_TRY();
        } else {
            TupleTableSlot *slot = table_slot_create(part, NULL);

            PG_TRY(); {
                for (size_t i = begin; i < end; i++) {
                    CHECK_FOR_INTERRUPTS();
                    MergeRow *r = &ins.v[i];
                    ExecClearTuple(slot);
                    memset(slot->tts_isnull, 0, (size_t)RelationGetDescr(part)->natts * sizeof(bool));
                    slot->tts_values[a_id - 1] = PointerGetDatum(&r->id);
                    slot->tts_values[a_tier - 1] = Int16GetDatum(r->tier);
                    slot->tts_values[a_carrier - 1] = r->carrier;
                    if (!is_paths) slot->tts_values[a_hilb - 1] = Int64GetDatum(r->hilbert);
                    slot->tts_values[a_mask - 1] = lp_pg_mask_sql(r->mask);
                    ExecStoreVirtualTuple(slot);

                    ItemPointerData conflictTid;
                    for (;;) {
                        if (!ExecCheckIndexConstraints(rri, slot, estate, &conflictTid, NULL, arbiter)) {
                            if (IsolationUsesXactSnapshot())
                                ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
                                                errmsg("duplicate key value violates unique constraint")));
                            lp_push(concurrent_landed, *r);
                            break;
                        }
                        uint32 specToken = SpeculativeInsertionLockAcquire(GetTopTransactionId());
                        table_tuple_insert_speculative(part, slot, cid, 0, bistate, specToken);
                        bool specConflict = false;
                        List *recheck = ExecInsertIndexTuples(rri, slot, estate, false, true, &specConflict, arbiter, false);
                        list_free(recheck);
                        if (specConflict) {
                            table_tuple_complete_speculative(part, slot, specToken, false);
                            SpeculativeInsertionLockRelease(GetTopTransactionId());
                        } else {
                            table_tuple_complete_speculative(part, slot, specToken, true);
                            SpeculativeInsertionLockRelease(GetTopTransactionId());
                            inserted++;
                            part_inserted = true;
                            break;
                        }
                    }
                    ExecClearTuple(slot);
                }
                if (part_inserted) table_finish_bulk_insert(part, 0);
            } PG_FINALLY(); {
                FreeBulkInsertState(bistate);
                ExecDropSingleTupleTableSlot(slot);
                ExecCloseIndices(rri);
                FreeExecutorState(estate);
                list_free(arbiter);
                table_close(part, NoLock);
            } PG_END_TRY();
        }
        if (part_inserted && touched_parts) hash_search(touched_parts, &partOid, HASH_ENTER, NULL);
        begin = end;
    }
    lp_vec_free(&ins);
    return inserted;
}

static uint64 merge_enrich_subset(Relation parent, bool is_paths, Oid idtype, int nparts, const Oid *parts, const Oid *pks,
                                  const MergeRow *rows, size_t n, Snapshot snap, HTAB *touched_parts)
{
    if (!n) return 0;
    PartitionKey pkey = RelationGetPartitionKey(parent);
    PartitionDesc pdesc = pkey ? RelationGetPartitionDesc(parent, true) : NULL;
    Oid parent_oid = RelationGetRelid(parent);
    bool by_tier = canonical_is_by_tier(parent_oid, pkey);
    StageEnrichRow *er = palloc(sizeof(StageEnrichRow) * n);
    for (size_t i = 0; i < n; i++) {
        er[i] = (StageEnrichRow){
            .id = rows[i].id,
            .partition = route_canonical_partition(parent_oid, pkey, pdesc, &rows[i].id, rows[i].tier),
            .tier = rows[i].tier, .mask = rows[i].mask,
            .carrier_null = rows[i].carrier_null, .hilbert_null = rows[i].hilbert_null,
            .carrier = rows[i].carrier, .hilbert = rows[i].hilbert
        };
    }
    lp_sort(er, n, sizeof(*er), stage_enrich_cmp);
    uint64 updated = 0;
    for (size_t begin = 0; begin < n;) {
        size_t end = begin + 1;
        while (end < n && er[end].partition == er[begin].partition) end++;
        Oid partOid = er[begin].partition;
        Relation part = table_open(partOid, RowExclusiveLock);
        Oid pkOid = find_partition_pk(nparts, parts, pks, part, partOid);
        table_close(part, NoLock);
        bool touched = false;
        size_t part_matched = 0;
        updated += enrich_partition_rows(partOid, pkOid, idtype, is_paths, true, true,
                                         parent_oid, er + begin, end - begin, snap, &touched, &part_matched);
        if (by_tier && part_matched < (end - begin)) {
            size_t sn = end - begin;
            lp_id *sids = palloc(sn * sizeof(lp_id));
            int16 *actual_tiers = palloc(sn * sizeof(int16));
            for (size_t k = 0; k < sn; k++) sids[k] = er[begin + k].id;
            lp_pg_lookup_entity_tiers(sids, sn, actual_tiers);
            for (size_t k = 0; k < sn; k++) {
                if (actual_tiers[k] >= 0 &&
                    route_canonical_partition(parent_oid, pkey, pdesc, &sids[k], actual_tiers[k]) != partOid) {
                    pfree(actual_tiers);
                    pfree(sids);
                    pfree(er);
                    ereport(ERROR, (errcode(ERRCODE_CHECK_VIOLATION),
                                    errmsg("laplace_merge_stage: conflicting entity tier")));
                }
            }
            pfree(actual_tiers);
            pfree(sids);
        }
        if (touched && touched_parts) hash_search(touched_parts, &partOid, HASH_ENTER, NULL);
        begin = end;
    }
    pfree(er);
    return updated;
}

PG_FUNCTION_INFO_V1(laplace_merge_stage);
Datum laplace_merge_stage(PG_FUNCTION_ARGS)
{
    bool has_defer = (PG_NARGS() >= 3);
    if (has_defer && PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_merge_stage requires an explicit index-maintenance policy")));
    bool defer_index = has_defer ? PG_GETARG_BOOL(2) : false;

    Oid e_oid = PG_GETARG_OID(0), p_oid = PG_GETARG_OID(1);
    Relation stage_e = try_relation_open(e_oid, ShareLock);
    Relation stage_p = try_relation_open(p_oid, ShareLock);
    Oid stage_ns = get_namespace_oid("stage", false);
    if (!stage_e || !stage_p || e_oid == p_oid ||
        RelationGetNamespace(stage_e) != stage_ns || stage_e->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED || stage_e->rd_rel->relkind != RELKIND_RELATION ||
        RelationGetNamespace(stage_p) != stage_ns || stage_p->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED || stage_p->rd_rel->relkind != RELKIND_RELATION) {
        if (stage_e) table_close(stage_e, NoLock);
        if (stage_p) table_close(stage_p, NoLock);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_merge_stage requires distinct UNLOGGED stage input tables")));
    }

    Oid ent_oid = lp_pg_relation("entity"), path_oid = lp_pg_relation("physicality");
    Relation ent_parent = table_open(ent_oid, AccessShareLock);
    Relation path_parent = table_open(path_oid, AccessShareLock);
    const char *e_req_sel[] = {"id", "tier", "mask", "coord", "hilbert"};
    const char *p_req_sel[] = {"entity", "tier", "mask", "path"};
    require_table_or_col_acl(ent_oid, ACL_SELECT, e_req_sel, 5);
    require_table_or_col_acl(ent_oid, ACL_INSERT, e_req_sel, 5);
    require_table_or_col_acl(ent_oid, ACL_UPDATE, (const char *[]){"tier", "mask"}, 2);
    require_table_or_col_acl(path_oid, ACL_SELECT, p_req_sel, 4);
    require_table_or_col_acl(path_oid, ACL_INSERT, p_req_sel, 4);
    require_table_or_col_acl(path_oid, ACL_UPDATE, (const char *[]){"tier", "mask", "path"}, 3);

    MemoryContext merge_ctx = AllocSetContextCreate(CurrentMemoryContext, "Native merge payload", ALLOCSET_DEFAULT_SIZES);
    MergeRowVec erows = {0}, prows = {0};
    MergeRowVec e_conc = {0}, p_conc = {0};
    MergeRowVec e_exist = {0}, p_exist = {0};
    stage_load_and_validate(stage_e, stage_p, ent_oid, path_oid, merge_ctx, &erows, &prows);

    /* Close stage relations before truncate while keeping ShareLock to EOT. */
    table_close(stage_e, NoLock);
    table_close(stage_p, NoLock);

    Oid e_idtype = get_atttype(ent_oid, get_attnum(ent_oid, "id"));
    Oid p_idtype = get_atttype(path_oid, get_attnum(path_oid, "entity"));
    const Oid *eparts = NULL, *epks = NULL, *pparts = NULL, *ppks = NULL;
    int neparts = lp_pg_canonical_partitions("entity", &eparts, &epks, NULL, NULL);
    int npparts = lp_pg_canonical_partitions("physicality", &pparts, &ppks, NULL, NULL);

    /* Try non-blocking per-prefix advisory locks before presence probing.
     * Parallel ingest lanes own disjoint id.b[0] prefixes and succeed without
     * contention, enabling batched table_multi_insert without speculative
     * double-pinning. Overlapping concurrent writers fall back immediately to
     * speculative insertion and preserve Lock|transactionid waiting semantics. */
    bool exclusive_buckets = !IsolationUsesXactSnapshot();
    size_t n = erows.n;
    if (n > 0) {
        if (exclusive_buckets) {
            uint8_t b0_seen[256] = {0};
            for (size_t i = 0; i < n; i++)
                if (!erows.v[i].present || !prows.v[i].present)
                    b0_seen[erows.v[i].id.b[0]] = 1;
            for (int b = 0; b < 256; b++) {
                if (!b0_seen[b]) continue;
                if (!DatumGetBool(DirectFunctionCall2(pg_try_advisory_xact_lock_int4,
                                                      Int32GetDatum(1279348804), Int32GetDatum(b)))) {
                    exclusive_buckets = false;
                    break;
                }
            }
        }
        lp_id *probe_ids = palloc(n * sizeof(lp_id));
        size_t *probe_idx = palloc(n * sizeof(size_t));
        bool *probe_res = palloc0(n * sizeof(bool));
        size_t np = 0;
        for (size_t i = 0; i < n; i++)
            if (!erows.v[i].present || !prows.v[i].present) {
                probe_ids[np] = erows.v[i].id;
                probe_idx[np++] = i;
            }
        if (np > 0) {
            lp_pg_present(probe_ids, np, probe_res);
            for (size_t k = 0; k < np; k++)
                if (probe_res[k]) {
                    erows.v[probe_idx[k]].present = true;
                    prows.v[probe_idx[k]].present = true;
                }
        }
        pfree(probe_ids); pfree(probe_idx); pfree(probe_res);
    }

    for (size_t i = 0; i < erows.n; i++) if (erows.v[i].present) lp_push(&e_exist, erows.v[i]);
    for (size_t i = 0; i < prows.n; i++) if (prows.v[i].present) lp_push(&p_exist, prows.v[i]);

    HASHCTL hctl = {.keysize = sizeof(Oid), .entrysize = sizeof(Oid)};
    HTAB *touched_parts = hash_create("Merge touched physicality partitions", Max(npparts, 1), &hctl, HASH_ELEM | HASH_BLOBS);

    /* Fail fast on known-row compatibility before inserting novel rows, then insert novel, then enrich concurrently landed. */
    Snapshot snap = GetActiveSnapshot();
    uint64 updated_rows = merge_enrich_subset(ent_parent, false, e_idtype, neparts, eparts, epks, e_exist.v, e_exist.n, snap, NULL);
    updated_rows += merge_enrich_subset(path_parent, true, p_idtype, npparts, pparts, ppks, p_exist.v, p_exist.n, snap, touched_parts);

    uint64 entity_rows = merge_insert_table(ent_parent, "entity", false, e_idtype, neparts, eparts, epks, erows.v, erows.n, &e_conc, NULL, exclusive_buckets);
    uint64 physicality_rows = merge_insert_table(path_parent, "physicality", true, p_idtype, npparts, pparts, ppks, prows.v, prows.n, &p_conc, touched_parts, exclusive_buckets);

    if (e_conc.n > 0 || p_conc.n > 0) {
        Snapshot latest = RegisterSnapshot(GetLatestSnapshot());
        PG_TRY(); {
            if (e_conc.n > 0)
                updated_rows += merge_enrich_subset(ent_parent, false, e_idtype, neparts, eparts, epks, e_conc.v, e_conc.n, latest, NULL);
            if (p_conc.n > 0)
                updated_rows += merge_enrich_subset(path_parent, true, p_idtype, npparts, pparts, ppks, p_conc.v, p_conc.n, latest, touched_parts);
        } PG_FINALLY(); {
            UnregisterSnapshot(latest);
        } PG_END_TRY();
    }

    int ntouched = hash_get_num_entries(touched_parts);
    Oid *pending_oids = palloc((Size)Max(ntouched, 1) * sizeof(Oid));
    int pi = 0;
    HASH_SEQ_STATUS hseq;
    hash_seq_init(&hseq, touched_parts);
    Oid *po;
    while ((po = hash_seq_search(&hseq))) pending_oids[pi++] = *po;
    hash_destroy(touched_parts);
    if (pi > 1) qsort(pending_oids, (size_t)pi, sizeof(Oid), lp_oid_cmp);

    ArrayType *pending_arr = NULL;
    if (defer_index && pi > 0) {
        Datum *od = palloc((Size)pi * sizeof(Datum));
        for (int i = 0; i < pi; i++) od[i] = ObjectIdGetDatum(pending_oids[i]);
        pending_arr = construct_array(od, pi, OIDOID, sizeof(Oid), true, TYPALIGN_INT);
        pfree(od);
    } else {
        if (!defer_index && pi > 0) clean_partition_oid_list(pending_oids, pi);
        pending_arr = construct_empty_array(OIDOID);
    }
    pfree(pending_oids);

    table_close(ent_parent, NoLock);
    table_close(path_parent, NoLock);
    lp_vec_free(&erows); lp_vec_free(&prows);
    lp_vec_free(&e_exist); lp_vec_free(&p_exist);
    lp_vec_free(&e_conc); lp_vec_free(&p_conc);
    MemoryContextDelete(merge_ctx);

    CommandCounterIncrement();
    lp_pg_truncate_rel(e_oid, p_oid);

    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_merge_stage requires composite result")));
    Datum vals[4] = {
        Int64GetDatum((int64)entity_rows),
        Int64GetDatum((int64)physicality_rows),
        Int64GetDatum((int64)updated_rows),
        pending_arr ? PointerGetDatum(pending_arr) : (Datum)0
    };
    bool nulls[4] = {false, false, false, pending_arr == NULL};
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), vals, nulls)));
}

/* ----------------------------------------------------- stage merge diagnostics */
typedef struct {
    lp_id id;
    int16 tier;
    lp_mask mask;
    int64 hilbert;
    bool tier_null, mask_null, hilbert_null, carrier_null, present;
    Datum carrier, mask_datum;
} DiagStageRow;
typedef lp_vec(DiagStageRow) DiagStageRowVec;

typedef struct {
    lp_id id;
    int64 count;
} DiagCountEntry;

typedef struct {
    lp_id id;
    bool found;
    int64 hilbert;
    Datum carrier;
    bytea *ewkb;
} DiagRecEntry;

typedef struct {
    char *reason;
    lp_id id;
    char *detail;
} DiagHit;

static int diag_hit_cmp(const void *a, const void *b)
{
    const DiagHit *x = a, *y = b;
    int c = strcmp(x->reason, y->reason);
    if (c) return c;
    c = lp_id_cmp(&x->id, &y->id);
    if (c) return c;
    return strcmp(x->detail, y->detail);
}

static char *diag_hex_prefix(const bytea *ewkb, size_t max_bytes)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = (size_t)VARSIZE_ANY_EXHDR(ewkb);
    if (n > max_bytes) n = max_bytes;
    char *out = palloc(2 * n + 1);
    const uint8 *src = (const uint8 *)VARDATA_ANY(ewkb);
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[src[i] >> 4];
        out[2 * i + 1] = hex[src[i] & 0x0f];
    }
    out[2 * n] = '\0';
    return out;
}

static void diag_entity_rec_cb(int64 ordinal, const lp_id *id, int16 tier, Datum coord, int64 hilbert, void *context)
{
    (void)ordinal; (void)tier;
    typedef struct { MemoryContext ctx; lp_idmap *map; } Ctx;
    Ctx *c = context;
    int64 at = lp_idmap_find(c->map, id);
    if (at < 0) return;
    DiagRecEntry *e = lp_idmap_at(c->map, (size_t)at);
    MemoryContext old = MemoryContextSwitchTo(c->ctx);
    bytea *raw = (bytea *)PG_DETOAST_DATUM(coord);
    Size sz = VARSIZE_ANY(raw);
    bytea *cp = palloc(sz);
    memcpy(cp, raw, sz);
    e->carrier = PointerGetDatum(cp);
    e->ewkb = lp_pg_path_ewkb(coord);
    e->hilbert = hilbert;
    e->found = true;
    MemoryContextSwitchTo(old);
}

static void diag_path_rec_cb(TupleTableSlot *slot, const PresenceKey *key, void *context)
{
    typedef struct { MemoryContext ctx; AttrNumber a_path; lp_idmap *map; } Ctx;
    Ctx *c = context;
    int64 at = lp_idmap_find(c->map, &key->id);
    if (at < 0) return;
    bool np = false;
    Datum dp = slot_getattr(slot, c->a_path, &np);
    if (np) ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("laplace_merge_diagnostics: null canonical path carrier")));
    DiagRecEntry *e = lp_idmap_at(c->map, (size_t)at);
    MemoryContext old = MemoryContextSwitchTo(c->ctx);
    bytea *raw = (bytea *)PG_DETOAST_DATUM(dp);
    Size sz = VARSIZE_ANY(raw);
    bytea *cp = palloc(sz);
    memcpy(cp, raw, sz);
    e->carrier = PointerGetDatum(cp);
    e->ewkb = lp_pg_path_ewkb(dp);
    e->found = true;
    MemoryContextSwitchTo(old);
}

static void diag_sample_path_cb(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    typedef struct { MemoryContext ctx; lp_idmap *map; } Ctx;
    Ctx *c = context;
    int64 at = lp_idmap_find(c->map, id);
    if (at < 0) return;
    DiagRecEntry *e = lp_idmap_at(c->map, (size_t)at);
    MemoryContext old = MemoryContextSwitchTo(c->ctx);
    e->ewkb = lp_pg_path_ewkb(path);
    e->found = true;
    MemoryContextSwitchTo(old);
}

static void diag_scan_stage_rows(Relation rel, bool is_paths, MemoryContext ctx,
                                 DiagStageRowVec *rows, lp_idmap *counts)
{
    Oid relid = RelationGetRelid(rel);
    AttrNumber a_id = get_attnum(relid, is_paths ? "entity" : "id");
    AttrNumber a_tier = get_attnum(relid, "tier");
    AttrNumber a_carrier = get_attnum(relid, is_paths ? "path" : "coord");
    AttrNumber a_hilb = is_paths ? InvalidAttrNumber : get_attnum(relid, "hilbert");
    AttrNumber a_mask = get_attnum(relid, "mask");
    AttrNumber a_pres = get_attnum(relid, "present");
    if (a_id == InvalidAttrNumber || a_tier == InvalidAttrNumber ||
        a_carrier == InvalidAttrNumber || (!is_paths && a_hilb == InvalidAttrNumber) || a_mask == InvalidAttrNumber)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("laplace_merge_diagnostics: missing required stage column")));
    bool use_pres = (a_pres != InvalidAttrNumber && get_atttype(relid, a_pres) == BOOLOID);

    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan(rel, snap, 0, NULL);
    TupleTableSlot *slot = table_slot_create(rel, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool ni = false, nt = false, nc = false, nh = true, nm = false, np = false;
        Datum di = slot_getattr(slot, a_id, &ni);
        if (ni) { ExecClearTuple(slot); continue; }
        Datum dt = slot_getattr(slot, a_tier, &nt);
        Datum dc = slot_getattr(slot, a_carrier, &nc);
        Datum dh = (!is_paths && a_hilb != InvalidAttrNumber) ? slot_getattr(slot, a_hilb, &nh) : (Datum)0;
        Datum dm = slot_getattr(slot, a_mask, &nm);
        bool pres = false;
        if (use_pres) {
            Datum dp = slot_getattr(slot, a_pres, &np);
            pres = !np && DatumGetBool(dp);
        }

        MemoryContext old = MemoryContextSwitchTo(ctx);
        DiagStageRow row = {
            .tier = nt ? -1 : DatumGetInt16(dt),
            .mask = nm ? 0 : lp_pg_mask_native(dm),
            .hilbert = nh ? 0 : DatumGetInt64(dh),
            .tier_null = nt, .mask_null = nm, .hilbert_null = nh,
            .carrier_null = nc, .present = pres
        };
        memcpy(row.id.b, DatumGetPointer(di), sizeof(lp_id));
        if (!nc) {
            bytea *raw = (bytea *)PG_DETOAST_DATUM(dc);
            Size sz = VARSIZE_ANY(raw);
            bytea *cp = palloc(sz);
            memcpy(cp, raw, sz);
            row.carrier = PointerGetDatum(cp);
        }
        bool fresh = false;
        size_t pos = lp_idmap_put(counts, &row.id, &fresh);
        DiagCountEntry *ce = lp_idmap_at(counts, pos);
        if (fresh) ce->id = row.id;
        ce->count++;
        lp_push(rows, row);
        MemoryContextSwitchTo(old);
        ExecClearTuple(slot);
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
}

static char *diag_format_point_zm(Datum coord)
{
    Geo g = geo_of(coord);
    const double *p = xyzm(&g);
    return psprintf("POINT ZM (%.15g %.15g %.15g %.15g)", p[0], p[1], p[2], p[3]);
}

PG_FUNCTION_INFO_V1(laplace_merge_diagnostics);
Datum laplace_merge_diagnostics(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *output = set_begin(fcinfo, "laplace_merge_diagnostics", &td);

    Oid e_oid = PG_GETARG_OID(0), p_oid = PG_GETARG_OID(1);
    Relation stage_e = try_relation_open(e_oid, AccessShareLock);
    Relation stage_p = try_relation_open(p_oid, AccessShareLock);
    if (!stage_e || !stage_p || e_oid == p_oid ||
        !stage_is_private_unlogged(stage_e, true) || !stage_is_private_unlogged(stage_p, true)) {
        if (stage_e) table_close(stage_e, NoLock);
        if (stage_p) table_close(stage_p, NoLock);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_merge_diagnostics requires distinct UNLOGGED stage input tables")));
    }
    require_table_or_col_acl(e_oid, ACL_SELECT, (const char *[]){"id", "tier", "coord", "hilbert", "mask"}, 5);
    require_table_or_col_acl(p_oid, ACL_SELECT, (const char *[]){"entity", "tier", "path", "mask"}, 4);

    Oid ent_oid = lp_pg_relation("entity"), path_oid = lp_pg_relation("physicality");
    const char *e_cols[] = {"id", "tier", "coord", "hilbert"};
    const char *p_cols[] = {"entity", "path"};
    const char *e_pk_col = NULL, *p_pk_col = NULL;
    Relation ent_parent = lp_pg_read_parent(ent_oid, &e_pk_col, e_cols, 4);
    Relation path_parent = lp_pg_read_parent(path_oid, &p_pk_col, p_cols, 2);

    MemoryContext diag_ctx = AllocSetContextCreate(CurrentMemoryContext, "Native merge diagnostics", ALLOCSET_DEFAULT_SIZES);
    DiagStageRowVec erows = {0}, prows = {0};
    lp_vec(DiagHit) hits = {0};
    lp_idmap *ecounts = lp_idmap_sized(sizeof(DiagCountEntry));
    lp_idmap *pcounts = lp_idmap_sized(sizeof(DiagCountEntry));
    lp_idmap *erec_map = lp_idmap_sized(sizeof(DiagRecEntry));
    lp_idmap *prec_map = lp_idmap_sized(sizeof(DiagRecEntry));

    PG_TRY(); {
        diag_scan_stage_rows(stage_e, false, diag_ctx, &erows, ecounts);
        diag_scan_stage_rows(stage_p, true, diag_ctx, &prows, pcounts);
        table_close(stage_e, NoLock); stage_e = NULL;
        table_close(stage_p, NoLock); stage_p = NULL;

        MemoryContext old = MemoryContextSwitchTo(diag_ctx);
        const lp_highway *hw = lp_pg_highway();

        /* 1. duplicate entity */
        for (size_t i = 0; i < lp_idmap_count(ecounts); i++) {
            const DiagCountEntry *ce = lp_idmap_at(ecounts, i);
            if (ce->count != 1)
                lp_push(&hits, ((DiagHit){.reason = "duplicate entity", .id = ce->id, .detail = psprintf("rows=%lld", (long long)ce->count)}));
        }
        /* 2. duplicate path */
        for (size_t i = 0; i < lp_idmap_count(pcounts); i++) {
            const DiagCountEntry *ce = lp_idmap_at(pcounts, i);
            if (ce->count != 1)
                lp_push(&hits, ((DiagHit){.reason = "duplicate path", .id = ce->id, .detail = psprintf("rows=%lld", (long long)ce->count)}));
        }
        /* 3. invalid entity carrier/tier */
        for (size_t i = 0; i < erows.n; i++) {
            const DiagStageRow *r = &erows.v[i];
            lp_mask dummy = 0;
            if (r->tier_null || r->mask_null || r->tier < 1 || r->tier > 255 || !lp_mask_merge(hw, r->mask, 0, &dummy)) {
                char *ms = r->mask_null ? "NULL" : psprintf("0x%016llx", (unsigned long long)r->mask);
                char *ts = r->tier_null ? "NULL" : psprintf("%d", (int)r->tier);
                lp_push(&hits, ((DiagHit){.reason = "invalid entity carrier/tier", .id = r->id, .detail = psprintf("tier=%s mask=%s", ts, ms)}));
            }
        }
        /* 4. invalid path carrier/tier */
        for (size_t i = 0; i < prows.n; i++) {
            const DiagStageRow *r = &prows.v[i];
            lp_mask dummy = 0;
            if (r->tier_null || r->mask_null || r->tier < 1 || r->tier > 255 || !lp_mask_merge(hw, r->mask, 0, &dummy)) {
                char *ms = r->mask_null ? "NULL" : psprintf("0x%016llx", (unsigned long long)r->mask);
                char *ts = r->tier_null ? "NULL" : psprintf("%d", (int)r->tier);
                lp_push(&hits, ((DiagHit){.reason = "invalid path carrier/tier", .id = r->id, .detail = psprintf("tier=%s mask=%s", ts, ms)}));
            }
        }

        /* Collect canonical entity lookup IDs for checks 5 & 8 */
        lp_vec(lp_id) e_probe = {0};
        for (size_t i = 0; i < erows.n; i++) {
            if (!erows.v[i].carrier_null || !erows.v[i].hilbert_null) {
                bool fresh = false;
                size_t at = lp_idmap_put(erec_map, &erows.v[i].id, &fresh);
                if (fresh) {
                    DiagRecEntry *e = lp_idmap_at(erec_map, at);
                    e->id = erows.v[i].id;
                    lp_push(&e_probe, erows.v[i].id);
                }
            }
        }
        if (e_probe.n > 0) {
            struct { MemoryContext ctx; lp_idmap *map; } ectx = {diag_ctx, erec_map};
            lp_pg_lookup_entity_coords(e_probe.v, e_probe.n, diag_entity_rec_cb, &ectx);
        }
        lp_vec_free(&e_probe);

        /* 5. entity identity conflict */
        for (size_t i = 0; i < erows.n; i++) {
            const DiagStageRow *s = &erows.v[i];
            if (s->carrier_null && s->hilbert_null) continue;
            int64 at = lp_idmap_find(erec_map, &s->id);
            if (at < 0) continue;
            const DiagRecEntry *d = lp_idmap_at(erec_map, (size_t)at);
            if (!d->found) continue;
            bool coord_eq = false, hilb_eq = (!s->hilbert_null && s->hilbert == d->hilbert);
            bytea *sewkb = NULL;
            if (!s->carrier_null) {
                sewkb = lp_pg_path_ewkb(s->carrier);
                coord_eq = (VARSIZE_ANY_EXHDR(sewkb) == VARSIZE_ANY_EXHDR(d->ewkb) &&
                            memcmp(VARDATA_ANY(sewkb), VARDATA_ANY(d->ewkb), (size_t)VARSIZE_ANY_EXHDR(sewkb)) == 0);
            }
            bool conflict = (!s->carrier_null && !coord_eq) || (!s->hilbert_null && !hilb_eq);
            if (conflict) {
                char *swkt = s->carrier_null ? "NULL" : diag_format_point_zm(s->carrier);
                char *dwkt = diag_format_point_zm(d->carrier);
                const char *ce_str = s->carrier_null ? "null" : (coord_eq ? "t" : "f");
                const char *he_str = s->hilbert_null ? "null" : (hilb_eq ? "t" : "f");
                lp_push(&hits, ((DiagHit){
                    .reason = "entity identity conflict", .id = s->id,
                    .detail = psprintf("coord_equal=%s hilbert_equal=%s staged=%s recorded=%s", ce_str, he_str, swkt, dwkt)
                }));
            }
        }

        /* Collect canonical physicality lookup IDs for checks 6 & 7 */
        for (size_t i = 0; i < prows.n; i++) {
            if (!prows.v[i].carrier_null) {
                bool fresh = false;
                size_t at = lp_idmap_put(prec_map, &prows.v[i].id, &fresh);
                if (fresh) { ((DiagRecEntry *)lp_idmap_at(prec_map, at))->id = prows.v[i].id; }
            }
        }
        for (size_t i = 0; i < erows.n; i++) {
            if (!erows.v[i].present && lp_idmap_find(pcounts, &erows.v[i].id) < 0) {
                bool fresh = false;
                size_t at = lp_idmap_put(prec_map, &erows.v[i].id, &fresh);
                if (fresh) { ((DiagRecEntry *)lp_idmap_at(prec_map, at))->id = erows.v[i].id; }
            }
        }
        size_t np_probe = lp_idmap_count(prec_map);
        if (np_probe > 0) {
            PresenceKey *pkeys = palloc(np_probe * sizeof(PresenceKey));
            for (size_t i = 0; i < np_probe; i++) {
                const DiagRecEntry *re = lp_idmap_at(prec_map, i);
                pkeys[i] = (PresenceKey){.id = re->id, .ordinal = (int64)i + 1};
            }
            struct { MemoryContext ctx; AttrNumber a_path; lp_idmap *map; } pctx = {
                diag_ctx, get_attnum(path_oid, "path"), prec_map
            };
            lp_pg_indexed_keys(path_parent, p_pk_col, pkeys, np_probe, diag_path_rec_cb, &pctx);
            pfree(pkeys);
        }

        /* 6. path identity conflict */
        for (size_t i = 0; i < prows.n; i++) {
            const DiagStageRow *s = &prows.v[i];
            if (s->carrier_null) continue;
            int64 at = lp_idmap_find(prec_map, &s->id);
            if (at < 0) continue;
            const DiagRecEntry *d = lp_idmap_at(prec_map, (size_t)at);
            if (!d->found) continue;
            bool ewkb_eq = false, enriched = false;
            bytea *sewkb = lp_pg_path_ewkb(s->carrier);
            ewkb_eq = (VARSIZE_ANY_EXHDR(sewkb) == VARSIZE_ANY_EXHDR(d->ewkb) &&
                       memcmp(VARDATA_ANY(sewkb), VARDATA_ANY(d->ewkb), (size_t)VARSIZE_ANY_EXHDR(sewkb)) == 0);
            if (!ewkb_eq) enriched = (lp_pg_path_enrich_datum(d->carrier, s->carrier) != (Datum)0);
            bool conflict = !ewkb_eq && !enriched;
            if (conflict) {
                int snv = 0, rnv = 0;
                lp_id *sk = lp_pg_path_keys(sewkb, &snv);
                lp_id *rk = lp_pg_path_keys(d->ewkb, &rnv);
                bool ids_eq = (snv == rnv && memcmp(sk, rk, (size_t)snv * sizeof(lp_id)) == 0);
                const char *ie_str = ids_eq ? "t" : "f";
                char *sprefix = diag_hex_prefix(sewkb, 160);
                char *rprefix = diag_hex_prefix(d->ewkb, 160);
                lp_push(&hits, ((DiagHit){
                    .reason = "path identity conflict", .id = s->id,
                    .detail = psprintf("ids_equal=%s staged_vertices=%d recorded_vertices=%d staged_prefix=%s recorded_prefix=%s",
                                       ie_str, snv, rnv, sprefix, rprefix)
                }));
            }
        }

        /* 7. entity without path */
        for (size_t i = 0; i < erows.n; i++) {
            const DiagStageRow *s = &erows.v[i];
            if (s->present || lp_idmap_find(pcounts, &s->id) >= 0) continue;
            int64 at = lp_idmap_find(prec_map, &s->id);
            if (at >= 0 && ((const DiagRecEntry *)lp_idmap_at(prec_map, (size_t)at))->found) continue;
            lp_push(&hits, ((DiagHit){.reason = "entity without path", .id = s->id, .detail = ""}));
        }

        /* 8. path without entity (probe canonical entity table via tiers: -1 = absent, even for Tier-0) */
        lp_vec(lp_id) miss_ent_ids = {0};
        lp_vec(size_t) miss_ent_row = {0};
        for (size_t i = 0; i < prows.n; i++) {
            const DiagStageRow *s = &prows.v[i];
            if (s->present || lp_idmap_find(ecounts, &s->id) >= 0) continue;
            lp_push(&miss_ent_ids, s->id);
            lp_push(&miss_ent_row, i);
        }
        if (miss_ent_ids.n > 0) {
            int16 *tiers = palloc(miss_ent_ids.n * sizeof(int16));
            lp_pg_lookup_entity_tiers(miss_ent_ids.v, miss_ent_ids.n, tiers);
            for (size_t k = 0; k < miss_ent_ids.n; k++) {
                if (tiers[k] < 0)
                    lp_push(&hits, ((DiagHit){.reason = "path without entity", .id = miss_ent_ids.v[k], .detail = ""}));
            }
        }
        lp_vec_free(&miss_ent_ids); lp_vec_free(&miss_ent_row);

        /* Order by (reason, id) and emit LIMIT 8 with recorded physicality EWKB */
        if (hits.n > 1) lp_sort(hits.v, hits.n, sizeof(DiagHit), diag_hit_cmp);
        size_t n_emit = Min(hits.n, (size_t)8);
        if (n_emit > 0) {
            lp_idmap *sample_paths = lp_idmap_sized(sizeof(DiagRecEntry));
            lp_vec(lp_id) sids = {0};
            for (size_t i = 0; i < n_emit; i++) {
                bool fresh = false;
                size_t at = lp_idmap_put(sample_paths, &hits.v[i].id, &fresh);
                if (fresh) {
                    ((DiagRecEntry *)lp_idmap_at(sample_paths, at))->id = hits.v[i].id;
                    lp_push(&sids, hits.v[i].id);
                }
            }
            struct { MemoryContext ctx; lp_idmap *map; } sctx = {diag_ctx, sample_paths};
            lp_pg_lookup_paths(sids.v, sids.n, false, diag_sample_path_cb, &sctx);
            for (size_t i = 0; i < n_emit; i++) {
                int64 at = lp_idmap_find(sample_paths, &hits.v[i].id);
                const DiagRecEntry *rec = (at >= 0) ? lp_idmap_at(sample_paths, (size_t)at) : NULL;
                bool has_ewkb = (rec && rec->found && rec->ewkb);
                Datum vals[4] = {
                    CStringGetTextDatum(hits.v[i].reason),
                    id_datum(&hits.v[i].id),
                    CStringGetTextDatum(hits.v[i].detail),
                    has_ewkb ? PointerGetDatum(rec->ewkb) : (Datum)0
                };
                bool nulls[4] = {false, false, false, !has_ewkb};
                set_put(fcinfo, output, td, vals, nulls);
                pfree(DatumGetPointer(vals[0]));
                pfree(DatumGetPointer(vals[1]));
                if (hits.v[i].detail[0] != '\0') pfree(DatumGetPointer(vals[2]));
            }
            lp_vec_free(&sids);
            lp_idmap_free(sample_paths);
        }
        MemoryContextSwitchTo(old);
    } PG_FINALLY(); {
        if (stage_e) table_close(stage_e, NoLock);
        if (stage_p) table_close(stage_p, NoLock);
        table_close(ent_parent, NoLock);
        table_close(path_parent, NoLock);
        lp_vec_free(&erows); lp_vec_free(&prows); lp_vec_free(&hits);
        lp_idmap_free(ecounts); lp_idmap_free(pcounts);
        lp_idmap_free(erec_map); lp_idmap_free(prec_map);
        MemoryContextDelete(diag_ctx);
    } PG_END_TRY();
    PG_RETURN_NULL();
}
