/* Route copied stage keys through the ordinary parent-table UPDATE. Each
 * parameter contains keys from one destination partition, so PostgreSQL can
 * prune the other targets without changing parent-table permissions. */
#include "pg_core.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/hsearch.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/partcache.h"
#include "laplace/geometry.h"
#include "partitioning/partdesc.h"
#include "partitioning/partbounds.h"
#include <limits.h>

typedef struct { Oid partition; lp_id id; int64 ordinal; } PresenceKey;
static long enrichment_fetch_rows(void);
static int presence_order(const void *a,const void *b)
{
    const PresenceKey *x=a,*y=b;
    if(x->partition!=y->partition)return x->partition<y->partition?-1:1;
    int compare=memcmp(x->id.b,y->id.b,sizeof(lp_id));
    return compare?compare:(x->ordinal>y->ordinal)-(x->ordinal<y->ordinal);
}

typedef struct { Oid partition; lp_id id; ItemPointerData source; } EnrichmentKey;

static Oid enrichment_partition(Oid target, PartitionKey key, PartitionDesc partitions, Datum value)
{
    if (partitions) {
        bool equal;
        int bound = partition_range_datum_bsearch(key->partsupfunc,
            key->partcollation, partitions->boundinfo, 1, &value, &equal);
        int index = partitions->boundinfo->indexes[bound + 1];
        if (index < 0) index = partitions->boundinfo->default_index;
        if (index >= 0) return partitions->oids[index];
    }
    return target;
}

/* Shared indexed membership kernel for client frontiers and the final flat
 * overlap verification. Neither path reads coordinates to establish identity. */
static void presence_keys(FunctionCallInfo fcinfo,Tuplestorestate *output,TupleDesc descriptor,
                          Relation parent,const char *column,PresenceKey *keys,int count)
{
    Oid target=RelationGetRelid(parent);
    AttrNumber attribute=get_attnum(target,column);
    PartitionKey key=RelationGetPartitionKey(parent);
    if(!key || key->strategy!=PARTITION_STRATEGY_RANGE || key->partnatts!=1
        || key->partattrs[0]!=attribute)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native presence requires canonical ID range partitions")));
    PartitionDesc partitions=RelationGetPartitionDesc(parent,true);
    for(int i=0;i<count;i++)keys[i].partition=enrichment_partition(target,key,partitions,PointerGetDatum(&keys[i].id));
    lp_sort(keys,(size_t)count,sizeof(*keys),presence_order);
    Oid native_id=get_atttype(target,attribute);
    for(int begin=0;begin<count;){
        CHECK_FOR_INTERRUPTS();int end=begin+1;
        while(end<count && keys[end].partition==keys[begin].partition)end++;
        Relation table=table_open(keys[begin].partition,AccessShareLock);
        Oid primary=RelationGetPrimaryKeyIndex(table,false);
        if(!OidIsValid(primary))ereport(ERROR,(errmsg("laplace_present_ids: Native partition has no primary index")));
        Relation index=index_open(primary,AccessShareLock);
        Datum *values=palloc((Size)(end-begin)*sizeof(Datum));
        for(int i=begin;i<end;i++)values[i-begin]=PointerGetDatum(&keys[i].id);
        ArrayType *wanted=construct_array(values,end-begin,native_id,sizeof(lp_id),false,TYPALIGN_CHAR);
        pfree(values);
        Oid equality=get_opfamily_member(index->rd_opfamily[0],native_id,native_id,BTEqualStrategyNumber);
        ScanKeyData scan_key;
        ScanKeyEntryInitialize(&scan_key,SK_SEARCHARRAY,1,BTEqualStrategyNumber,native_id,
            index->rd_indcollation[0],get_opcode(equality),PointerGetDatum(wanted));
        IndexScanDesc scan=index_beginscan(table,index,GetActiveSnapshot(),NULL,1,0);
        index_rescan(scan,&scan_key,1,NULL,0);
        TupleTableSlot *slot=table_slot_create(table,NULL);int at=begin;
        while(index_getnext_slot(scan,ForwardScanDirection,slot)){
            CHECK_FOR_INTERRUPTS();bool missing;
            Datum found=slot_getattr(slot,attribute,&missing);
            if(missing)ereport(ERROR,(errmsg("laplace_present_ids: null primary ID")));
            const lp_id *id=(const lp_id *)DatumGetPointer(found);
            while(at<end && memcmp(keys[at].id.b,id->b,sizeof(lp_id))<0)at++;
            while(at<end && lp_id_eq(&keys[at].id,id)){
                Datum row[2]={id_datum(id),Int64GetDatum(keys[at++].ordinal)};bool nulls[2]={false,false};
                set_put(fcinfo,output,descriptor,row,nulls);pfree(DatumGetPointer(row[0]));
            }
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);index_endscan(scan);pfree(wanted);
        index_close(index,NoLock);table_close(table,NoLock);begin=end;
    }
}

static Relation presence_parent(Oid target,const char **column)
{
    Oid schema=get_namespace_oid("public",false);
    bool paths=target==get_relname_relid("physicality",schema);
    if(!paths && target!=get_relname_relid("entity",schema))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native presence target must be public.entity or public.physicality")));
    *column=paths?"entity":"id";
    Relation parent=table_open(target,AccessShareLock);
    if(check_enable_rls(target,InvalidOid,false)==RLS_ENABLED)
        ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("Native presence requires a canonical shared relation without active RLS")));
    char *permission=psprintf("SELECT %s FROM public.%s WHERE false",*column,paths?"physicality":"entity");
    if(SPI_execute(permission,true,0)!=SPI_OK_SELECT)
        ereport(ERROR,(errmsg("Native presence: permission proof failed")));
    SPI_freetuptable(SPI_tuptable);pfree(permission);
    List *children=find_all_inheritors(target,AccessShareLock,NULL);list_free(children);
    return parent;
}

PG_FUNCTION_INFO_V1(laplace_present_ids);
Datum laplace_present_ids(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;Tuplestorestate *output=set_begin(fcinfo,"laplace_present_ids",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(!count){pfree(ids);PG_RETURN_NULL();}
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("laplace_present_ids: SPI_connect")));
    const char *column;
    Relation parent=presence_parent(get_relname_relid("entity",get_namespace_oid("public",false)),&column);
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=i+1};
    presence_keys(fcinfo,output,descriptor,parent,column,keys,count);
    pfree(keys);pfree(ids);table_close(parent,NoLock);SPI_finish();PG_RETURN_NULL();
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
    const char *column;Relation parent=presence_parent(target,&column);
    if(get_atttype(input,get_attnum(input,id_column))!=get_atttype(target,get_attnum(target,column)))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_present_stage requires Native content IDs")));
    char *source=quote_qualified_identifier(get_namespace_name(RelationGetNamespace(stage)),RelationGetRelationName(stage));
    char *sql=psprintf("SELECT %s FROM %s WHERE %s IS NOT TRUE",quote_identifier(id_column),source,quote_identifier(present_column));
    SPIPlanPtr plan=SPI_prepare_cursor(sql,0,NULL,CURSOR_OPT_NO_SCROLL);
    Portal scan=plan?SPI_cursor_open(NULL,plan,NULL,NULL,true):NULL;
    if(!scan)ereport(ERROR,(errmsg("laplace_present_stage: stage selection failed")));
    long fetch=enrichment_fetch_rows();int64 ordinal=0;
    for(;;){
        SPI_cursor_fetch(scan,true,fetch);
        if(!SPI_processed){SPI_freetuptable(SPI_tuptable);break;}
        int count=(int)SPI_processed;
        PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(int i=0;i<count;i++){
            bool missing;Datum id=SPI_getbinval(SPI_tuptable->vals[i],SPI_tuptable->tupdesc,1,&missing);
            if(missing)ereport(ERROR,(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),errmsg("laplace_present_stage: null Native ID")));
            keys[i]=(PresenceKey){.id=*(const lp_id *)DatumGetPointer(id),.ordinal=++ordinal};
        }
        SPI_freetuptable(SPI_tuptable);
        presence_keys(fcinfo,output,descriptor,parent,column,keys,count);pfree(keys);
    }
    SPI_cursor_close(scan);SPI_freeplan(plan);pfree(sql);pfree(source);
    table_close(parent,NoLock);table_close(stage,NoLock);SPI_finish();PG_RETURN_NULL();
}

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
            psprintf(" AND EXISTS (SELECT FROM %s classified WHERE classified.id=s.%s AND classified.%s)",proof_name,key,flag);
        table_close(proof,NoLock);
    }
    const char *changed = paths
        ? "(d.tier>s.tier OR (d.mask|s.mask)<>d.mask OR CASE WHEN s.path IS NULL THEN false ELSE (ST_AsEWKB(d.path)<>ST_AsEWKB(s.path)"
          " AND ST_AsEWKB(laplace_path_enrich(d.path,s.path))<>ST_AsEWKB(d.path)) END)"
        : "(d.tier>s.tier OR (d.mask|s.mask)<>d.mask)";
    /* Do not join the entire destination merely to discover target leaves.
     * PostgreSQL's own range-bound search routes the copied keys in C. */
    char *compatible=proof_name?psprintf("public.laplace_stage_compatible(d.%s,s.%s,d.%s,s.%s,d.hilbert,s.hilbert,%s)",
        key,key,paths?"path":"coord",paths?"path":"coord",paths?"true":"false"):pstrdup("true");
    char *needed=proof_name?psprintf("(%s OR NOT %s)",changed,compatible):pstrdup(changed);
    char *select = psprintf("SELECT s.%s,s.ctid FROM %s s WHERE true%s ORDER BY s.%s", key, source, restriction, key);
    char *update = psprintf("UPDATE %s d SET tier=CASE WHEN %s THEN least(d.tier,s.tier) ELSE d.tier END,mask=laplace_mask_merge(d.mask,s.mask)%s"
                           " FROM %s s WHERE d.%s=s.%s AND d.%s=ANY($1) AND s.ctid=ANY($2) AND %s%s",
                           destination,compatible, paths ? ",path=CASE WHEN s.path IS NULL THEN d.path WHEN ST_AsEWKB(d.path)<>ST_AsEWKB(s.path)"
                           " THEN laplace_path_enrich(d.path,s.path) ELSE d.path END" : "",
                           source, key, key, key, needed,restriction);
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR, (errmsg("laplace_enrich_stage: SPI_connect")));
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
            keys[i].partition = enrichment_partition(target, partition_key, partitions,
                PointerGetDatum(&keys[i].id));
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

/* Complete bulk index maintenance before repeated containment reads.
 * Route copied IDs through the same Native partition bounds as enrichment;
 * neither scan public paths again nor sweep unrelated destination indexes. */
PG_FUNCTION_INFO_V1(laplace_clean_stage);
Datum laplace_clean_stage(PG_FUNCTION_ARGS)
{
    Oid input = PG_GETARG_OID(0);
    Relation stage = table_open(input, AccessShareLock);
    Oid target = get_relname_relid("physicality", get_namespace_oid("public", false));
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
    SPIPlanPtr plan = SPI_prepare_cursor(selection, 0, NULL, CURSOR_OPT_NO_SCROLL);
    if (!plan) ereport(ERROR, (errmsg("laplace_clean_stage: selection plan failed")));
    Portal scan = SPI_cursor_open(NULL, plan, NULL, NULL, true);
    if (!scan) ereport(ERROR, (errmsg("laplace_clean_stage: selection failed")));
    for (;;) {
        SPI_cursor_fetch(scan, true, enrichment_fetch_rows());
        if (!SPI_processed) { SPI_freetuptable(SPI_tuptable); break; }
        key = RelationGetPartitionKey(parent);
        PartitionDesc partitions = RelationGetPartitionDesc(parent, true);
        for (uint64 row = 0; row < SPI_processed; row++) {
            CHECK_FOR_INTERRUPTS();
            bool missing;
            Datum id = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &missing);
            if (missing)
                ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace_clean_stage: null Native ID")));
            Oid partition = enrichment_partition(target, key, partitions, id);
            hash_search(touched, &partition, HASH_ENTER, NULL);
        }
        SPI_freetuptable(SPI_tuptable);
    }
    SPI_cursor_close(scan); SPI_freeplan(plan);
    int count = hash_get_num_entries(touched), at = 0;
    Datum *values = palloc(sizeof(Datum) * Max(count, 1));
    HASH_SEQ_STATUS entries; hash_seq_init(&entries, touched); Oid *partition;
    while ((partition = hash_seq_search(&entries))) values[at++] = ObjectIdGetDatum(*partition);
    ArrayType *ids = construct_array(values, count, OIDOID, sizeof(Oid), true, TYPALIGN_INT);
    Oid type = OIDARRAYOID; Datum parameter = PointerGetDatum(ids);
    if (SPI_execute_with_args("SELECT public.laplace_clean_pending_list(i.indexrelid)"
        " FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class c ON c.oid=i.indexrelid"
        " WHERE i.indrelid=ANY($1) AND c.relam=" CppAsString2(GIN_AM_OID)
        " ORDER BY i.indexrelid", 1, &type, &parameter, NULL, false, 0) != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("laplace_clean_stage: index maintenance failed")));
    int64 cleaned = 0;
    for (uint64 row = 0; row < SPI_processed; row++) {
        bool missing;
        Datum pages = SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &missing);
        if (!missing) cleaned += DatumGetInt64(pages);
    }
    SPI_freetuptable(SPI_tuptable); pfree(ids); pfree(values); hash_destroy(touched);
    SPI_finish(); table_close(stage, NoLock); table_close(parent, NoLock);
    pfree(source); pfree(selection);
    PG_RETURN_INT64(cleaned);
}
