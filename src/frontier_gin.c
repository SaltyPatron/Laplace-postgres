/* Shared GIN containment scanner and container/holder/claim frontier queries
 * (lp_pg_lookup_containers, laplace_containers, laplace_holders, laplace_claims). */
#include "pg_core.h"
#include "geo.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/gin.h"
#include "access/tupconvert.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/tidbitmap.h"
#include "parser/parse_func.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "partitioning/partdesc.h"
#include <limits.h>

/* GIN is the retained containment access contract, just as the primary index
 * is the retained identity access contract. Unknown SPI array sizes can
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
    PartitionDesc partdesc=parent->rd_rel->relkind==RELKIND_PARTITIONED_TABLE?RelationGetPartitionDesc(parent,true):NULL;
    int nparts=partdesc?partdesc->nparts:1;
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
    ArrayType *required_keys=(nrequired && !endpoint)?id_array_of(element,lp_idmap_keys(required),(int)(required_any?nrequired:Min(budget,nrequired))):NULL;
    Datum mask_values[64];int nbits=0;
    for(int bit=0;bit<64;bit++)if(bits&(UINT64_C(1)<<bit))mask_values[nbits++]=Int16GetDatum((int16)bit);
    ArrayType *mask_keys=nbits?construct_array(mask_values,nbits,INT2OID,2,true,TYPALIGN_SHORT):NULL;
    for(int p_idx=0;p_idx<nparts;p_idx++){
        Oid relation_id=partdesc?partdesc->oids[p_idx]:RelationGetRelid(parent);
        CHECK_FOR_INTERRUPTS();Relation table=table_open(relation_id,AccessShareLock);
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
            BlockNumber entry_pages=stats.nEntryPages>1?stats.nEntryPages:RelationGetNumberOfBlocks(index);
            /* Each exact key probe descends the entry B-tree (~4 pages per
             * key), whereas LP_STRAT_OVERLAPS streams stats.nEntryPages leaf
             * pages once against a shared PathOverlapSet. Use exact seeks only
             * when those B-tree probes touch fewer pages than streaming the
             * dictionary, and keep a single full-set window when streaming so
             * work_mem slicing never rescans the dictionary multiple times. */
            if(entry_pages>0 && unique<=((uint64)entry_pages+3)/4)
                nomination=LP_PG_GIN_EXACT_OVERLAPS;
            else
                window=Max(window,unique);
            if(first_leaf)elog(DEBUG1,"Native containment index: entries " UINT64_FORMAT "; entry_pages %u; strategy %u; window %zu",stats.nEntries,entry_pages,nomination,window);
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
    if(required_keys)pfree(required_keys);if(mask_keys)pfree(mask_keys);MemoryContextDelete(scratch);
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
typedef struct {AttrNumber id,path,mask;lp_idmap *parents;lp_pg_path_visit visit;void *context;} DirectContainers;
static bool direct_container_visit(TupleTableSlot *slot,void *context)
{
    DirectContainers *out=context;bool id_null,path_null,mask_null=false;
    Datum id=slot_getattr(slot,out->id,&id_null);
    if(id_null)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native containment parent identity is null")));
    const lp_id *key=(const lp_id *)DatumGetPointer(id);
    bool fresh;lp_idmap_put(out->parents,key,&fresh);if(!fresh)return true;
    Datum path=slot_getattr(slot,out->path,&path_null),mask=(Datum)0;
    if(out->mask>0)mask=slot_getattr(slot,out->mask,&mask_null);
    if(path_null||mask_null)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native path frontier: null carrier")));
    out->visit(key,path,mask,out->context);
    return true;
}
void lp_pg_lookup_containers(const lp_id *ids,size_t count,bool masks,bool assertions,lp_pg_path_visit visit,void *context)
{
    if(!count)return;
    const char *column;Relation parent=lp_pg_presence_parent(lp_pg_relation("physicality"),&column,masks?3:2);
    lp_idmap *wanted=lp_idmap_new(),*parents=lp_idmap_new();
    PG_TRY();{
        for(size_t i=0;i<count;i++)lp_idmap_put(wanted,ids+i,NULL);
        DirectContainers out={get_attnum(RelationGetRelid(parent),"entity"),
            get_attnum(RelationGetRelid(parent),"path"),
            masks?get_attnum(RelationGetRelid(parent),"mask"):InvalidAttrNumber,
            parents,visit,context};
        scan_container_rows(parent,column,wanted,3,NULL,false,0,NULL,0,assertions,direct_container_visit,&out);
        table_close(parent,NoLock);
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
    Relation parent=lp_pg_read_parent(target,&column,selected,lengthof(selected));
    /* Unstored basis/cache children still have real parents. A NULL SQL max
     * over entity must never suppress that entire containment frontier. */
    TierMaximum result={get_attnum(target,"tier"),0};
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++){
        keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    }
    lp_pg_indexed_keys(parent,column,keys,(size_t)count,stored_tier,&result);
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
    Relation parent=lp_pg_read_parent(target,&column,selected,lengthof(selected));
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
        lp_pg_acquired_rows(parent,keys,found,parents,container_row,&out,NULL);pfree(keys);
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
    AttrNumber id,path,tier,mask;int floor,kinds,endpoint;bool per_part;int64 cap;lp_mask bits;
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
    lp_mask native_mask=lp_pg_mask_native(mask);
    if((native_mask&read->bits)!=read->bits)return true;
    int kind=(native_mask&UINT64_C(1))?1:0;
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
        if(read->endpoint){
            bool first=i==0,last=i+1==geometry.p.n;
            if(!((read->endpoint&1 && first) || (read->endpoint&2 && last)))continue;
        }
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
    if(PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3) || PG_ARGISNULL(4) || PG_ARGISNULL(5))
        PG_RETURN_NULL();
    HolderRead read={.output.fcinfo=fcinfo,.owner=CurrentMemoryContext,
        .per_part=PG_GETARG_BOOL(2),.floor=PG_GETARG_INT16(3),.cap=PG_GETARG_INT64(4),.kinds=PG_GETARG_INT16(5)};
    read.output.output=set_begin(fcinfo,"laplace_holders",&read.output.descriptor);
    if(PG_NARGS()>6 && !PG_ARGISNULL(6) && !read_mask_bits(PG_GETARG_ARRAYTYPE_P(6),&read.bits))PG_RETURN_NULL();
    if(PG_NARGS()>7 && !PG_ARGISNULL(7)){
        read.endpoint=PG_GETARG_INT16(7);
        if(read.endpoint<0 || read.endpoint>3)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holders: endpoint must be 0 (any), 1 (first), 2 (last), or 3 (either)")));
    }
    if(read.kinds==1)read.bits|=UINT64_C(1);
    if(read.cap < -1 || (!read.per_part && read.cap>=0) || read.kinds<1 || read.kinds>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holders: cap requires per-part mode; kinds must select claims (1), observations (2), or both (3)")));
    int count,nrequired;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count),*required=ids_of(PG_GETARG_ARRAYTYPE_P(1),&nrequired);
    if(!count){pfree(ids);pfree(required);PG_RETURN_NULL();}
    const char *selected[]={"entity","path","tier","mask"},*column;Oid target=lp_pg_relation("physicality");
    Relation parent=lp_pg_read_parent(target,&column,selected,lengthof(selected));
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
        scan_container_rows(parent,column,read.wanted,3,read.required,false,read.endpoint,NULL,read.bits,false,holder_row,&read);
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

/* Claim selection is another projection of the same GIN/primary-key
 * frontier. Qualification precedes the caller's fan: masks, middle refusals,
 * tier and a real standing must all survive. ID order makes bounded results
 * independent of heap order; per-input duplicates retain their ordinals. */
typedef struct {uint64 epoch,sent,nom_epoch,nominated;int head,floor;} ClaimKey;
typedef struct {CarrierLocation carrier;Datum path;int16 tier;bool known;double rating,deviation,volatility;int64 matches;} ClaimStanding;
typedef struct {
    PresenceOutput output;lp_idmap *wanted,*parents,*required,*opposite;lp_id *refuse,*include;int nrefuse,ninclude,*links;
    bool each,done;uint64 limit,sent,saturated,epoch,nom_epoch,nom_saturated;int floor,endpoint;int64 arity;lp_mask bits;
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
    Datum path_datum=slot_getattr(slot,read->path,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim path is null")));
    bytea *detoasted=(bytea *)PG_DETOAST_DATUM(path_datum);
    Geo path=geo_of(PointerGetDatum(detoasted));
    if(read->arity && lp_path_len(path.p)!=(uint64)read->arity)return true;
    if(read->opposite){
        if(!path.p.n)return true;
        lp_id id=lp_path_id(path.p,read->endpoint==1?path.p.n-1:0);
        if(lp_idmap_find(read->opposite,&id)<0)return true;
    }
    if(lp_path_middle_any(path.p,read->refuse,(size_t)read->nrefuse))return true;
    if(read->include && !lp_path_middle_any(path.p,read->include,(size_t)read->ninclude))return true;
    uint64 nom_cap=read->limit<UINT64_MAX?Max(read->limit,UINT64_C(64)):UINT64_MAX;
    if(read->each){
        bool eligible=false;
        for(size_t i=0;i<path.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==path.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(path.p,i);int64 at=lp_idmap_find(read->wanted,&id);
            if(at<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)at);
            if(tier>part->floor && part->nominated<nom_cap){eligible=true;break;}
        }
        if(!eligible)return nom_cap==UINT64_MAX || read->nom_saturated<lp_idmap_count(read->wanted);
    }
    value=slot_getattr(slot,read->id,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim identity is null")));
    bool fresh;size_t at=lp_idmap_put(read->parents,(const lp_id *)DatumGetPointer(value),&fresh);
    ClaimStanding *standing=lp_idmap_at(read->parents,at);
    if(fresh){
        memset(standing,0,sizeof(*standing));
        Size sz=VARSIZE_ANY(detoasted);
        bytea *copied=MemoryContextAlloc(read->tuple_context,sz);
        memcpy(copied,detoasted,sz);
        standing->path=PointerGetDatum(copied);
        standing->tier=(int16)tier;
    }
    standing->carrier=(CarrierLocation){slot->tts_tableOid,slot->tts_tid};
    if(nom_cap<UINT64_MAX){
        if(!read->each)return lp_idmap_count(read->parents)<nom_cap;
        if(++read->nom_epoch==0){
            for(size_t i=0;i<lp_idmap_count(read->wanted);i++)((ClaimKey *)lp_idmap_at(read->wanted,i))->nom_epoch=0;
            read->nom_epoch=1;
        }
        for(size_t i=0;i<path.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==path.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(path.p,i);int64 ka=lp_idmap_find(read->wanted,&id);
            if(ka<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)ka);
            if(part->nom_epoch==read->nom_epoch || tier<=part->floor || part->nominated>=nom_cap)continue;
            part->nom_epoch=read->nom_epoch;
            if(++part->nominated>=nom_cap)read->nom_saturated++;
        }
        return read->nom_saturated<lp_idmap_count(read->wanted);
    }
    return true;
}
static void claim_standing(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ClaimRead *read=context;ClaimStanding *standing=lp_idmap_at(read->parents,(size_t)key->ordinal-1);
    Datum values[4];
    for(int i=0;i<4;i++){bool missing;values[i]=slot_getattr(slot,read->standing[i],&missing);if(missing)ereport(ERROR,(errmsg("Native claim standing is null")));}
    *standing=(ClaimStanding){.carrier=standing->carrier,.path=standing->path,.tier=standing->tier,.known=true,.rating=DatumGetFloat8(values[0]),
        .deviation=DatumGetFloat8(values[1]),.volatility=DatumGetFloat8(values[2]),.matches=DatumGetInt64(values[3])};
}
static void claim_emit_cached(ClaimRead *read,const PresenceKey *key)
{
    const ClaimStanding *standing=lp_idmap_at(read->parents,(size_t)key->ordinal-1);
    Datum path=standing->path;
    Datum values[]={0,PointerGetDatum(&key->id),path,Float8GetDatum(standing->rating),Float8GetDatum(standing->deviation),Float8GetDatum(standing->volatility),Int64GetDatum(standing->matches)};
    bool nulls[lengthof(values)]={false};
    if(!read->each){
        set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values+1,nulls+1);
        read->done=++read->sent>=read->limit;
    }else{
        Geo geometry=geo_of(path);
        if(++read->epoch==0){for(size_t i=0;i<lp_idmap_count(read->wanted);i++)((ClaimKey *)lp_idmap_at(read->wanted,i))->epoch=0;read->epoch=1;}
        for(size_t i=0;i<geometry.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==geometry.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(geometry.p,i);int64 at=lp_idmap_find(read->wanted,&id);if(at<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)at);
            if(part->epoch==read->epoch || part->sent>=read->limit || standing->tier<=part->floor)continue;
            part->epoch=read->epoch;
            for(int input=part->head;input>=0;input=read->links[input]){
                values[0]=Int64GetDatum((int64)input+1);set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values,nulls);
            }
            if(++part->sent==read->limit)read->saturated++;
        }
        read->done=read->saturated==lp_idmap_count(read->wanted);
    }
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
        Relation entities=lp_pg_read_parent(target,&column,entity_columns,lengthof(entity_columns));
        size_t n=lp_idmap_count(read.wanted);PresenceKey *keys=palloc_extended(n*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<n;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(read.wanted,i),.ordinal=(int64)i+1};
        read.tier=get_attnum(target,"tier");lp_pg_indexed_keys(entities,column,keys,n,claim_key_tier,&read);
        table_close(entities,NoLock);pfree(keys);
        for(size_t i=0;i<n;i++)read.floor=Max(read.floor,((ClaimKey *)lp_idmap_at(read.wanted,i))->floor);
        read.tuple_context=AllocSetContextCreate(CurrentMemoryContext,"Native selected claim",ALLOCSET_DEFAULT_SIZES);
        const char *path_columns[]={"entity","path","tier","mask"};target=lp_pg_relation("physicality");
        Relation paths=lp_pg_read_parent(target,&column,path_columns,lengthof(path_columns));
        read.id=get_attnum(target,"entity");read.path=get_attnum(target,"path");read.tier=get_attnum(target,"tier");read.mask=get_attnum(target,"mask");
        scan_container_rows(paths,column,read.wanted,each?3:7,read.required,true,read.endpoint,read.opposite,read.bits,false,claim_nominate,&read);
        table_close(paths,NoLock);
        n=lp_idmap_count(read.parents);keys=palloc_extended((n?n:1)*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<n;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(read.parents,i),.ordinal=(int64)i+1};
        const char *standing_columns[]={"claim","rating","deviation","volatility","matches"};target=lp_pg_relation("consensus");
        Relation standings=lp_pg_read_parent(target,&column,standing_columns,lengthof(standing_columns));
        for(int i=0;i<4;i++)read.standing[i]=get_attnum(target,standing_columns[i+1]);
        lp_pg_indexed_keys(standings,column,keys,n,claim_standing,&read);table_close(standings,NoLock);
        for(size_t i=0;i<n && !read.done;i++){
            if(((ClaimStanding *)lp_idmap_at(read.parents,(size_t)keys[i].ordinal-1))->known)
                claim_emit_cached(&read,keys+i);
        }
        MemoryContextDelete(read.tuple_context);read.tuple_context=NULL;pfree(keys);
    }PG_FINALLY();{if(read.tuple_context)MemoryContextDelete(read.tuple_context);lp_idmap_free(read.wanted);lp_idmap_free(read.parents);lp_idmap_free(read.required);lp_idmap_free(read.opposite);}PG_END_TRY();
    pfree(ids);pfree(read.refuse);if(read.include)pfree(read.include);if(opposite)pfree(opposite);pfree(read.links);return (Datum)0;
}
PG_FUNCTION_INFO_V1(laplace_claims);
Datum laplace_claims(PG_FUNCTION_ARGS){return read_claims(fcinfo,false);}
PG_FUNCTION_INFO_V1(laplace_claims_each);
Datum laplace_claims_each(PG_FUNCTION_ARGS){return read_claims(fcinfo,true);}
