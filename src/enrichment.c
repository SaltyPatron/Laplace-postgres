/* Stage presence, carrier compatibility, partition-directed metadata updates,
 * and partition GIN pending-list maintenance. */
#include "pg_core.h"
#include "geo.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "executor/executor.h"
#include "partitioning/partdesc.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/partcache.h"
#include "utils/plancache.h"
#include "utils/rel.h"
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

typedef struct {
    FunctionCallInfo call;Tuplestorestate *rows;TupleDesc descriptor;Relation parent;
    const char *column;PresenceKey *keys;int used,capacity;int64 ordinal;
} PresenceBatch;
static void presence_flush(PresenceBatch *batch)
{
    if(!batch->used)return;
    lp_pg_presence_keys(batch->call,batch->rows,batch->descriptor,batch->parent,batch->column,batch->keys,batch->used);
    batch->used=0;
}
static void presence_stage_row(TupleTableSlot *slot,void *context)
{
    PresenceBatch *batch=context;bool missing;Datum id=slot_getattr(slot,1,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),errmsg("laplace_present_stage: null Native ID")));
    batch->keys[batch->used++]=(PresenceKey){.id=*(const lp_id *)DatumGetPointer(id),.ordinal=++batch->ordinal};
    if(batch->used==batch->capacity)presence_flush(batch);
}

PG_FUNCTION_INFO_V1(laplace_present_stage);
Datum laplace_present_stage(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;Tuplestorestate *output=set_begin(fcinfo,"laplace_present_stage",&descriptor);
    Oid input=PG_GETARG_OID(0),target=PG_GETARG_OID(1);
    const char *id_column=NameStr(*PG_GETARG_NAME(2)),*present_column=NameStr(*PG_GETARG_NAME(3));
    Relation stage=table_open(input,ShareLock);
    bool local=RelationGetNamespace(stage)==get_namespace_oid("stage",true)
        && stage->rd_rel->relpersistence==RELPERSISTENCE_UNLOGGED;
    local=local || (isTempNamespace(RelationGetNamespace(stage)) && stage->rd_rel->relpersistence==RELPERSISTENCE_TEMP);
    if(!local || stage->rd_rel->relkind!=RELKIND_RELATION
        || get_atttype(input,get_attnum(input,present_column))!=BOOLOID)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_present_stage requires a private stage ID set and boolean presence column")));
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("laplace_present_stage: SPI_connect")));
    const char *column;Relation parent=lp_pg_presence_parent(target,&column,1);
    if(get_atttype(input,get_attnum(input,id_column))!=get_atttype(target,get_attnum(target,column)))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_present_stage requires Native content IDs")));
    char *source=quote_qualified_identifier(get_namespace_name(RelationGetNamespace(stage)),RelationGetRelationName(stage));
    char *sql=psprintf("SELECT %s FROM %s WHERE %s IS NOT TRUE",quote_identifier(id_column),source,quote_identifier(present_column));
    SPIPlanPtr plan=lp_pg_prepare_cursor(sql,0,NULL,CURSOR_OPT_NO_SCROLL);
    if(!plan)ereport(ERROR,(errmsg("laplace_present_stage: stage plan failed")));
    int capacity=(int)enrichment_fetch_rows();
    PresenceBatch batch={fcinfo,output,descriptor,parent,column,palloc(sizeof(PresenceKey)*(Size)capacity),0,capacity,0};
    lp_pg_stream(plan,NULL,presence_stage_row,&batch);presence_flush(&batch);
    pfree(batch.keys);SPI_freeplan(plan);pfree(sql);pfree(source);
    table_close(parent,NoLock);table_close(stage,NoLock);SPI_finish();PG_RETURN_NULL();
}

/* Validate an existing carrier while its metadata update already addresses
 * that ID. This is not a presence test and never retrieves coordinates over
 * the client connection. Unrelated join candidates cannot raise an error. */
PG_FUNCTION_INFO_V1(laplace_stage_compatible);
Datum laplace_stage_compatible(PG_FUNCTION_ARGS)
{
    if(PG_ARGISNULL(0) || PG_ARGISNULL(1))PG_RETURN_BOOL(false);
    if(!lp_id_eq((const lp_id *)PG_GETARG_POINTER(0),(const lp_id *)PG_GETARG_POINTER(1)))PG_RETURN_BOOL(true);
    if(!PG_ARGISNULL(5) && (PG_ARGISNULL(4) || PG_GETARG_INT64(4)!=PG_GETARG_INT64(5)))
        ereport(ERROR,(errcode(ERRCODE_CHECK_VIOLATION),errmsg("laplace_merge_stage: conflicting identity Hilbert key")));
    if(PG_ARGISNULL(3))PG_RETURN_BOOL(true);
    if(PG_ARGISNULL(2) || PG_ARGISNULL(6))
        ereport(ERROR,(errcode(ERRCODE_CHECK_VIOLATION),errmsg("laplace_merge_stage: invalid carrier comparison")));
    FmgrInfo *send=fcinfo->flinfo->fn_extra;
    if(!send){
        Oid function;bool variable;
        getTypeBinaryOutputInfo(get_fn_expr_argtype(fcinfo->flinfo,2),&function,&variable);
        send=MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,sizeof *send);
        fmgr_info_cxt(function,send,fcinfo->flinfo->fn_mcxt);fcinfo->flinfo->fn_extra=send;
    }
    bytea *a=SendFunctionCall(send,PG_GETARG_DATUM(2)),*b=SendFunctionCall(send,PG_GETARG_DATUM(3));
    bool equal=VARSIZE_ANY_EXHDR(a)==VARSIZE_ANY_EXHDR(b)
        && !memcmp(VARDATA_ANY(a),VARDATA_ANY(b),VARSIZE_ANY_EXHDR(a));
    bool valid=equal || (PG_GETARG_BOOL(6) && lp_ewkb_enrich_path(
        lp_path_of((const uint8 *)VARDATA_ANY(a),VARSIZE_ANY_EXHDR(a)),
        lp_path_of((const uint8 *)VARDATA_ANY(b),VARSIZE_ANY_EXHDR(b)),NULL,0));
    pfree(a);pfree(b);
    if(!valid)ereport(ERROR,(errcode(ERRCODE_CHECK_VIOLATION),errmsg("laplace_merge_stage: conflicting identity data or incompatible path annotations")));
    PG_RETURN_BOOL(true);
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

typedef struct {
    Oid target,idtype;Relation parent;SPIPlanPtr apply;EnrichmentKey *keys;
    size_t used,capacity;bool range;uint64 updated;
} EnrichmentBatch;
static int enrichment_order(const void *a,const void *b)
{
    return memcmp(((const EnrichmentKey *)a)->id.b,((const EnrichmentKey *)b)->id.b,sizeof(lp_id));
}
static void enrichment_flush(EnrichmentBatch *batch)
{
    if(!batch->used)return;
    PartitionKey key=batch->range?RelationGetPartitionKey(batch->parent):NULL;
    PartitionDesc partitions=batch->range?RelationGetPartitionDesc(batch->parent,true):NULL;
    for(size_t i=0;i<batch->used;i++)batch->keys[i].partition=lp_pg_enrichment_partition(batch->target,key,partitions,PointerGetDatum(&batch->keys[i].id));
    lp_sort(batch->keys,batch->used,sizeof(*batch->keys),enrichment_order);
    /* The reader remains one executor pass. Each bounded partition set is
     * updated in a nested SPI frame, preserving the reader's result state. */
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("laplace_enrich_stage: writer SPI_connect")));
    PG_TRY();{
        for(size_t begin=0;begin<batch->used;){
            CHECK_FOR_INTERRUPTS();size_t end=begin+1;
            while(end<batch->used && batch->keys[end].partition==batch->keys[begin].partition)end++;
            ArrayType *ids=enrichment_array(batch->idtype,batch->keys+begin,(int)(end-begin),false);
            ArrayType *locations=enrichment_array(TIDOID,batch->keys+begin,(int)(end-begin),true);
            Datum parameters[]={PointerGetDatum(ids),PointerGetDatum(locations)};
            if(SPI_execute_plan(batch->apply,parameters,NULL,false,0)!=SPI_OK_UPDATE)
                ereport(ERROR,(errmsg("laplace_enrich_stage: update failed")));
            batch->updated+=SPI_processed;pfree(ids);pfree(locations);begin=end;
        }
    }PG_FINALLY();{SPI_finish();}PG_END_TRY();
    batch->used=0;
}
static void enrichment_stage_row(TupleTableSlot *slot,void *context)
{
    EnrichmentBatch *batch=context;bool missing;Datum id=slot_getattr(slot,1,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),errmsg("laplace_enrich_stage: null Native ID")));
    EnrichmentKey *key=batch->keys+batch->used++;key->id=*(const lp_id *)DatumGetPointer(id);
    Datum location=slot_getattr(slot,2,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),errmsg("laplace_enrich_stage: null input location")));
    key->source=*(const ItemPointerData *)DatumGetPointer(location);
    if(batch->used==batch->capacity)enrichment_flush(batch);
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
    char *source = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(stage)), RelationGetRelationName(stage));
    char *destination = quote_qualified_identifier(get_namespace_name(schema), paths ? "physicality" : "entity");
    char *proof_name=NULL,*restriction=pstrdup("");
    if(PG_NARGS()>2){
        Oid selected=PG_GETARG_OID(2);Relation proof=table_open(selected,ShareLock);
        bool private=(isTempNamespace(RelationGetNamespace(proof)) && proof->rd_rel->relpersistence==RELPERSISTENCE_TEMP)
            || (RelationGetNamespace(proof)==get_namespace_oid("stage",false) && proof->rd_rel->relpersistence==RELPERSISTENCE_UNLOGGED);
        bool direct=selected==input;
        const char *flag=direct?"present":paths?"path_present":"entity_present";
        if(!private || proof->rd_rel->relkind!=RELKIND_RELATION
            || get_atttype(selected,get_attnum(selected,direct?key:"id"))!=idtype
            || get_atttype(selected,get_attnum(selected,flag))!=BOOLOID || PG_GETARG_INT64(3)<0)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_enrich_stage requires a private classified ID set")));
        proof_name=quote_qualified_identifier(get_namespace_name(RelationGetNamespace(proof)),RelationGetRelationName(proof));
        pfree(restriction);restriction=direct?pstrdup(" AND s.present"):
            psprintf(" AND EXISTS (SELECT FROM %s classified WHERE classified.id OPERATOR(@extschema@.=) s.%s AND classified.%s)",proof_name,key,flag);
        table_close(proof,NoLock);
    }
    const char *changed = paths
        ? "(d.tier>s.tier OR (d.mask|s.mask)<>d.mask OR CASE WHEN s.path IS NULL THEN false ELSE (@postgis@.ST_AsEWKB(d.path)<>@postgis@.ST_AsEWKB(s.path)"
          " AND @postgis@.ST_AsEWKB(@extschema@.laplace_path_enrich(d.path,s.path))<>@postgis@.ST_AsEWKB(d.path)) END)"
        : "(d.tier>s.tier OR (d.mask|s.mask)<>d.mask)";
    /* Do not join the entire destination merely to discover target leaves.
     * PostgreSQL's own range-bound search routes the copied keys in C. */
    char *compatible=proof_name?psprintf("@extschema@.laplace_stage_compatible(d.%s,s.%s,d.%s,s.%s,d.hilbert,s.hilbert,%s)",
        key,key,paths?"path":"coord",paths?"path":"coord",paths?"true":"false"):pstrdup("true");
    char *needed=proof_name?psprintf("(%s OR NOT %s)",changed,compatible):pstrdup(changed);
    char *select = psprintf("SELECT s.%s,s.ctid FROM %s s WHERE true%s", key, source, restriction);
    char *update = psprintf("UPDATE %s d SET tier=CASE WHEN %s THEN least(d.tier,s.tier) ELSE d.tier END,mask=@extschema@.laplace_mask_merge(d.mask,s.mask)%s"
                           " FROM %s s WHERE d.%s OPERATOR(@extschema@.=) s.%s AND d.%s OPERATOR(@extschema@.=) ANY($1) AND s.ctid=ANY($2) AND %s%s",
                           destination,compatible, paths ? ",path=CASE WHEN s.path IS NULL THEN d.path WHEN @postgis@.ST_AsEWKB(d.path)<>@postgis@.ST_AsEWKB(s.path)"
                           " THEN @extschema@.laplace_path_enrich(d.path,s.path) ELSE d.path END" : "",
                           source, key, key, key, needed,restriction);
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("laplace_enrich_stage: SPI_connect")));
    /* A custom UPDATE sees the actual partition-local ID array. It has the
     * same parent permissions and latest-tuple enrichment guard as before. */
    Oid types[2] = {idarray, TIDARRAYOID};
    SPIPlanPtr apply = lp_pg_prepare_cursor(update, 2, types, CURSOR_OPT_CUSTOM_PLAN);
    SPIPlanPtr selection = lp_pg_prepare_cursor(select, 0, NULL, CURSOR_OPT_NO_SCROLL);
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
    size_t capacity=(size_t)enrichment_fetch_rows();
    EnrichmentBatch batch={target,idtype,parent,apply,palloc(sizeof(EnrichmentKey)*capacity),0,capacity,range,0};
    lp_pg_stream(selection,NULL,enrichment_stage_row,&batch);enrichment_flush(&batch);
    uint64 updated=batch.updated;pfree(batch.keys);
    SPI_freeplan(selection);SPI_freeplan(apply);SPI_finish();
    table_close(stage, NoLock);
    table_close(parent, NoLock);
    pfree(source); pfree(destination); pfree(select); pfree(update);
    PG_RETURN_INT64((int64)updated);
}

/* Complete bulk index maintenance before repeated containment reads.
 * Route copied IDs through the same Native partition bounds as enrichment;
 * neither scan public paths again nor sweep unrelated destination indexes. */
PG_FUNCTION_INFO_V1(laplace_clean_stage);
PG_FUNCTION_INFO_V1(laplace_stage_partitions);
PG_FUNCTION_INFO_V1(laplace_clean_partitions);
typedef struct { Oid target;Relation parent;HTAB *touched; } CleanStage;
static void clean_stage_row(TupleTableSlot *slot,void *context)
{
    CleanStage *stage=context;bool missing;Datum id=slot_getattr(slot,1,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),errmsg("laplace_clean_stage: null Native ID")));
    Oid partition=lp_pg_enrichment_partition(stage->target,RelationGetPartitionKey(stage->parent),RelationGetPartitionDesc(stage->parent,true),id);
    hash_search(stage->touched,&partition,HASH_ENTER,NULL);
}
static ArrayType *stage_partition_ids(Oid input)
{
    MemoryContext caller=CurrentMemoryContext;
    Relation stage = table_open(input, AccessShareLock);
    Oid target = lp_pg_relation("physicality");
    Relation parent = table_open(target, AccessShareLock);
    Oid idtype = get_atttype(target, get_attnum(target, "entity"));
    if (RelationGetNamespace(stage) != get_namespace_oid("stage", false)
        || stage->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED
        || stage->rd_rel->relkind != RELKIND_RELATION
        || get_atttype(input, get_attnum(input, "entity")) != idtype)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_clean_stage requires an UNLOGGED stage table with Native entity IDs")));
    List *inputs = find_all_inheritors(input, ShareLock, NULL);
    list_free(inputs);
    PartitionKey key = RelationGetPartitionKey(parent);
    if (!key || key->strategy != PARTITION_STRATEGY_RANGE || key->partnatts != 1
        || key->partattrs[0] != get_attnum(target, "entity"))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_clean_stage requires Native physicality range partitions")));
    char *source = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(stage)), RelationGetRelationName(stage));
    char *selection = psprintf("SELECT entity FROM %s", source);
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("laplace_clean_stage: SPI_connect")));
    HASHCTL control = {.keysize=sizeof(Oid), .entrysize=sizeof(Oid)};
    HTAB *touched = hash_create("Native copied path partitions",
        Max(RelationGetPartitionDesc(parent, true)->nparts, 1), &control, HASH_ELEM | HASH_BLOBS);
    SPIPlanPtr plan = lp_pg_prepare_cursor(selection, 0, NULL, CURSOR_OPT_NO_SCROLL);
    if (!plan) ereport(ERROR, (errmsg("laplace_clean_stage: selection plan failed")));
    CleanStage scan={target,parent,touched};lp_pg_stream(plan,NULL,clean_stage_row,&scan);SPI_freeplan(plan);
    int count = hash_get_num_entries(touched), at = 0;
    Datum *values = MemoryContextAlloc(caller,sizeof(Datum) * Max(count, 1));
    HASH_SEQ_STATUS entries; hash_seq_init(&entries, touched); Oid *partition;
    while ((partition = hash_seq_search(&entries))) values[at++] = ObjectIdGetDatum(*partition);
    hash_destroy(touched);SPI_finish();
    table_close(stage, NoLock);table_close(parent, NoLock);
    pfree(source);pfree(selection);
    ArrayType *ids=construct_array(values,count,OIDOID,sizeof(Oid),true,TYPALIGN_INT);
    pfree(values);return ids;
}

/* Deferred merges return storage identities, never assumptions about a hash
 * prefix or partition count. The same restricted authority drains the union
 * once all child waves in a working window have committed. */
static int64 clean_partition_ids(ArrayType *ids)
{
    Relation parent=table_open(lp_pg_relation("physicality"),AccessShareLock);
    if(!RelationGetPartitionKey(parent))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_clean_partitions requires partitioned Native physicality")));
    PartitionDesc parts=RelationGetPartitionDesc(parent,true);
    HASHCTL control={.keysize=sizeof(Oid),.entrysize=sizeof(Oid)};
    HTAB *allowed=hash_create("Native maintenance partitions",Max(parts->nparts,1),&control,HASH_ELEM|HASH_BLOBS);
    for(int i=0;i<parts->nparts;i++)hash_search(allowed,&parts->oids[i],HASH_ENTER,NULL);
    ArrayIterator iterator=array_create_iterator(ids,0,NULL);Datum item;bool missing;
    while(array_iterate(iterator,&item,&missing)){
        Oid partition=missing?InvalidOid:DatumGetObjectId(item);
        if(missing || !hash_search(allowed,&partition,HASH_FIND,NULL))
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_clean_partitions requires Native physicality partitions")));
    }
    array_free_iterator(iterator);hash_destroy(allowed);
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("laplace_clean_partitions: SPI_connect")));
    Oid type = OIDARRAYOID; Datum parameter = PointerGetDatum(ids);
    if (lp_pg_execute_args("SELECT @extschema@.laplace_clean_pending_list(i.indexrelid)"
        " FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class c ON c.oid=i.indexrelid"
        " WHERE i.indrelid=ANY($1) AND c.relam=" CppAsString2(GIN_AM_OID)
        " ORDER BY i.indexrelid", 1, &type, &parameter, NULL, false, 0) != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("laplace_clean_partitions: index maintenance failed")));
    int64 cleaned = 0;
    for (uint64 row = 0; row < SPI_processed; row++) {
        bool missing;
        Datum pages = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &missing);
        if (!missing) cleaned += DatumGetInt64(pages);
    }
    SPI_freetuptable(SPI_tuptable);SPI_finish();table_close(parent,NoLock);
    return cleaned;
}

Datum laplace_stage_partitions(PG_FUNCTION_ARGS)
{
    PG_RETURN_ARRAYTYPE_P(stage_partition_ids(PG_GETARG_OID(0)));
}

Datum laplace_clean_partitions(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(clean_partition_ids(PG_GETARG_ARRAYTYPE_P(0)));
}

Datum laplace_clean_stage(PG_FUNCTION_ARGS)
{
    ArrayType *ids=stage_partition_ids(PG_GETARG_OID(0));
    int64 cleaned=clean_partition_ids(ids);pfree(ids);PG_RETURN_INT64(cleaned);
}
