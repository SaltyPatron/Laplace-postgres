/* Route copied stage keys through the ordinary parent-table UPDATE. Each
 * parameter contains keys from one destination partition, so PostgreSQL can
 * prune the other targets without changing parent-table permissions. */
#include "pg_core.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/partcache.h"
#include "partitioning/partdesc.h"
#include "partitioning/partbounds.h"
#include <limits.h>

typedef struct { Oid partition; lp_id id; ItemPointerData source; } EnrichmentKey;

static long enrichment_fetch_rows(void)
{
    Size tuple = MAXALIGN(sizeof(HeapTupleData)) + MAXALIGN(SizeofHeapTupleHeader)
        + sizeof(HeapTuple) + MAXALIGN(sizeof(Oid) + sizeof(lp_id) + sizeof(ItemPointerData));
    /* The fetched tuples coexist with the copied routing keys. During an
     * UPDATE those tuples are gone; retain room for the array and its two
     * constant uses in the custom plan instead. Nothing limits input rows. */
    Size live = Max(tuple + sizeof(EnrichmentKey),
                    sizeof(EnrichmentKey) + sizeof(Datum) + 3 * (sizeof(lp_id) + sizeof(ItemPointerData)));
    uint64 budget = (uint64)work_mem * 1024;
    Size count = Max((uint64)1, budget / live);
    count = Min(count, MaxAllocSize / sizeof(EnrichmentKey));
    count = Min(count, MaxAllocSize / sizeof(Datum));
    count = Min(count, (MaxAllocSize - ARR_OVERHEAD_NONULLS(1)) / sizeof(lp_id));
    return (long)Min(count, (Size)INT_MAX);
}

static ArrayType *enrichment_array(Oid type, const EnrichmentKey *keys, int count, bool locations)
{
    Datum *values = palloc(sizeof(Datum) * count);
    for (int i = 0; i < count; i++) values[i] = locations
        ? PointerGetDatum(&keys[i].source) : PointerGetDatum(&keys[i].id);
    ArrayType *ids = construct_array(values, count, type,
        locations ? sizeof(ItemPointerData) : sizeof(lp_id), false,
        locations ? TYPALIGN_SHORT : TYPALIGN_CHAR);
    pfree(values);
    return ids;
}

PG_FUNCTION_INFO_V1(laplace_enrich_stage);
Datum laplace_enrich_stage(PG_FUNCTION_ARGS)
{
    Oid input = PG_GETARG_OID(0), target = PG_GETARG_OID(1);
    Oid public = get_namespace_oid("public", false);
    bool paths = target == get_relname_relid("physicality", public);
    if (!paths && target != get_relname_relid("entity", public))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                       errmsg("laplace_enrich_stage: target must be public.entity or public.physicality")));
    /* Keep the copied input immutable while its tuple locations are used.
     * This does not lock destination rows or alter the caller's transaction. */
    Relation stage = table_open(input, AccessShareLock);
    if (RelationGetNamespace(stage) != get_namespace_oid("stage", false)
        || stage->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED
        || stage->rd_rel->relkind != RELKIND_RELATION)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                       errmsg("laplace_enrich_stage requires an UNLOGGED stage input table")));
    List *inputs = find_all_inheritors(input, ShareLock, NULL);
    list_free(inputs);
    const char *key = paths ? "entity" : "id";
    Oid idtype = get_atttype(target, get_attnum(target, key)), idarray = get_array_type(idtype);
    int16 key_bytes; bool key_by_value; char key_alignment;
    get_typlenbyvalalign(idtype, &key_bytes, &key_by_value, &key_alignment);
    if (get_atttype(input, get_attnum(input, key)) != idtype
        || key_bytes != sizeof(lp_id) || key_by_value || !OidIsValid(idarray))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                       errmsg("laplace_enrich_stage requires matching fixed-width Native IDs")));
    Relation parent = table_open(target, AccessShareLock);
    PartitionKey partition_key = RelationGetPartitionKey(parent);
    bool range = partition_key && partition_key->strategy == PARTITION_STRATEGY_RANGE
        && partition_key->partnatts == 1 && partition_key->partattrs[0] == get_attnum(target, key);
    PartitionDesc partitions = range ? RelationGetPartitionDesc(parent, true) : NULL;
    char *source = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(stage)), RelationGetRelationName(stage));
    char *destination = quote_qualified_identifier("public", paths ? "physicality" : "entity");
    const char *changed = paths
        ? "(d.tier>s.tier OR (d.mask|s.mask)<>d.mask OR (ST_AsEWKB(d.path)<>ST_AsEWKB(s.path)"
          " AND ST_AsEWKB(laplace_path_enrich(d.path,s.path))<>ST_AsEWKB(d.path)))"
        : "(d.tier>s.tier OR (d.mask|s.mask)<>d.mask)";
    /* Do not join the entire destination merely to discover target leaves.
     * PostgreSQL's own range-bound search routes the copied keys in C. */
    char *select = psprintf("SELECT s.%s,s.ctid FROM %s s ORDER BY s.%s", key, source, key);
    char *update = psprintf("UPDATE %s d SET tier=least(d.tier,s.tier),mask=laplace_mask_merge(d.mask,s.mask)%s"
                           " FROM %s s WHERE d.%s=s.%s AND d.%s=ANY($1) AND s.ctid=ANY($2) AND %s",
                           destination, paths ? ",path=CASE WHEN ST_AsEWKB(d.path)<>ST_AsEWKB(s.path)"
                           " THEN laplace_path_enrich(d.path,s.path) ELSE d.path END" : "",
                           source, key, key, key, changed);
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("laplace_enrich_stage: SPI_connect")));
    /* A set covering the destination should use the original full set join,
     * rather than many indexed destination groups. Compare analyzed row
     * cardinalities, not a fixed batch size or a configurable cutoff. Unknown
     * destination statistics leave routing in effect. */
    Oid relation_type = REGCLASSOID; Datum relation = ObjectIdGetDatum(target);
    if (SPI_execute_with_args("SELECT CASE WHEN min(c.reltuples)<0 THEN NULL ELSE sum(c.reltuples::float8) END"
            " FROM pg_partition_tree($1) p JOIN pg_class c ON c.oid=p.relid WHERE p.isleaf",
            1, &relation_type, &relation, NULL, true, 1) != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("laplace_enrich_stage: destination statistics failed")));
    bool unknown;
    Datum destination_rows = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &unknown);
    bool complete_set = !unknown && stage->rd_rel->reltuples >= 0
        && stage->rd_rel->reltuples >= DatumGetFloat8(destination_rows);
    SPI_freetuptable(SPI_tuptable);
    if (complete_set) {
        char *whole = psprintf("UPDATE %s d SET tier=least(d.tier,s.tier),mask=laplace_mask_merge(d.mask,s.mask)%s"
                              " FROM %s s WHERE d.%s=s.%s AND %s", destination,
                              paths ? ",path=CASE WHEN ST_AsEWKB(d.path)<>ST_AsEWKB(s.path)"
                              " THEN laplace_path_enrich(d.path,s.path) ELSE d.path END" : "",
                              source, key, key, changed);
        if (SPI_execute(whole, false, 0) != SPI_OK_UPDATE)
            ereport(ERROR, (errmsg("laplace_enrich_stage: complete-set update failed")));
        uint64 updated = SPI_processed;
        SPI_finish(); table_close(stage, NoLock); table_close(parent, NoLock);
        pfree(whole); pfree(source); pfree(destination); pfree(select); pfree(update);
        PG_RETURN_INT64((int64)updated);
    }
    /* A custom UPDATE sees the actual partition-local ID array. It has the
     * same parent permissions and latest-tuple enrichment guard as before. */
    Oid types[2] = {idarray, TIDARRAYOID};
    SPIPlanPtr apply = SPI_prepare_cursor(update, 2, types, CURSOR_OPT_CUSTOM_PLAN);
    SPIPlanPtr selection = SPI_prepare_cursor(select, 0, NULL, CURSOR_OPT_NO_SCROLL);
    if (!apply || !selection)
        ereport(ERROR, (errmsg("laplace_enrich_stage: plan failed")));
    /* An unchanged set must still enforce the original UPDATE privileges,
     * including legitimate column grants, before skipping its execution. */
    ListCell *plan_cell;
    foreach(plan_cell, SPI_plan_get_plan_sources(apply)) {
        CachedPlanSource *source_plan = lfirst(plan_cell);
        ListCell *query_cell;
        foreach(query_cell, source_plan->query_list) {
            Query *query = lfirst(query_cell);
            ExecCheckPermissions(query->rtable, query->rteperminfos, true);
        }
    }
    Portal scan = SPI_cursor_open(NULL, selection, NULL, NULL, true);
    if (!scan) ereport(ERROR, (errmsg("laplace_enrich_stage: selection failed")));
    uint64 updated = 0;
    long fetch = enrichment_fetch_rows();
    for (;;) {
        SPI_cursor_fetch(scan, true, fetch);
        uint64 count = SPI_processed;
        SPITupleTable *tuples = SPI_tuptable;
        if (!count) { SPI_freetuptable(tuples); break; }
        /* SPI may process relcache invalidations between fetches. Keep no
         * partition-bound pointer across an executor call. */
        if (range) {
            partition_key = RelationGetPartitionKey(parent);
            partitions = RelationGetPartitionDesc(parent, true);
        }
        EnrichmentKey *keys = palloc(sizeof(*keys) * count);
        for (uint64 i = 0; i < count; i++) {
            bool missing;
            Datum id = SPI_getbinval(tuples->vals[i], tuples->tupdesc, 1, &missing);
            if (missing) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_enrich_stage: null Native ID")));
            memcpy(keys[i].id.b, DatumGetPointer(id), sizeof(lp_id));
            Datum location = SPI_getbinval(tuples->vals[i], tuples->tupdesc, 2, &missing);
            if (missing) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_enrich_stage: null input location")));
            memcpy(&keys[i].source, DatumGetPointer(location), sizeof(ItemPointerData));
            keys[i].partition = target;
            if (range) {
                Datum value = PointerGetDatum(&keys[i].id);
                bool equal;
                int bound = partition_range_datum_bsearch(partition_key->partsupfunc,
                    partition_key->partcollation, partitions->boundinfo, 1, &value, &equal);
                int index = partitions->boundinfo->indexes[bound + 1];
                if (index < 0) index = partitions->boundinfo->default_index;
                if (index >= 0) keys[i].partition = partitions->oids[index];
            }
        }
        /* Nested SPI execution replaces SPI_tuptable. Copy routing keys and
         * release the fetched tuples before applying any destination set. */
        SPI_freetuptable(tuples);
        for (uint64 begin = 0; begin < count;) {
            CHECK_FOR_INTERRUPTS();
            uint64 end = begin + 1;
            while (end < count && keys[end].partition == keys[begin].partition) end++;
            ArrayType *ids = enrichment_array(idtype, keys + begin, (int)(end - begin), false);
            ArrayType *locations = enrichment_array(TIDOID, keys + begin, (int)(end - begin), true);
            Datum parameters[2] = {PointerGetDatum(ids), PointerGetDatum(locations)};
            if (SPI_execute_plan(apply, parameters, NULL, false, 0) != SPI_OK_UPDATE)
                ereport(ERROR, (errmsg("laplace_enrich_stage: update failed")));
            updated += SPI_processed;
            pfree(ids); pfree(locations);
            begin = end;
        }
        pfree(keys);
    }
    SPI_cursor_close(scan);
    SPI_freeplan(selection); SPI_freeplan(apply); SPI_finish();
    table_close(stage, NoLock);
    table_close(parent, NoLock);
    pfree(source); pfree(destination); pfree(select); pfree(update);
    PG_RETURN_INT64((int64)updated);
}
