/* Shared Native primary-key set reads and partition-directed metadata updates.
 * Exact IDs route to their canonical ranges; parent-table privileges remain
 * authoritative for both membership and path retrieval. */
#include "pg_core.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/tupconvert.h"
#include "access/gin.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/hsearch.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/partcache.h"
#include "utils/acl.h"
#include "laplace/geometry.h"
#include "geo.h"
#include "nodes/tidbitmap.h"
#include "partitioning/partdesc.h"
#include "partitioning/partbounds.h"
#include "storage/bufmgr.h"
#include <limits.h>

typedef struct { Oid partition; lp_id id; int64 ordinal; } PresenceKey;
/* A carrier already read under this statement's snapshot. Keep only its
 * location, not another copy of its potentially large geometry. This prefix
 * is shared by plain carrier maps and scored-claim map entries. */
typedef struct { Oid partition; ItemPointerData tuple; } CarrierLocation;
static long enrichment_fetch_rows(void);
static int presence_order(const void *a,const void *b)
{
    const PresenceKey *x=a,*y=b;
    /* ID order groups nonoverlapping ranges independently of relation OIDs. */
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
typedef void (*NativeKeyVisit)(TupleTableSlot *slot,const PresenceKey *key,void *context);
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
    for(size_t i=0;i<count;i++)keys[i].partition=enrichment_partition(target,key,partitions,PointerGetDatum(&keys[i].id));
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
static void indexed_keys(Relation parent,const char *column,PresenceKey *keys,size_t count,
                         NativeKeyVisit visit,void *context)
{
    indexed_keys_until(parent,column,keys,count,visit,context,NULL);
}

/* Reuse GIN's acquired row locations after standing qualification. Sorting by
 * the same partition/ID key as indexed_keys preserves its visit/fan order.
 * Parent ACL/RLS checks and child locks were established before nomination;
 * the original active snapshot remains authoritative for heap visibility. */
static void acquired_rows(Relation parent,PresenceKey *keys,size_t count,lp_idmap *carriers,
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

typedef struct { FunctionCallInfo fcinfo; Tuplestorestate *output; TupleDesc descriptor; } PresenceOutput;
static void presence_visit(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    PresenceOutput *out=context;
    Datum row[2]={id_datum(&key->id),Int64GetDatum(key->ordinal)};bool nulls[2]={false,false};
    set_put(out->fcinfo,out->output,out->descriptor,row,nulls);pfree(DatumGetPointer(row[0]));
}
static void presence_keys(FunctionCallInfo fcinfo,Tuplestorestate *output,TupleDesc descriptor,
                          Relation parent,const char *column,PresenceKey *keys,int count)
{
    PresenceOutput out={fcinfo,output,descriptor};
    indexed_keys(parent,column,keys,(size_t)count,presence_visit,&out);
}

static Relation read_parent(Oid target,const char **column,const char *const *selected,int columns)
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
    List *children=find_all_inheritors(target,AccessShareLock,NULL);list_free(children);
    return parent;
}
static Relation presence_parent(Oid target,const char **column,int columns)
{
    const char *selected[]={target==lp_pg_relation("physicality")?"entity":target==lp_pg_relation("consensus")?"claim":"id","path","mask"};
    return read_parent(target,column,selected,columns);
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
    Relation parent=read_parent(target,&column,selected,out.columns);
    for(int i=0;i<out.columns;i++)out.attributes[i]=get_attnum(target,selected[i]);
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    indexed_keys(parent,column,keys,(size_t)count,stored_row,&out);
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
    Relation parent=presence_parent(target,&column,masks?3:2);
    if(count>MaxAllocHugeSize/sizeof(PresenceKey))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("Native path frontier exceeds addressable memory")));
    PresenceKey *keys=palloc_extended(count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(size_t i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    PathFrontier front={visit,context,get_attnum(target,"path"),masks?get_attnum(target,"mask"):InvalidAttrNumber,
        AllocSetContextCreate(CurrentMemoryContext,"Native path frontier tuple",ALLOCSET_DEFAULT_SIZES)};
    indexed_keys(parent,column,keys,count,path_frontier_visit,&front);
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
    Relation parent=read_parent(target,&column,selected,lineage?3:2);
    WitnessFrontier front={visit,context,lineage?get_attnum(target,"lineage"):InvalidAttrNumber,get_attnum(target,"trust")};
    size_t window=frontier_window_ids();
    for(size_t at=0;at<count;){
        size_t amount=Min(window,count-at);PresenceKey *keys=palloc(amount*sizeof *keys);
        for(size_t i=0;i<amount;i++)keys[i]=(PresenceKey){.id=ids[at+i],.ordinal=(int64)(at+i)+1};
        indexed_keys(parent,column,keys,amount,witness_frontier_visit,&front);pfree(keys);at+=amount;
    }
    table_close(parent,NoLock);
}

/* GIN is the retained containment access contract, just as the primary index
 * is the retained identity access contract above. Unknown SPI array sizes can
 * otherwise make the planner scan every physicality at each sparse DAG hop. */
static Relation container_index(Relation table,AttrNumber path,Oid array_type,StrategyNumber strategy,List *predicate,int *column,Oid *operation)
{
    List *indexes=RelationGetIndexList(table);Relation found=NULL;
    foreach_oid(index_id,indexes){
        Relation index=index_open(index_id,AccessShareLock);
        if(index->rd_rel->relam==GIN_AM_OID && index->rd_index->indisvalid &&
           index->rd_index->indisready && index->rd_index->indislive &&
           (!index->rd_index->indcheckxmin || TransactionIdPrecedes(HeapTupleHeaderGetXmin(index->rd_indextuple->t_data),TransactionXmin)) &&
           equal(RelationGetIndexPredicate(index),predicate)){
            for(int i=0;i<index->rd_index->indnkeyatts;i++)if(index->rd_index->indkey.values[i]==path){
                Oid op=get_opfamily_member(index->rd_opfamily[i],get_atttype(RelationGetRelid(table),path),array_type,strategy);
                if(OidIsValid(op)){*column=i+1;*operation=get_opcode(op);found=index;break;}
            }
        }
        if(found)break;
        index_close(index,NoLock);
    }
    list_free(indexes);
    if(!found)ereport(ERROR,(errcode(ERRCODE_UNDEFINED_OBJECT),
        errmsg("Native containment requires a complete %sGIN path index on %s",predicate?"assertion ":"",RelationGetRelationName(table))));
    return found;
}

static bool container_matches(TupleTableSlot *slot,AttrNumber path,lp_idmap *wanted,bool all,uint64 *epoch)
{
    bool missing;Datum value=slot_getattr(slot,path,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native containment path is null")));
    Geo geometry=geo_of(value);size_t required=lp_idmap_count(wanted),found=0;
    if(all && geometry.p.n<required)return false;
    if(all && ++*epoch==0){
        for(size_t i=0;i<required;i++)*(uint64 *)lp_idmap_at(wanted,i)=0;
        *epoch=1;
    }
    for(size_t i=0;i<geometry.p.n;i++){
        lp_id child=lp_path_id(geometry.p,i);int64 at=lp_idmap_find(wanted,&child);
        if(at<0)continue;if(!all)return true;
        uint64 *seen=lp_idmap_at(wanted,(size_t)at);
        if(*seen!=*epoch){*seen=*epoch;if(++found==required)return true;}
    }
    return false;
}

static void container_remember(TupleTableSlot *slot,AttrNumber id,lp_idmap *parents)
{
    bool missing;Datum value=slot_getattr(slot,id,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native containment parent identity is null")));
    CarrierLocation *location=lp_idmap_get(parents,(const lp_id *)DatumGetPointer(value),NULL);
    *location=(CarrierLocation){slot->tts_tableOid,slot->tts_tid};
}
static bool container_endpoint_matches(TupleTableSlot *slot,AttrNumber path,lp_idmap *wanted,int endpoint)
{
    bool missing;Datum value=slot_getattr(slot,path,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native containment path is null")));
    Geo geometry=geo_of(value);if(!geometry.p.n)return false;
    lp_id first=lp_path_id(geometry.p,0),last=lp_path_id(geometry.p,geometry.p.n-1);
    return ((endpoint&1) && lp_idmap_find(wanted,&first)>=0)
        || ((endpoint&2) && lp_idmap_find(wanted,&last)>=0);
}
/* Shared GIN nomination for overlap, all-key containment and endpoint sets.
 * Endpoint seeks union existing first/last postings into one bounded bitmap;
 * they never acquire all the text parents of a common predicate or word.
 * Array windows are transport bounds; lossy rechecks use the complete set. */
typedef bool (*ContainerVisit)(TupleTableSlot *slot,void *context);
static void scan_container_rows(Relation parent,const char *column,lp_idmap *wanted,StrategyNumber strategy,
                                 lp_idmap *required,bool required_any,int endpoint,lp_idmap *opposite,lp_mask bits,bool assertions,ContainerVisit visit,void *context)
{
    if(GinFuzzySearchLimit)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native containment requires exact GIN search (gin_fuzzy_search_limit=0)")));
    Oid element=get_atttype(RelationGetRelid(parent),get_attnum(RelationGetRelid(parent),column));
    Oid array_type=get_array_type(element);
    Oid assertion_function=InvalidOid;
    if(assertions){
        Oid arguments[]={get_atttype(RelationGetRelid(parent),get_attnum(RelationGetRelid(parent),"path")),INT2OID};
        assertion_function=LookupFuncName(list_make2(makeString(get_namespace_name(lp_pg_schema())),makeString("laplace_path_has_roles")),2,arguments,false);
    }
    List *relations=find_all_inheritors(RelationGetRelid(parent),NoLock,NULL);
    Snapshot snapshot=GetActiveSnapshot();
    MemoryContext scratch=AllocSetContextCreate(CurrentMemoryContext,"Native GIN tuple",ALLOCSET_SMALL_SIZES);
    size_t budget=frontier_window_ids(),unique=lp_idmap_count(wanted);uint64 epoch=0;bool proceed=true;
    /* Every extra key needs an entry-tree probe in each partition. A broader
     * opposite set is cheaper to test against the acquired primary candidates;
     * claim_nominate still qualifies its complete set before standing and fan. */
    bool indexed_opposite=opposite && lp_idmap_count(opposite)<=unique;
    bool first_leaf=true;
    elog(DEBUG1,"Native containment frontier: %zu IDs; strategy %u; endpoint %d",unique,strategy,endpoint);
    size_t nrequired=required?lp_idmap_count(required):0;
    ArrayType *required_keys=nrequired?id_array_of(element,lp_idmap_keys(required),(int)(required_any?nrequired:Min(budget,nrequired))):NULL;
    Datum mask_values[64];int nbits=0;
    for(int bit=0;bit<64;bit++)if(bits&(UINT64_C(1)<<bit))mask_values[nbits++]=Int16GetDatum((int16)bit);
    ArrayType *mask_keys=nbits?construct_array(mask_values,nbits,INT2OID,2,true,TYPALIGN_SHORT):NULL;
    foreach_oid(relation_id,relations){
        CHECK_FOR_INTERRUPTS();Relation table=table_open(relation_id,NoLock);
        if(table->rd_rel->relkind==RELKIND_PARTITIONED_TABLE){table_close(table,NoLock);continue;}
        if(!table->rd_tableam || !table->rd_tableam->scan_bitmap_next_tuple)
            ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("Native containment requires bitmap table access")));
        AttrNumber path=get_attnum(relation_id,"path");
        TupleConversionMap *conversion=convert_tuples_by_name(RelationGetDescr(table),RelationGetDescr(parent));
        TupleTableSlot *projected=conversion?MakeSingleTupleTableSlot(RelationGetDescr(parent),&TTSOpsVirtual):NULL;
        int index_column;Oid operation;
        Oid operand=endpoint?element:array_type;
        StrategyNumber nomination=endpoint?((endpoint&1)?8:9):strategy;
        List *predicate=NIL;
        if(assertions){
            Expr *roles=(Expr *)makeConst(INT2OID,-1,InvalidOid,sizeof(int16),Int16GetDatum((1u<<LP_SAID_CLAIM)|(1u<<LP_SAID_RECORD)),false,true);
            predicate=list_make1(makeFuncExpr(assertion_function,BOOLOID,
                list_make2(makeVar(1,path,get_atttype(relation_id,path),-1,InvalidOid,0),roles),InvalidOid,InvalidOid,COERCE_EXPLICIT_CALL));
        }
        Relation index=container_index(table,path,operand,nomination,predicate,&index_column,&operation);
        size_t window=budget;
        if(!endpoint && strategy==3 && unique>=LP_PG_GIN_RANGE_IDS){
            GinStatsData stats;ginGetStats(index,&stats);
            /* A small dictionary is cheaper to stream than millions of absent
             * seeks. For a sparse set, ask the support routine for exact keys
             * in ONE memory-bounded scan. Artificial 511-ID windows otherwise
             * rescan the GIN pending list and repeat entry-tree descent. */
            if(stats.nEntries>0 && (uint64)stats.nEntries>unique)
                nomination=LP_PG_GIN_EXACT_OVERLAPS;
            if(first_leaf)elog(DEBUG1,"Native containment index: entries " UINT64_FORMAT "; strategy %u; window %zu",stats.nEntries,nomination,window);
        }
        first_leaf=false;
        ScanKeyData key[4];int nkeys=required_keys?2:1;
        ScanKeyEntryInitialize(key,0,index_column,nomination,operand,index->rd_indcollation[index_column-1],operation,(Datum)0);
        if(required_keys){
            StrategyNumber selection=required_any?3:7;
            Oid operation=get_opfamily_member(index->rd_opfamily[index_column-1],get_atttype(relation_id,path),array_type,selection);
            if(!OidIsValid(operation))ereport(ERROR,(errmsg("Native required membership needs its GIN operation")));
            ScanKeyEntryInitialize(key+1,0,index_column,selection,array_type,index->rd_indcollation[index_column-1],get_opcode(operation),PointerGetDatum(required_keys));
        }
        int opposite_key=indexed_opposite?nkeys++:-1;
        /* The canonical GIN index already stores path AND mask. Push
         * positive carrier bits into that same access method: filtering
         * after the heap fetched every text parent defeats the index. A
         * caller-owned path-only index remains a correct fallback. */
        if(mask_keys){
            AttrNumber mask=get_attnum(relation_id,"mask");
            for(int i=0;i<index->rd_index->indnkeyatts;i++)if(index->rd_index->indkey.values[i]==mask){
                Oid op=get_opfamily_member(index->rd_opfamily[i],get_atttype(relation_id,mask),INT2ARRAYOID,2);
                if(OidIsValid(op)){
                    ScanKeyEntryInitialize(key+nkeys++,0,i+1,2,INT2ARRAYOID,index->rd_indcollation[i],get_opcode(op),PointerGetDatum(mask_keys));
                    break;
                }
            }
        }
        IndexScanDesc scan=index_beginscan_bitmap(index,snapshot,NULL,nkeys);
        TIDBitmap *bitmap=tbm_create((Size)work_mem*1024,NULL);
        for(size_t begin=0;begin<unique;begin+=window){
            size_t amount=Min(window,unique-begin);
            ArrayType *keys=id_array_of(element,lp_idmap_keys(wanted)+begin,(int)amount);
            size_t opposite_count=indexed_opposite?lp_idmap_count(opposite):1;
            for(size_t other=0;other<opposite_count;other+=window){
                ArrayType *other_keys=NULL;
                if(indexed_opposite){
                    other_keys=id_array_of(element,lp_idmap_keys(opposite)+other,(int)Min(window,opposite_count-other));
                    ScanKeyEntryInitialize(key+opposite_key,0,index_column,
                        endpoint==1?LP_PG_GIN_LAST_ANY:LP_PG_GIN_FIRST_ANY,array_type,
                        index->rd_indcollation[index_column-1],operation,PointerGetDatum(other_keys));
                }
                if(endpoint){
                  for(int end=0;end<2;end++)if(endpoint&(1<<end)){
                    ScanKeyEntryInitialize(key,0,index_column,end?LP_PG_GIN_LAST_ANY:LP_PG_GIN_FIRST_ANY,array_type,
                        index->rd_indcollation[index_column-1],operation,PointerGetDatum(keys));
                    index_rescan(scan,key,nkeys,NULL,0);index_getbitmap(scan,bitmap);
                  }
                }else{
                    ScanKeyEntryInitialize(key,0,index_column,nomination,operand,index->rd_indcollation[index_column-1],operation,PointerGetDatum(keys));
                    index_rescan(scan,key,nkeys,NULL,0);index_getbitmap(scan,bitmap);
                }
                if(other_keys)pfree(other_keys);
            }
            pfree(keys);
        }
        TableScanDesc heap=table_beginscan_bm(table,snapshot,0,NULL);
        heap->st.rs_tbmiterator=tbm_begin_iterate(bitmap,NULL,InvalidDsaPointer);
        TupleTableSlot *slot=table_slot_create(table,NULL);
        bool recheck;uint64 lossy=0,exact=0;
        while(table_scan_bitmap_next_tuple(heap,slot,&recheck,&lossy,&exact)){
            CHECK_FOR_INTERRUPTS();
            MemoryContext previous=MemoryContextSwitchTo(scratch);
            bool selected=true;
            if(assertions && recheck){
                bool missing;Datum stored=slot_getattr(slot,path,&missing);
                if(missing)ereport(ERROR,(errmsg("Native assertion path is null")));
                selected=lp_pg_path_has_roles(stored,(1u<<LP_SAID_CLAIM)|(1u<<LP_SAID_RECORD));
            }
            if(selected && (endpoint?(!recheck || container_endpoint_matches(slot,path,wanted,endpoint)):
               ((!recheck && (strategy==3 || unique<=window)) || container_matches(slot,path,wanted,strategy==7,&epoch)))){
                slot->tts_tableOid=relation_id;
                TupleTableSlot *row=conversion?execute_attr_map_slot(conversion->attrMap,slot,projected):slot;
                if(projected){row->tts_tableOid=relation_id;row->tts_tid=slot->tts_tid;}
                proceed=visit(row,context);if(projected)ExecClearTuple(projected);
            }
            MemoryContextSwitchTo(previous);MemoryContextReset(scratch);ExecClearTuple(slot);
            if(!proceed)break;
        }
        ExecDropSingleTupleTableSlot(slot);
        if(projected){ExecDropSingleTupleTableSlot(projected);free_conversion_map(conversion);}
        tbm_end_iterate(&heap->st.rs_tbmiterator);table_endscan(heap);
        tbm_free(bitmap);index_endscan(scan);index_close(index,NoLock);table_close(table,NoLock);
        if(predicate)list_free_deep(predicate);
        if(!proceed)break;
    }
    if(required_keys)pfree(required_keys);if(mask_keys)pfree(mask_keys);MemoryContextDelete(scratch);list_free(relations);
}
typedef struct {AttrNumber id;lp_idmap *parents;} ContainerParents;
static bool container_parent(TupleTableSlot *slot,void *context)
{
    ContainerParents *out=context;container_remember(slot,out->id,out->parents);return true;
}
static void scan_containers(Relation parent,const char *column,lp_idmap *wanted,lp_idmap *parents,StrategyNumber strategy,lp_mask bits,bool assertions)
{
    ContainerParents out={get_attnum(RelationGetRelid(parent),"entity"),parents};
    scan_container_rows(parent,column,wanted,strategy,NULL,false,0,NULL,bits,assertions,container_parent,&out);
}
void lp_pg_lookup_containers(const lp_id *ids,size_t count,bool masks,bool assertions,lp_pg_path_visit visit,void *context)
{
    if(!count)return;
    const char *column;Relation parent=presence_parent(lp_pg_relation("physicality"),&column,masks?3:2);
    lp_idmap *wanted=lp_idmap_new(),*parents=lp_idmap_sized(sizeof(CarrierLocation));
    PG_TRY();{
        for(size_t i=0;i<count;i++)lp_idmap_put(wanted,ids+i,NULL);
        scan_containers(parent,column,wanted,parents,3,0,assertions);
        size_t found=lp_idmap_count(parents);
        PresenceKey *keys=palloc_extended(Max(found,(size_t)1)*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<found;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(parents,i),.ordinal=(int64)i+1};
        PathFrontier front={visit,context,get_attnum(RelationGetRelid(parent),"path"),
            masks?get_attnum(RelationGetRelid(parent),"mask"):InvalidAttrNumber,
            AllocSetContextCreate(CurrentMemoryContext,"Native acquired path tuple",ALLOCSET_DEFAULT_SIZES)};
        acquired_rows(parent,keys,found,parents,path_frontier_visit,&front,NULL);
        MemoryContextDelete(front.tuple_context);pfree(keys);table_close(parent,NoLock);
    }PG_FINALLY();{lp_idmap_free(wanted);lp_idmap_free(parents);}PG_END_TRY();
}

typedef struct {AttrNumber tier;int maximum;} TierMaximum;
static void stored_tier(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    TierMaximum *out=context;bool missing;Datum tier=slot_getattr(slot,out->tier,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native entity tier is null")));
    out->maximum=Max(out->maximum,DatumGetInt16(tier));
}
static int frontier_tier(const lp_id *ids,int count)
{
    const char *selected[]={"id","tier"},*column;Oid target=lp_pg_relation("entity");
    Relation parent=read_parent(target,&column,selected,lengthof(selected));
    /* Unstored basis/cache children still have real parents. A NULL SQL max
     * over entity must never suppress that entire containment frontier. */
    TierMaximum result={get_attnum(target,"tier"),0};
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++){
        keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    }
    indexed_keys(parent,column,keys,(size_t)count,stored_tier,&result);
    pfree(keys);table_close(parent,NoLock);return result.maximum;
}
typedef struct {
    PresenceOutput output;AttrNumber path,tier,mask;int floor;lp_mask bits;
    MemoryContext tuple_context;
} ContainerOutput;
static bool read_mask_bits(ArrayType *array,lp_mask *mask)
{
    Datum *bits;bool *missing;int count;bool possible=true;*mask=0;
    deconstruct_array(array,INT2OID,2,true,TYPALIGN_SHORT,&bits,&missing,&count);
    for(int i=0;i<count;i++){
        if(missing[i])ereport(ERROR,(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),errmsg("laplace: mask bit arrays cannot contain nulls")));
        int bit=DatumGetInt16(bits[i]);
        if(bit<0 || bit>=64)possible=false;else *mask|=UINT64_C(1)<<bit;
    }
    if(bits)pfree(bits);if(missing)pfree(missing);return possible;
}
static void container_row(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ContainerOutput *out=context;bool missing;
    Datum tier=slot_getattr(slot,out->tier,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native physicality tier is null")));
    if(DatumGetInt16(tier)<=out->floor)return;
    MemoryContext previous=MemoryContextSwitchTo(out->tuple_context);
    Datum mask=slot_getattr(slot,out->mask,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native physicality mask is null")));
    if(!out->bits || (lp_pg_mask_native(mask)&out->bits)==out->bits){
        Datum path=slot_getattr(slot,out->path,&missing);
        if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native physicality path is null")));
        Datum values[]={id_datum(&key->id),path,tier,mask};bool nulls[]={false,false,false,false};
        set_put(out->output.fcinfo,out->output.output,out->output.descriptor,values,nulls);
    }
    MemoryContextSwitchTo(previous);MemoryContextReset(out->tuple_context);
}
PG_FUNCTION_INFO_V1(laplace_containers);
Datum laplace_containers(PG_FUNCTION_ARGS)
{
    ContainerOutput out={0};out.output.fcinfo=fcinfo;
    out.output.output=set_begin(fcinfo,"laplace_containers",&out.output.descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(!count){pfree(ids);PG_RETURN_NULL();}
    if(!read_mask_bits(PG_GETARG_ARRAYTYPE_P(1),&out.bits)){pfree(ids);PG_RETURN_NULL();}
    out.floor=frontier_tier(ids,count);
    const char *selected[]={"entity","path","tier","mask"},*column;Oid target=lp_pg_relation("physicality");
    Relation parent=read_parent(target,&column,selected,lengthof(selected));
    out.path=get_attnum(target,"path");out.tier=get_attnum(target,"tier");out.mask=get_attnum(target,"mask");
    out.tuple_context=AllocSetContextCreate(CurrentMemoryContext,"Native container output",ALLOCSET_DEFAULT_SIZES);
    lp_idmap *wanted=lp_idmap_sized(sizeof(uint64)),*parents=lp_idmap_sized(sizeof(CarrierLocation));
    PG_TRY();{
        for(int i=0;i<count;i++){
            bool fresh;size_t at=lp_idmap_put(wanted,ids+i,&fresh);
            if(fresh)*(uint64 *)lp_idmap_at(wanted,at)=0;
        }
        scan_containers(parent,column,wanted,parents,7,out.bits,false);
        size_t found=lp_idmap_count(parents);
        PresenceKey *keys=palloc_extended((found?found:1)*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<found;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(parents,i),.ordinal=(int64)i+1};
        acquired_rows(parent,keys,found,parents,container_row,&out,NULL);pfree(keys);
    }PG_FINALLY();{lp_idmap_free(wanted);lp_idmap_free(parents);}PG_END_TRY();
    MemoryContextDelete(out.tuple_context);table_close(parent,NoLock);pfree(ids);PG_RETURN_NULL();
}

/* One overlap probe per bounded frontier, not one SQL executor per key or
 * partition. Per-part mode preserves request occurrences. A cap classifies
 * each key's claim and observation sets independently: a saturated set emits
 * one hub marker and none of whichever rows the heap happened to return. */
typedef struct {uint64 epoch,held[2];int head;} HolderKey;
typedef struct {
    PresenceOutput output;lp_idmap *wanted,*required,*seen;int *links;
    AttrNumber id,path,tier,mask;int floor,kinds;bool per_part;int64 cap;
    uint64 epoch,required_epoch,saturated;Tuplestorestate *pending;MemoryContext owner;
} HolderRead;
static void holder_emit(HolderRead *read,size_t key,Datum *values,bool *nulls)
{
    if(!read->per_part){values[0]=Int64GetDatum(0);set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values,nulls);return;}
    const HolderKey *part=lp_idmap_at(read->wanted,key);
    for(int at=part->head;at>=0;at=read->links[at]){
        values[0]=Int64GetDatum((int64)at+1);
        set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values,nulls);
    }
}
static bool holder_row(TupleTableSlot *slot,void *context)
{
    HolderRead *read=context;bool missing;
    Datum tier=slot_getattr(slot,read->tier,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder tier is null")));
    if(DatumGetInt16(tier)<=read->floor)return true;
    Datum mask=slot_getattr(slot,read->mask,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder mask is null")));
    int kind=(lp_pg_mask_native(mask)&UINT64_C(1))?1:0;
    if(!(read->kinds&(kind?1:2)))return true;
    Datum id=slot_getattr(slot,read->id,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder ID is null")));
    bool fresh;lp_idmap_put(read->seen,(const lp_id *)DatumGetPointer(id),&fresh);if(!fresh)return true;
    if(lp_idmap_count(read->required) && !container_matches(slot,read->path,read->required,true,&read->required_epoch))return true;
    Datum path=slot_getattr(slot,read->path,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder path is null")));
    Datum values[]={Int64GetDatum(0),id,path,tier,mask,Int16GetDatum((int16)kind),BoolGetDatum(false)};
    bool nulls[lengthof(values)]={false};
    if(!read->per_part){holder_emit(read,0,values,nulls);return true;}
    Geo geometry=geo_of(path);
    if(++read->epoch==0){
        for(size_t i=0;i<lp_idmap_count(read->wanted);i++)((HolderKey *)lp_idmap_at(read->wanted,i))->epoch=0;
        read->epoch=1;
    }
    for(size_t i=0;i<geometry.p.n;i++){
        lp_id child=lp_path_id(geometry.p,i);int64 at=lp_idmap_find(read->wanted,&child);if(at<0)continue;
        HolderKey *key=lp_idmap_at(read->wanted,(size_t)at);
        if(key->epoch==read->epoch)continue;key->epoch=read->epoch;
        if(read->cap<0){holder_emit(read,(size_t)at,values,nulls);continue;}
        if(key->held[kind]>(uint64)read->cap)continue;
        if(++key->held[kind]>(uint64)read->cap){read->saturated++;continue;}
        values[0]=Int64GetDatum(at+1);
        MemoryContext previous=MemoryContextSwitchTo(read->owner);
        tuplestore_putvalues(read->pending,read->output.descriptor,values,nulls);
        MemoryContextSwitchTo(previous);
    }
    size_t groups=lp_idmap_count(read->wanted)*((read->kinds&1?1:0)+(read->kinds&2?1:0));
    return read->cap<0 || read->saturated<groups;
}
PG_FUNCTION_INFO_V1(laplace_holders);
Datum laplace_holders(PG_FUNCTION_ARGS)
{
    HolderRead read={.output.fcinfo=fcinfo,.owner=CurrentMemoryContext,
        .per_part=PG_GETARG_BOOL(2),.floor=PG_GETARG_INT16(3),.cap=PG_GETARG_INT64(4),.kinds=PG_GETARG_INT16(5)};
    read.output.output=set_begin(fcinfo,"laplace_holders",&read.output.descriptor);
    if(read.cap < -1 || (!read.per_part && read.cap>=0) || read.kinds<1 || read.kinds>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holders: cap requires per-part mode; kinds must select claims (1), observations (2), or both (3)")));
    int count,nrequired;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count),*required=ids_of(PG_GETARG_ARRAYTYPE_P(1),&nrequired);
    if(!count){pfree(ids);pfree(required);PG_RETURN_NULL();}
    const char *selected[]={"entity","path","tier","mask"},*column;Oid target=lp_pg_relation("physicality");
    Relation parent=read_parent(target,&column,selected,lengthof(selected));
    read.id=get_attnum(target,"entity");read.path=get_attnum(target,"path");
    read.tier=get_attnum(target,"tier");read.mask=get_attnum(target,"mask");
    read.links=palloc_extended((Size)count*sizeof(*read.links),MCXT_ALLOC_HUGE);
    read.wanted=lp_idmap_sized(sizeof(HolderKey));read.required=lp_idmap_sized(sizeof(uint64));read.seen=lp_idmap_new();
    read.pending=read.cap>=0?tuplestore_begin_heap(true,false,work_mem):NULL;
    PG_TRY();{
        for(int i=count-1;i>=0;i--){
            bool fresh;size_t at=lp_idmap_put(read.wanted,ids+i,&fresh);HolderKey *key=lp_idmap_at(read.wanted,at);
            if(fresh){memset(key,0,sizeof(*key));key->head=-1;}
            read.links[i]=key->head;key->head=i;
        }
        for(int i=0;i<nrequired;i++){bool fresh;size_t at=lp_idmap_put(read.required,required+i,&fresh);if(fresh)*(uint64 *)lp_idmap_at(read.required,at)=0;}
        scan_container_rows(parent,column,read.wanted,3,read.required,false,0,NULL,read.kinds==1?UINT64_C(1):0,false,holder_row,&read);
        if(read.pending){
            TupleTableSlot *slot=MakeSingleTupleTableSlot(read.output.descriptor,&TTSOpsMinimalTuple);
            tuplestore_rescan(read.pending);
            while(tuplestore_gettupleslot(read.pending,true,false,slot)){
                CHECK_FOR_INTERRUPTS();slot_getallattrs(slot);
                size_t at=(size_t)DatumGetInt64(slot->tts_values[0])-1;int kind=DatumGetInt16(slot->tts_values[5]);
                if(((HolderKey *)lp_idmap_at(read.wanted,at))->held[kind]<=(uint64)read.cap)
                    holder_emit(&read,at,slot->tts_values,slot->tts_isnull);
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);tuplestore_end(read.pending);
            for(size_t at=0;at<lp_idmap_count(read.wanted);at++)for(int kind=0;kind<2;kind++){
                if(((HolderKey *)lp_idmap_at(read.wanted,at))->held[kind]<=(uint64)read.cap)continue;
                Datum values[]={0,0,0,0,0,Int16GetDatum((int16)kind),BoolGetDatum(true)};
                bool nulls[]={false,true,true,true,true,false,false};holder_emit(&read,at,values,nulls);
            }
        }
    }PG_FINALLY();{lp_idmap_free(read.wanted);lp_idmap_free(read.required);lp_idmap_free(read.seen);}PG_END_TRY();
    pfree(read.links);pfree(ids);pfree(required);table_close(parent,NoLock);PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_present_ids);
/* Claim selection is another projection of the same GIN/primary-key
 * frontier. Qualification precedes the caller's fan: masks, middle refusals,
 * tier and a real standing must all survive. ID order makes bounded results
 * independent of heap order; per-input duplicates retain their ordinals. */
typedef struct {uint64 epoch,sent;int head,floor;} ClaimKey;
typedef struct {CarrierLocation carrier;bool known;double rating,deviation,volatility;int64 matches;} ClaimStanding;
typedef struct {
    PresenceOutput output;lp_idmap *wanted,*parents,*required,*opposite;lp_id *refuse,*include;int nrefuse,ninclude,*links;
    bool each,done;uint64 limit,sent,saturated,epoch;int floor,endpoint;int64 arity;lp_mask bits;
    AttrNumber id,path,tier,mask,standing[4];MemoryContext tuple_context;
} ClaimRead;
static void claim_key_tier(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ClaimRead *read=context;bool missing;
    Datum value=slot_getattr(slot,read->tier,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim selector tier is null")));
    ((ClaimKey *)lp_idmap_at(read->wanted,(size_t)key->ordinal-1))->floor=DatumGetInt16(value);
}
static bool claim_nominate(TupleTableSlot *slot,void *context)
{
    ClaimRead *read=context;bool missing;Datum value=slot_getattr(slot,read->mask,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim mask is null")));
    if((lp_pg_mask_native(value)&read->bits)!=read->bits)return true;
    value=slot_getattr(slot,read->tier,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim tier is null")));
    int tier=DatumGetInt16(value);if(!read->each && tier<=read->floor)return true;
    value=slot_getattr(slot,read->path,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim path is null")));
    Geo path=geo_of(value);
    if(read->arity && lp_path_len(path.p)!=(uint64)read->arity)return true;
    if(read->opposite){
        if(!path.p.n)return true;
        lp_id id=lp_path_id(path.p,read->endpoint==1?path.p.n-1:0);
        if(lp_idmap_find(read->opposite,&id)<0)return true;
    }
    if(lp_path_middle_any(path.p,read->refuse,(size_t)read->nrefuse))return true;
    if(read->include && !lp_path_middle_any(path.p,read->include,(size_t)read->ninclude))return true;
    if(read->each){
        bool eligible=false;
        for(size_t i=0;i<path.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==path.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(path.p,i);int64 at=lp_idmap_find(read->wanted,&id);
            if(at>=0 && tier>((ClaimKey *)lp_idmap_at(read->wanted,(size_t)at))->floor){eligible=true;break;}
        }
        if(!eligible)return true;
    }
    value=slot_getattr(slot,read->id,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim identity is null")));
    bool fresh;size_t at=lp_idmap_put(read->parents,(const lp_id *)DatumGetPointer(value),&fresh);
    ClaimStanding *standing=lp_idmap_at(read->parents,at);
    if(fresh)memset(standing,0,sizeof(*standing));
    standing->carrier=(CarrierLocation){slot->tts_tableOid,slot->tts_tid};
    return true;
}
static void claim_standing(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ClaimRead *read=context;ClaimStanding *standing=lp_idmap_at(read->parents,(size_t)key->ordinal-1);
    Datum values[4];
    for(int i=0;i<4;i++){bool missing;values[i]=slot_getattr(slot,read->standing[i],&missing);if(missing)ereport(ERROR,(errmsg("Native claim standing is null")));}
    *standing=(ClaimStanding){.carrier=standing->carrier,.known=true,.rating=DatumGetFloat8(values[0]),
        .deviation=DatumGetFloat8(values[1]),.volatility=DatumGetFloat8(values[2]),.matches=DatumGetInt64(values[3])};
}
static void claim_carrier(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ClaimRead *read=context;const ClaimStanding *standing=lp_idmap_at(read->parents,(size_t)key->ordinal-1);
    MemoryContext previous=MemoryContextSwitchTo(read->tuple_context);bool missing;
    Datum path=slot_getattr(slot,read->path,&missing);if(missing)ereport(ERROR,(errmsg("Native claim carrier is null")));
    Datum values[]={0,PointerGetDatum(&key->id),path,Float8GetDatum(standing->rating),Float8GetDatum(standing->deviation),Float8GetDatum(standing->volatility),Int64GetDatum(standing->matches)};
    bool nulls[lengthof(values)]={false};
    if(!read->each){
        set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values+1,nulls+1);
        read->done=++read->sent>=read->limit;
    }else{
        Datum tier=slot_getattr(slot,read->tier,&missing);if(missing)ereport(ERROR,(errmsg("Native claim carrier tier is null")));
        Geo geometry=geo_of(path);
        if(++read->epoch==0){for(size_t i=0;i<lp_idmap_count(read->wanted);i++)((ClaimKey *)lp_idmap_at(read->wanted,i))->epoch=0;read->epoch=1;}
        for(size_t i=0;i<geometry.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==geometry.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(geometry.p,i);int64 at=lp_idmap_find(read->wanted,&id);if(at<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)at);
            if(part->epoch==read->epoch || part->sent>=read->limit || DatumGetInt16(tier)<=part->floor)continue;
            part->epoch=read->epoch;
            for(int input=part->head;input>=0;input=read->links[input]){
                values[0]=Int64GetDatum((int64)input+1);set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values,nulls);
            }
            if(++part->sent==read->limit)read->saturated++;
        }
        read->done=read->saturated==lp_idmap_count(read->wanted);
    }
    MemoryContextSwitchTo(previous);MemoryContextReset(read->tuple_context);
}
static Datum read_claims(FunctionCallInfo fcinfo,bool each)
{
    ClaimRead read={.output.fcinfo=fcinfo,.each=each,.limit=UINT64_MAX};
    read.output.output=set_begin(fcinfo,each?"laplace_claims_each":"laplace_claims",&read.output.descriptor);
    if(PG_ARGISNULL(0)||PG_ARGISNULL(2)||PG_ARGISNULL(3))return (Datum)0;
    if(PG_NARGS()>4){
        if(PG_ARGISNULL(4))return (Datum)0;
        read.endpoint=PG_GETARG_INT16(4);
        if(read.endpoint<0 || read.endpoint>3)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native claim endpoint must be 0 (any), 1 (first), 2 (last) or 3 (either)")));
    }
    if(PG_NARGS()>6 && !PG_ARGISNULL(6)){
        read.arity=PG_GETARG_INT64(6);
        if(read.arity<0)ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native claim arity must be zero or positive")));
    }
    if(!PG_ARGISNULL(1)){
        int64 limit=PG_GETARG_INT64(1);
        if(limit<0)ereport(ERROR,(errcode(ERRCODE_INVALID_ROW_COUNT_IN_LIMIT_CLAUSE),errmsg("Native claim fan must not be negative")));
        read.limit=(uint64)limit;if(!limit)return (Datum)0;
    }
    if(!read_mask_bits(PG_GETARG_ARRAYTYPE_P(2),&read.bits))return (Datum)0;
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    read.refuse=ids_of(PG_GETARG_ARRAYTYPE_P(3),&read.nrefuse);
    if(!count){pfree(ids);pfree(read.refuse);return (Datum)0;}
    if(PG_NARGS()>5 && !PG_ARGISNULL(5)){
        read.include=ids_of(PG_GETARG_ARRAYTYPE_P(5),&read.ninclude);
        if(!read.ninclude){pfree(ids);pfree(read.refuse);pfree(read.include);return (Datum)0;}
    }
    lp_id *opposite=NULL;int nopposite=0;
    if(PG_NARGS()>7 && !PG_ARGISNULL(7)){
        if(read.endpoint!=1 && read.endpoint!=2)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native opposite endpoint requires first or last endpoint selection")));
        opposite=ids_of(PG_GETARG_ARRAYTYPE_P(7),&nopposite);
        if(!nopposite){pfree(ids);pfree(read.refuse);if(read.include)pfree(read.include);pfree(opposite);return (Datum)0;}
        read.opposite=lp_idmap_new();
    }
    read.wanted=lp_idmap_sized(sizeof(ClaimKey));read.parents=lp_idmap_sized(sizeof(ClaimStanding));read.required=lp_idmap_new();
    read.links=palloc_extended((Size)count*sizeof(int),MCXT_ALLOC_HUGE);
    PG_TRY();{
        for(int i=0;i<nopposite;i++)lp_idmap_put(read.opposite,opposite+i,NULL);
        for(int i=0;i<read.ninclude;i++)lp_idmap_put(read.required,read.include+i,NULL);
        for(int i=count-1;i>=0;i--){bool fresh;size_t at=lp_idmap_put(read.wanted,ids+i,&fresh);ClaimKey *part=lp_idmap_at(read.wanted,at);
            if(fresh){memset(part,0,sizeof(*part));part->head=-1;}read.links[i]=part->head;part->head=i;}
        const char *column,*entity_columns[]={"id","tier"};Oid target=lp_pg_relation("entity");
        Relation entities=read_parent(target,&column,entity_columns,lengthof(entity_columns));
        size_t n=lp_idmap_count(read.wanted);PresenceKey *keys=palloc_extended(n*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<n;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(read.wanted,i),.ordinal=(int64)i+1};
        read.tier=get_attnum(target,"tier");indexed_keys(entities,column,keys,n,claim_key_tier,&read);
        table_close(entities,NoLock);pfree(keys);
        for(size_t i=0;i<n;i++)read.floor=Max(read.floor,((ClaimKey *)lp_idmap_at(read.wanted,i))->floor);
        const char *path_columns[]={"entity","path","tier","mask"};target=lp_pg_relation("physicality");
        Relation paths=read_parent(target,&column,path_columns,lengthof(path_columns));
        read.id=get_attnum(target,"entity");read.path=get_attnum(target,"path");read.tier=get_attnum(target,"tier");read.mask=get_attnum(target,"mask");
        scan_container_rows(paths,column,read.wanted,each?3:7,read.required,true,read.endpoint,read.opposite,read.bits,false,claim_nominate,&read);
        n=lp_idmap_count(read.parents);keys=palloc_extended((n?n:1)*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<n;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(read.parents,i),.ordinal=(int64)i+1};
        const char *standing_columns[]={"claim","rating","deviation","volatility","matches"};target=lp_pg_relation("consensus");
        Relation standings=read_parent(target,&column,standing_columns,lengthof(standing_columns));
        for(int i=0;i<4;i++)read.standing[i]=get_attnum(target,standing_columns[i+1]);
        indexed_keys(standings,column,keys,n,claim_standing,&read);table_close(standings,NoLock);
        size_t known=0;for(size_t i=0;i<n;i++)if(((ClaimStanding *)lp_idmap_at(read.parents,(size_t)keys[i].ordinal-1))->known)keys[known++]=keys[i];
        read.tuple_context=AllocSetContextCreate(CurrentMemoryContext,"Native selected claim",ALLOCSET_DEFAULT_SIZES);
        acquired_rows(paths,keys,known,read.parents,claim_carrier,&read,&read.done);
        MemoryContextDelete(read.tuple_context);pfree(keys);table_close(paths,NoLock);
    }PG_FINALLY();{lp_idmap_free(read.wanted);lp_idmap_free(read.parents);lp_idmap_free(read.required);lp_idmap_free(read.opposite);}PG_END_TRY();
    pfree(ids);pfree(read.refuse);if(read.include)pfree(read.include);if(opposite)pfree(opposite);pfree(read.links);return (Datum)0;
}
PG_FUNCTION_INFO_V1(laplace_claims);
Datum laplace_claims(PG_FUNCTION_ARGS){return read_claims(fcinfo,false);}
PG_FUNCTION_INFO_V1(laplace_claims_each);
Datum laplace_claims_each(PG_FUNCTION_ARGS){return read_claims(fcinfo,true);}

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
    const char *column;Relation parent=presence_parent(lp_pg_relation("entity"),&column,1);
    indexed_keys(parent,column,keys,wanted,present_bitmap_row,present);
    pfree(keys);table_close(parent,NoLock);
}
Datum laplace_present_ids(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;Tuplestorestate *output=set_begin(fcinfo,"laplace_present_ids",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(!count){pfree(ids);PG_RETURN_NULL();}
    const char *column;
    Relation parent=presence_parent(lp_pg_relation("entity"),&column,1);
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=i+1};
    presence_keys(fcinfo,output,descriptor,parent,column,keys,count);
    pfree(keys);pfree(ids);table_close(parent,NoLock);PG_RETURN_NULL();
}

typedef struct {
    FunctionCallInfo call;Tuplestorestate *rows;TupleDesc descriptor;Relation parent;
    const char *column;PresenceKey *keys;int used,capacity;int64 ordinal;
} PresenceBatch;
static void presence_flush(PresenceBatch *batch)
{
    if(!batch->used)return;
    presence_keys(batch->call,batch->rows,batch->descriptor,batch->parent,batch->column,batch->keys,batch->used);
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
    const char *column;Relation parent=presence_parent(target,&column,1);
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
    for(size_t i=0;i<batch->used;i++)batch->keys[i].partition=enrichment_partition(batch->target,key,partitions,PointerGetDatum(&batch->keys[i].id));
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
    Oid partition=enrichment_partition(stage->target,RelationGetPartitionKey(stage->parent),RelationGetPartitionDesc(stage->parent,true),id);
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
