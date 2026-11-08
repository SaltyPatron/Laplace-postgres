/* Partition-routed B-tree primary index kernel and ID-addressed frontier reads
 * (entity, physicality, consensus, witness, and presence membership). */
#include "pg_core.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/tupconvert.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "utils/acl.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "partitioning/partdesc.h"
#include "partitioning/partbounds.h"

static int presence_order(const void *a,const void *b)
{
    const PresenceKey *x=a,*y=b;
    /* ID order groups nonoverlapping ranges independently of relation OIDs. */
    int compare=memcmp(x->id.b,y->id.b,sizeof(lp_id));
    return compare?compare:(x->ordinal>y->ordinal)-(x->ordinal<y->ordinal);
}

Oid lp_pg_enrichment_partition(Oid target, PartitionKey key, PartitionDesc partitions, Datum value)
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
static void indexed_keys_until(Relation parent,const char *column,PresenceKey *keys,size_t count,
                               NativeKeyVisit visit,void *context,const bool *done)
{
    Oid target=RelationGetRelid(parent);
    AttrNumber attribute=get_attnum(target,column);
    PartitionKey key=RelationGetPartitionKey(parent);
    if((key && (key->strategy!=PARTITION_STRATEGY_RANGE || key->partnatts!=1
        || key->partattrs[0]!=attribute)) || (!key && parent->rd_rel->relkind!=RELKIND_RELATION))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native presence requires canonical ID range partitions")));
    PartitionDesc partitions=key?RelationGetPartitionDesc(parent,true):NULL;
    for(size_t i=0;i<count;i++)keys[i].partition=lp_pg_enrichment_partition(target,key,partitions,PointerGetDatum(&keys[i].id));
    lp_sort(keys,(size_t)count,sizeof(*keys),presence_order);
    Oid native_id=get_atttype(target,attribute);
    for(size_t begin=0;begin<count && (!done || !*done);){
        CHECK_FOR_INTERRUPTS();size_t end=begin+1;
        while(end<count && keys[end].partition==keys[begin].partition)end++;
        /* The PostgreSQL array representation bounds one index search, not
         * the admitted frontier. Continue with the remaining IDs if needed. */
        size_t array_ids=(MaxAllocSize-ARR_OVERHEAD_NONULLS(1))/sizeof(lp_id);
        if(end-begin>array_ids)end=begin+array_ids;
        Relation table=table_open(keys[begin].partition,AccessShareLock);
        AttrNumber stored_attribute=get_attnum(RelationGetRelid(table),column);
        TupleConversionMap *conversion=convert_tuples_by_name(RelationGetDescr(table),RelationGetDescr(parent));
        TupleTableSlot *projected=conversion?MakeSingleTupleTableSlot(RelationGetDescr(parent),&TTSOpsVirtual):NULL;
        Oid primary=RelationGetPrimaryKeyIndex(table,false);
        if(!OidIsValid(primary))ereport(ERROR,(errmsg("Native ID reader: relation has no primary index")));
        Relation index=index_open(primary,AccessShareLock);
        Datum *values=palloc((Size)(end-begin)*sizeof(Datum));
        for(size_t i=begin;i<end;i++)values[i-begin]=PointerGetDatum(&keys[i].id);
        ArrayType *wanted=construct_array(values,(int)(end-begin),native_id,sizeof(lp_id),false,TYPALIGN_CHAR);
        pfree(values);
        Oid equality=get_opfamily_member(index->rd_opfamily[0],native_id,native_id,BTEqualStrategyNumber);
        ScanKeyData scan_key;
        ScanKeyEntryInitialize(&scan_key,SK_SEARCHARRAY,1,BTEqualStrategyNumber,native_id,
            index->rd_indcollation[0],get_opcode(equality),PointerGetDatum(wanted));
        IndexScanDesc scan=index_beginscan(table,index,GetActiveSnapshot(),NULL,1,0);
        index_rescan(scan,&scan_key,1,NULL,0);
        TupleTableSlot *slot=table_slot_create(table,NULL);size_t at=begin;
        while((!done || !*done) && index_getnext_slot(scan,ForwardScanDirection,slot)){
            CHECK_FOR_INTERRUPTS();bool missing;
            Datum found=slot_getattr(slot,stored_attribute,&missing);
            if(missing)ereport(ERROR,(errmsg("Native ID reader: null primary ID")));
            const lp_id *id=(const lp_id *)DatumGetPointer(found);
            TupleTableSlot *row=conversion?execute_attr_map_slot(conversion->attrMap,slot,projected):slot;
            while(at<end && memcmp(keys[at].id.b,id->b,sizeof(lp_id))<0)at++;
            while(at<end && lp_id_eq(&keys[at].id,id)){
                visit(row,&keys[at++],context);
            }
            if(projected)ExecClearTuple(projected);
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);index_endscan(scan);pfree(wanted);
        if(projected){ExecDropSingleTupleTableSlot(projected);free_conversion_map(conversion);}
        index_close(index,NoLock);table_close(table,NoLock);begin=end;
    }
}

void lp_pg_indexed_keys(Relation parent,const char *column,PresenceKey *keys,size_t count,
                        NativeKeyVisit visit,void *context)
{
    indexed_keys_until(parent,column,keys,count,visit,context,NULL);
}

/* Reuse GIN's acquired row locations after standing qualification. Sorting by
 * the same partition/ID key as indexed_keys preserves its visit/fan order.
 * Parent ACL/RLS checks and child locks were established before nomination;
 * the original active snapshot remains authoritative for heap visibility. */
void lp_pg_acquired_rows(Relation parent,PresenceKey *keys,size_t count,lp_idmap *carriers,
                         NativeKeyVisit visit,void *context,const bool *done)
{
    for(size_t i=0;i<count;i++){
        const CarrierLocation *location=lp_idmap_at(carriers,(size_t)keys[i].ordinal-1);
        if(!OidIsValid(location->partition) || !ItemPointerIsValid(&location->tuple))
            ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native acquired carrier has no row location")));
        keys[i].partition=location->partition;
    }
    lp_sort(keys,count,sizeof(*keys),presence_order);
    Snapshot snapshot=GetActiveSnapshot();
    for(size_t begin=0;begin<count && (!done || !*done);){
        size_t end=begin+1;while(end<count && keys[end].partition==keys[begin].partition)end++;
        Relation table=table_open(keys[begin].partition,AccessShareLock);
        TupleConversionMap *conversion=convert_tuples_by_name(RelationGetDescr(table),RelationGetDescr(parent));
        TupleTableSlot *projected=conversion?MakeSingleTupleTableSlot(RelationGetDescr(parent),&TTSOpsVirtual):NULL;
        TupleTableSlot *slot=table_slot_create(table,NULL);
        AttrNumber identity=get_attnum(RelationGetRelid(table),"entity");
        for(size_t i=begin;i<end && (!done || !*done);i++){
            CHECK_FOR_INTERRUPTS();
            const CarrierLocation *location=lp_idmap_at(carriers,(size_t)keys[i].ordinal-1);
            ItemPointerData tuple=location->tuple;
            if(!table_tuple_fetch_row_version(table,&tuple,snapshot,slot))
                ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native acquired carrier lost its snapshot-visible row")));
            bool missing;Datum id=slot_getattr(slot,identity,&missing);
            if(missing || !lp_id_eq((const lp_id *)DatumGetPointer(id),&keys[i].id))
                ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native acquired carrier identity changed")));
            TupleTableSlot *row=conversion?execute_attr_map_slot(conversion->attrMap,slot,projected):slot;
            visit(row,keys+i,context);
            if(projected)ExecClearTuple(projected);
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);
        if(projected){ExecDropSingleTupleTableSlot(projected);free_conversion_map(conversion);}
        table_close(table,NoLock);begin=end;
    }
}

static void presence_visit(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    PresenceOutput *out=context;
    Datum row[2]={id_datum(&key->id),Int64GetDatum(key->ordinal)};bool nulls[2]={false,false};
    set_put(out->fcinfo,out->output,out->descriptor,row,nulls);pfree(DatumGetPointer(row[0]));
}
void lp_pg_presence_keys(FunctionCallInfo fcinfo,Tuplestorestate *output,TupleDesc descriptor,
                         Relation parent,const char *column,PresenceKey *keys,int count)
{
    PresenceOutput out={fcinfo,output,descriptor};
    lp_pg_indexed_keys(parent,column,keys,(size_t)count,presence_visit,&out);
}

Relation lp_pg_read_parent(Oid target,const char **column,const char *const *selected,int columns)
{
    Oid schema=lp_pg_schema();
    bool paths=target==get_relname_relid("physicality",schema);
    bool standings=target==get_relname_relid("consensus",schema);
    bool witness=target==get_relname_relid("witness",schema);
    if(!paths && !standings && !witness && target!=get_relname_relid("entity",schema))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native ID target must be an installed canonical relation")));
    *column=paths?"entity":standings?"claim":"id";
    Relation parent=table_open(target,AccessShareLock);
    if(check_enable_rls(target,InvalidOid,false)==RLS_ENABLED)
        ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("Native presence requires a canonical shared relation without active RLS")));
    /* Apply PostgreSQL's parent/column privileges directly. Planning a dummy
     * SELECT for every frontier added an avoidable SQL executor boundary. */
    if(pg_class_aclcheck(target,GetUserId(),ACL_SELECT)!=ACLCHECK_OK){
        int count=columns;
        for(int i=0;i<count;i++){
            AclResult allowed=pg_attribute_aclcheck(target,get_attnum(target,selected[i]),GetUserId(),ACL_SELECT);
            if(allowed!=ACLCHECK_OK)
                aclcheck_error_col(allowed,OBJECT_TABLE,RelationGetRelationName(parent),selected[i]);
        }
    }
    return parent;
}

Relation lp_pg_presence_parent(Oid target,const char **column,int columns)
{
    const char *selected[]={target==lp_pg_relation("physicality")?"entity":target==lp_pg_relation("consensus")?"claim":"id","path","mask"};
    return lp_pg_read_parent(target,column,selected,columns);
}

/* Exact stored rows, addressed by the same ID kernel as presence and DAG
 * frontiers. Ordinals belong to the request, including duplicate IDs; absent
 * IDs have no row. Tier-0 cache entries remain virtual until admitted. */
typedef struct {
    PresenceOutput output;AttrNumber attributes[6];int columns;
} StoredRows;
static void stored_row(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    StoredRows *out=context;Datum values[7];bool nulls[7]={false};
    values[0]=Int64GetDatum(key->ordinal);
    for(int i=0;i<out->columns;i++)values[i+1]=slot_getattr(slot,out->attributes[i],&nulls[i+1]);
    set_put(out->output.fcinfo,out->output.output,out->output.descriptor,values,nulls);
}
static Datum stored_rows(FunctionCallInfo fcinfo,int kind)
{
    const char *entities[]={"id","tier","coord","hilbert","mask"};
    const char *physicalities[]={"entity","tier","hilbert","path","mask"};
    const char *standings[]={"claim","rating","deviation","volatility","matches"};
    const char *const *selected=kind==2?standings:kind==1?physicalities:entities;
    const char *name=kind==2?"laplace_standings":kind==1?"laplace_physicalities":"laplace_entities";
    StoredRows out={.columns=5};out.output.fcinfo=fcinfo;
    out.output.output=set_begin(fcinfo,name,&out.output.descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(!count){pfree(ids);return (Datum)0;}
    Oid target=lp_pg_relation(kind==2?"consensus":kind==1?"physicality":"entity");const char *column;
    Relation parent=lp_pg_read_parent(target,&column,selected,out.columns);
    for(int i=0;i<out.columns;i++)out.attributes[i]=get_attnum(target,selected[i]);
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    lp_pg_indexed_keys(parent,column,keys,(size_t)count,stored_row,&out);
    pfree(keys);pfree(ids);table_close(parent,NoLock);return (Datum)0;
}
PG_FUNCTION_INFO_V1(laplace_entities);
Datum laplace_entities(PG_FUNCTION_ARGS){return stored_rows(fcinfo,false);}
PG_FUNCTION_INFO_V1(laplace_physicalities);
Datum laplace_physicalities(PG_FUNCTION_ARGS){return stored_rows(fcinfo,true);}
PG_FUNCTION_INFO_V1(laplace_standings);
Datum laplace_standings(PG_FUNCTION_ARGS){return stored_rows(fcinfo,2);}

typedef struct {
    lp_pg_path_visit visit;void *context;AttrNumber path,mask;MemoryContext tuple_context;
} PathFrontier;
static void path_frontier_visit(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    PathFrontier *front=context;bool path_null,mask_null;
    Datum path=slot_getattr(slot,front->path,&path_null),mask=(Datum)0;
    mask_null=false;if(front->mask>0)mask=slot_getattr(slot,front->mask,&mask_null);
    if(path_null||mask_null)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native path frontier: null carrier")));
    MemoryContext previous=MemoryContextSwitchTo(front->tuple_context);
    front->visit(&key->id,path,mask,front->context);
    MemoryContextSwitchTo(previous);MemoryContextReset(front->tuple_context);
}
void lp_pg_lookup_paths(const lp_id *ids,size_t count,bool masks,lp_pg_path_visit visit,void *context)
{
    if(!count)return;
    const char *column;Oid target=lp_pg_relation("physicality");
    Relation parent=lp_pg_presence_parent(target,&column,masks?3:2);
    if(count>MaxAllocHugeSize/sizeof(PresenceKey))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("Native path frontier exceeds addressable memory")));
    PresenceKey *keys=palloc_extended(count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(size_t i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    PathFrontier front={visit,context,get_attnum(target,"path"),masks?get_attnum(target,"mask"):InvalidAttrNumber,
        AllocSetContextCreate(CurrentMemoryContext,"Native path frontier tuple",ALLOCSET_DEFAULT_SIZES)};
    lp_pg_indexed_keys(parent,column,keys,count,path_frontier_visit,&front);
    MemoryContextDelete(front.tuple_context);pfree(keys);table_close(parent,NoLock);
}

typedef struct {lp_pg_witness_visit visit;void *context;AttrNumber lineage,trust;} WitnessFrontier;
static void witness_frontier_visit(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    WitnessFrontier *front=context;bool lineage_null=true,trust_null;
    Datum lineage=front->lineage>0?slot_getattr(slot,front->lineage,&lineage_null):(Datum)0;
    Datum trust=slot_getattr(slot,front->trust,&trust_null);
    if(trust_null)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native witness frontier: null trust")));
    front->visit(&key->id,lineage_null?NULL:(const lp_id *)DatumGetPointer(lineage),DatumGetFloat8(trust),front->context);
}
void lp_pg_lookup_witnesses(const lp_id *ids,size_t count,bool lineage,lp_pg_witness_visit visit,void *context)
{
    if(!count)return;
    Oid target=lp_pg_relation("witness");const char *column;const char *selected[]={"id","trust","lineage"};
    Relation parent=lp_pg_read_parent(target,&column,selected,lineage?3:2);
    WitnessFrontier front={visit,context,lineage?get_attnum(target,"lineage"):InvalidAttrNumber,get_attnum(target,"trust")};
    size_t window=frontier_window_ids();
    for(size_t at=0;at<count;){
        size_t amount=Min(window,count-at);PresenceKey *keys=palloc(amount*sizeof *keys);
        for(size_t i=0;i<amount;i++)keys[i]=(PresenceKey){.id=ids[at+i],.ordinal=(int64)(at+i)+1};
        lp_pg_indexed_keys(parent,column,keys,amount,witness_frontier_visit,&front);pfree(keys);at+=amount;
    }
    table_close(parent,NoLock);
}

static void present_bitmap_row(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ((bool *)context)[key->ordinal-1]=true;
}
void lp_pg_present(const lp_id *ids,size_t count,bool *present)
{
    if(!count)return;
    size_t wanted=0;
    for(size_t i=0;i<count;i++)wanted+=!present[i];
    if(!wanted)return;
    if(wanted>MaxAllocHugeSize/sizeof(PresenceKey))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("Native presence frontier exceeds addressable memory")));
    PresenceKey *keys=palloc_extended(sizeof(*keys)*wanted,MCXT_ALLOC_HUGE);
    size_t at=0;
    for(size_t i=0;i<count;i++)if(!present[i])keys[at++]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    const char *column;Relation parent=lp_pg_presence_parent(lp_pg_relation("entity"),&column,1);
    lp_pg_indexed_keys(parent,column,keys,wanted,present_bitmap_row,present);
    pfree(keys);table_close(parent,NoLock);
}
PG_FUNCTION_INFO_V1(laplace_present_ids);
Datum laplace_present_ids(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;Tuplestorestate *output=set_begin(fcinfo,"laplace_present_ids",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(!count){pfree(ids);PG_RETURN_NULL();}
    const char *column;
    Relation parent=lp_pg_presence_parent(lp_pg_relation("entity"),&column,1);
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=i+1};
    lp_pg_presence_keys(fcinfo,output,descriptor,parent,column,keys,count);
    pfree(keys);pfree(ids);table_close(parent,NoLock);PG_RETURN_NULL();
}
