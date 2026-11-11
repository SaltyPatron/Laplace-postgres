/* Shared GIN containment scanner and container/holder/claim frontier queries
 * (lp_pg_lookup_containers, laplace_containers, laplace_holders, laplace_claims). */
#include "pg_core.h"
#include "geo.h"
#include "access/relation.h"
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
static Relation container_index(Relation table,AttrNumber path,Oid array_type,StrategyNumber strategy,List *predicate,int *column,Oid *operation,Oid *cached_id,int *cached_col){
    if(cached_id && OidIsValid(*cached_id)){
        Relation index=try_relation_open(*cached_id,AccessShareLock);
        if(index && index->rd_rel->relam==GIN_AM_OID && index->rd_index && index->rd_index->indrelid==RelationGetRelid(table) &&
           index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive &&
           (!index->rd_index->indcheckxmin || TransactionIdPrecedes(HeapTupleHeaderGetXmin(index->rd_indextuple->t_data),TransactionXmin)) &&
           *cached_col>=1 && *cached_col<=index->rd_index->indnkeyatts && index->rd_index->indkey.values[*cached_col-1]==path){
            Oid op=get_opfamily_member(index->rd_opfamily[*cached_col-1],get_atttype(RelationGetRelid(table),path),array_type,strategy);
            if(OidIsValid(op)){*column=*cached_col;*operation=get_opcode(op);return index;}
        }
        if(index)index_close(index,NoLock);*cached_id=InvalidOid;
    }
    List *indexes=RelationGetIndexList(table);Relation found=NULL;ListCell *lc;
    foreach(lc,indexes){
        Oid index_id=lfirst_oid(lc);Relation index=index_open(index_id,AccessShareLock);
        if(index->rd_rel->relam==GIN_AM_OID && index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive &&
           (!index->rd_index->indcheckxmin || TransactionIdPrecedes(HeapTupleHeaderGetXmin(index->rd_indextuple->t_data),TransactionXmin)) &&
           heap_attisnull(index->rd_indextuple,Anum_pg_index_indpred,NULL)==(predicate==NIL) &&
           (predicate==NIL || equal(RelationGetIndexPredicate(index),predicate))){
            for(int i=0;i<index->rd_index->indnkeyatts;i++)if(index->rd_index->indkey.values[i]==path){
                Oid op=get_opfamily_member(index->rd_opfamily[i],get_atttype(RelationGetRelid(table),path),array_type,strategy);
                if(OidIsValid(op)){*column=i+1;*operation=get_opcode(op);found=index;break;}
            }
        }
        if(found)break;index_close(index,NoLock);
    }
    list_free(indexes);
    if(!found)ereport(ERROR,(errcode(ERRCODE_UNDEFINED_OBJECT),
        errmsg("Native containment requires a complete %sGIN path index on %s",predicate?"assertion ":"",RelationGetRelationName(table))));
    if(cached_id){*cached_id=RelationGetRelid(found);*cached_col=*column;}
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
typedef struct {uint64 epoch,held[2];int head;} HolderKey;
typedef struct {uint64 epoch[2],sent[2],nom_epoch[2],nominated[2];int head,floor;} ClaimKey;
static bool key_active(lp_idmap *wanted,size_t at,uint64 h_kinds,uint64 nom_cap,int dir){
    if(h_kinds){const HolderKey *hk=lp_idmap_at(wanted,at);return ((h_kinds&1) && hk->held[1]<nom_cap) || ((h_kinds&2) && hk->held[0]<nom_cap);}
    return ((const ClaimKey *)lp_idmap_at(wanted,at))->nominated[dir]<nom_cap;
}
typedef bool (*ContainerVisit)(TupleTableSlot *slot,void *context);
static void scan_container_rows(Relation parent,const char *column,lp_idmap *wanted,StrategyNumber strategy,
                                 lp_idmap *required,bool required_any,int endpoint,lp_idmap *opposite,lp_mask bits,bool assertions,uint64 nom_cap,ContainerVisit visit,void *context)
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
    bool indexed_opposite=opposite && lp_idmap_count(opposite)>0 && lp_idmap_count(opposite)<=budget;
    bool first_leaf=true;
    elog(DEBUG1,"Native containment frontier: %zu IDs; strategy %u; endpoint %d",unique,strategy,endpoint);
    size_t nrequired=required?lp_idmap_count(required):0;
    ArrayType *required_keys=(nrequired && !endpoint)?id_array_of(element,lp_idmap_keys(required),(int)(required_any?nrequired:Min(budget,nrequired))):NULL;
    Datum mask_values[64];int nbits=0;
    for(int bit=0;bit<64;bit++)if(bits&(UINT64_C(1)<<bit))mask_values[nbits++]=Int16GetDatum((int16)bit);
    ArrayType *mask_keys=nbits?construct_array(mask_values,nbits,INT2OID,2,true,TYPALIGN_SHORT):NULL;
    uint64 h_kinds=nom_cap>>60;nom_cap&=(UINT64_C(1)<<60)-1;
    ArrayType *single_keys=(unique<=budget && !indexed_opposite && !nom_cap)?id_array_of(element,lp_idmap_keys(wanted),(int)unique):NULL;
    lp_id *nom_buf=(nom_cap && (endpoint || h_kinds))?palloc(sizeof(lp_id)*(1+2*Min(budget,unique))):NULL;
    Size bm_mem=Min((Size)work_mem*1024,(Size)4*1024*1024);
    TIDBitmap *bitmap=tbm_create(bm_mem,NULL);
    AttrNumber parent_path=get_attnum(RelationGetRelid(parent),"path");
    List *parent_predicate=NIL;
    static Oid cached_parent[2],cached_idx[2][256];static int cached_col[2][256];static uint64 cached_gen;
    int a_slot=assertions?1:0;
    if(cached_gen!=lp_pg_binding_generation()){memset(cached_idx,0,sizeof(cached_idx));memset(cached_parent,0,sizeof(cached_parent));cached_gen=lp_pg_binding_generation();}
    if(cached_parent[a_slot]!=RelationGetRelid(parent)){memset(cached_idx[a_slot],0,sizeof(cached_idx[a_slot]));cached_parent[a_slot]=RelationGetRelid(parent);}
    for(int p_idx=0;p_idx<nparts;p_idx++){
        Oid relation_id=partdesc?partdesc->oids[p_idx]:RelationGetRelid(parent);
        CHECK_FOR_INTERRUPTS();Relation table=table_open(relation_id,AccessShareLock);
        if(table->rd_rel->relkind==RELKIND_PARTITIONED_TABLE){table_close(table,NoLock);continue;}
        if(!table->rd_tableam || !table->rd_tableam->scan_bitmap_next_tuple)
            ereport(ERROR,(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),errmsg("Native containment requires bitmap table access")));
        AttrNumber path=get_attnum(relation_id,"path");
        int index_column;Oid operation;
        Oid operand=endpoint?element:array_type;
        StrategyNumber nomination=endpoint?((endpoint&1)?8:9):strategy;
        Oid *slot_id=(p_idx<256 && path==parent_path)?&cached_idx[a_slot][p_idx]:NULL;
        int *slot_col=(p_idx<256 && path==parent_path)?&cached_col[a_slot][p_idx]:NULL;
        List *predicate=NIL;
        if(assertions && (!slot_id || !OidIsValid(*slot_id))){
            if(!parent_predicate){
                Expr *roles=(Expr *)makeConst(INT2OID,-1,InvalidOid,sizeof(int16),Int16GetDatum((1u<<LP_SAID_CLAIM)|(1u<<LP_SAID_RECORD)),false,true);
                parent_predicate=list_make1(makeFuncExpr(assertion_function,BOOLOID,
                    list_make2(makeVar(1,parent_path,get_atttype(RelationGetRelid(parent),parent_path),-1,InvalidOid,0),roles),InvalidOid,InvalidOid,COERCE_EXPLICIT_CALL));
            }
            predicate=parent_predicate;
            if(path!=parent_path){
                Expr *roles=(Expr *)makeConst(INT2OID,-1,InvalidOid,sizeof(int16),Int16GetDatum((1u<<LP_SAID_CLAIM)|(1u<<LP_SAID_RECORD)),false,true);
                predicate=list_make1(makeFuncExpr(assertion_function,BOOLOID,
                    list_make2(makeVar(1,path,get_atttype(relation_id,path),-1,InvalidOid,0),roles),InvalidOid,InvalidOid,COERCE_EXPLICIT_CALL));
            }
        }
        Relation index=container_index(table,path,operand,nomination,predicate,&index_column,&operation,slot_id,slot_col);
        size_t window=budget;
        if(!endpoint && strategy==3 && unique>=LP_PG_GIN_RANGE_IDS){
            GinStatsData stats;ginGetStats(index,&stats);
            BlockNumber entry_pages=stats.nEntryPages>1?stats.nEntryPages:RelationGetNumberOfBlocks(index);
            if(entry_pages>0 && unique<=entry_pages/4)nomination=LP_PG_GIN_EXACT_OVERLAPS;
            else window=Max(window,unique);
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
        if(mask_keys){
            AttrNumber mask=get_attnum(relation_id,"mask");
            for(int i=0;i<index->rd_index->indnkeyatts;i++)if(index->rd_index->indkey.values[i]==mask){
                Oid op=get_opfamily_member(index->rd_opfamily[i],get_atttype(relation_id,mask),INT2ARRAYOID,2);
                if(OidIsValid(op)){ScanKeyEntryInitialize(key+nkeys++,0,i+1,2,INT2ARRAYOID,index->rd_indcollation[i],get_opcode(op),PointerGetDatum(mask_keys));break;}
            }
        }
        IndexScanDesc scan=index_beginscan_bitmap(index,snapshot,NULL,nkeys);
        StrategyNumber end_strat=endpoint==1?LP_PG_GIN_FIRST_ANY:endpoint==2?LP_PG_GIN_LAST_ANY:LP_PG_GIN_BOTH_ANY;
        StrategyNumber opp_strat=endpoint==1?LP_PG_GIN_LAST_ANY:endpoint==2?LP_PG_GIN_FIRST_ANY:LP_PG_GIN_BOTH_ANY;
        for(size_t begin=0;begin<unique;begin+=window){
            size_t amount=Min(window,unique-begin);
            ArrayType *keys=NULL;
            if(nom_buf){
                const lp_id *all=lp_idmap_keys(wanted)+begin;
                if(endpoint==3){
                    uint32 nf=0,nl=0;
                    for(size_t i=0;i<amount;i++)if(key_active(wanted,begin+i,h_kinds,nom_cap,0))nom_buf[1+nf++]=all[i];
                    for(size_t i=0;i<amount;i++)if(key_active(wanted,begin+i,h_kinds,nom_cap,1))nom_buf[1+nf+nl++]=all[i];
                    if(!(nf+nl))continue;
                    memset(nom_buf[0].b,0xFF,8);memcpy(nom_buf[0].b+8,&nf,4);memcpy(nom_buf[0].b+12,&nl,4);
                    keys=id_array_of(element,nom_buf,1+(int)(nf+nl));
                }else{
                    int dir=endpoint==2?1:0,nk=0;
                    for(size_t i=0;i<amount;i++)if(key_active(wanted,begin+i,h_kinds,nom_cap,dir))nom_buf[nk++]=all[i];
                    if(!nk)continue;
                    keys=id_array_of(element,nom_buf,nk);
                }
            }else keys=(single_keys && begin==0 && amount==unique)?single_keys:id_array_of(element,lp_idmap_keys(wanted)+begin,(int)amount);
            size_t opposite_count=indexed_opposite?lp_idmap_count(opposite):1;
            for(size_t other=0;other<opposite_count;other+=window){
                ArrayType *other_keys=NULL;
                if(indexed_opposite){
                    other_keys=id_array_of(element,lp_idmap_keys(opposite)+other,(int)Min(window,opposite_count-other));
                    ScanKeyEntryInitialize(key+opposite_key,0,index_column,opp_strat,array_type,
                        index->rd_indcollation[index_column-1],operation,PointerGetDatum(other_keys));
                }
                ScanKeyEntryInitialize(key,0,index_column,endpoint?end_strat:nomination,endpoint?array_type:operand,
                    index->rd_indcollation[index_column-1],operation,PointerGetDatum(keys));
                index_rescan(scan,key,nkeys,NULL,0);index_getbitmap(scan,bitmap);
                if(other_keys)pfree(other_keys);
            }
            if(keys!=single_keys)pfree(keys);
        }
        if(!tbm_is_empty(bitmap)){
            TupleConversionMap *conversion=lp_pg_tuple_conversion(RelationGetDescr(table),RelationGetDescr(parent));
            TupleTableSlot *projected=conversion?MakeSingleTupleTableSlot(RelationGetDescr(parent),&TTSOpsVirtual):NULL;
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
            tbm_free(bitmap);bitmap=tbm_create(bm_mem,NULL);
        }
        index_endscan(scan);index_close(index,NoLock);table_close(table,NoLock);
        if(predicate && predicate!=parent_predicate)list_free_deep(predicate);
        if(!proceed)break;
    }
    tbm_free(bitmap);if(single_keys)pfree(single_keys);if(nom_buf)pfree(nom_buf);
    if(parent_predicate)list_free_deep(parent_predicate);
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
    scan_container_rows(parent,column,wanted,strategy,NULL,false,0,NULL,bits,assertions,0,container_parent,&out);
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
        scan_container_rows(parent,column,wanted,3,NULL,false,0,NULL,0,assertions,0,direct_container_visit,&out);
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
    TierMaximum result={get_attnum(target,"tier"),0};
    PresenceKey *keys=palloc_extended((Size)count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    lp_pg_indexed_keys(parent,column,keys,(size_t)count,stored_tier,&result);
    pfree(keys);table_close(parent,NoLock);return result.maximum;
}
typedef struct {PresenceOutput output;AttrNumber path,tier,mask;int floor;lp_mask bits;MemoryContext tuple_context;} ContainerOutput;
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
    ContainerOutput *out=context;bool missing;Datum tier=slot_getattr(slot,out->tier,&missing);
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
        for(int i=0;i<count;i++){bool fresh;size_t at=lp_idmap_put(wanted,ids+i,&fresh);if(fresh)*(uint64 *)lp_idmap_at(wanted,at)=0;}
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
        uint64 h_cap=read.cap>=0?(((uint64)read.kinds<<60)|((uint64)read.cap+1)):0;
        scan_container_rows(parent,column,read.wanted,3,read.required,false,read.endpoint,NULL,read.bits,false,h_cap,holder_row,&read);
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
typedef struct {CarrierLocation carrier;Datum path;int16 tier;bool known;double rating,deviation,volatility;int64 matches;} ClaimStanding;
typedef struct {
    PresenceOutput output;lp_idmap *wanted,*parents,*required,*opposite;lp_id *refuse,*include;int nrefuse,ninclude,*links;
    bool each,done;uint64 limit,sent,saturated,epoch,nom_epoch,nom_saturated;int floor,endpoint;int64 arity;lp_mask bits;
    AttrNumber id,path,tier,mask,standing[4];MemoryContext tuple_context;
} ClaimRead;
static void claim_key_tier(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    ClaimRead *read=context;bool missing;Datum value=slot_getattr(slot,read->tier,&missing);
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
        lp_id first=lp_path_id(path.p,0),last=lp_path_id(path.p,path.p.n-1);
        if(!(((read->endpoint&1) && lp_idmap_find(read->wanted,&first)>=0 && lp_idmap_find(read->opposite,&last)>=0)
          || ((read->endpoint&2) && lp_idmap_find(read->wanted,&last)>=0 && lp_idmap_find(read->opposite,&first)>=0)))return true;
    }
    if(lp_path_middle_any(path.p,read->refuse,(size_t)read->nrefuse))return true;
    if(read->include && !lp_path_middle_any(path.p,read->include,(size_t)read->ninclude))return true;
    uint64 nom_cap=read->limit<UINT64_MAX?Max(read->limit,UINT64_C(64)):UINT64_MAX;
    size_t target_dirs=lp_idmap_count(read->wanted)*(read->endpoint==3?2:1);
    if(read->each){
        bool eligible=false;
        for(size_t i=0;i<path.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==path.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(path.p,i);int64 at=lp_idmap_find(read->wanted,&id);if(at<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)at);int dir=(read->endpoint==3 && i>0)?1:0;
            if(tier>part->floor && part->nominated[dir]<nom_cap){eligible=true;break;}
        }
        if(!eligible)return nom_cap==UINT64_MAX || read->nom_saturated<target_dirs;
    }
    value=slot_getattr(slot,read->id,&missing);
    if(missing)ereport(ERROR,(errmsg("Native claim identity is null")));
    bool fresh;size_t at=lp_idmap_put(read->parents,(const lp_id *)DatumGetPointer(value),&fresh);
    ClaimStanding *standing=lp_idmap_at(read->parents,at);
    if(fresh){
        memset(standing,0,sizeof(*standing));Size sz=VARSIZE_ANY(detoasted);
        bytea *copied=MemoryContextAlloc(read->tuple_context,sz);memcpy(copied,detoasted,sz);
        standing->path=PointerGetDatum(copied);standing->tier=(int16)tier;
    }
    standing->carrier=(CarrierLocation){slot->tts_tableOid,slot->tts_tid};
    if(nom_cap<UINT64_MAX){
        if(!read->each)return lp_idmap_count(read->parents)<nom_cap;
        if(++read->nom_epoch==0){
            for(size_t i=0;i<lp_idmap_count(read->wanted);i++){ClaimKey *k=lp_idmap_at(read->wanted,i);k->nom_epoch[0]=k->nom_epoch[1]=0;}
            read->nom_epoch=1;
        }
        for(size_t i=0;i<path.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==path.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(path.p,i);int64 ka=lp_idmap_find(read->wanted,&id);if(ka<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)ka);int dir=(read->endpoint==3 && i>0)?1:0;
            if(part->nom_epoch[dir]==read->nom_epoch || tier<=part->floor || part->nominated[dir]>=nom_cap)continue;
            part->nom_epoch[dir]=read->nom_epoch;
            if(++part->nominated[dir]>=nom_cap)read->nom_saturated++;
        }
        return read->nom_saturated<target_dirs;
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
        size_t target_dirs=lp_idmap_count(read->wanted)*(read->endpoint==3?2:1);
        if(++read->epoch==0){for(size_t i=0;i<lp_idmap_count(read->wanted);i++){ClaimKey *k=lp_idmap_at(read->wanted,i);k->epoch[0]=k->epoch[1]=0;}read->epoch=1;}
        for(size_t i=0;i<geometry.p.n;i++){
            if(read->endpoint && !((i==0 && (read->endpoint&1)) || (i==geometry.p.n-1 && (read->endpoint&2))))continue;
            lp_id id=lp_path_id(geometry.p,i);int64 at=lp_idmap_find(read->wanted,&id);if(at<0)continue;
            ClaimKey *part=lp_idmap_at(read->wanted,(size_t)at);int dir=(read->endpoint==3 && i>0)?1:0;
            if(part->epoch[dir]==read->epoch || part->sent[dir]>=read->limit || standing->tier<=part->floor)continue;
            part->epoch[dir]=read->epoch;
            if(part->epoch[1-dir]!=read->epoch){
                for(int input=part->head;input>=0;input=read->links[input]){
                    values[0]=Int64GetDatum((int64)input+1);set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values,nulls);
                }
            }
            if(++part->sent[dir]==read->limit)read->saturated++;
        }
        read->done=read->saturated==target_dirs;
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
        uint64 nom_cap=(read.each && read.limit<UINT64_MAX)?Max(read.limit,UINT64_C(64)):0;
        scan_container_rows(paths,column,read.wanted,each?3:7,read.required,true,read.endpoint,read.opposite,read.bits,false,nom_cap,claim_nominate,&read);
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

/* laplace_held_files(keys, trunk, cap, max_nodes): up the container index from keys to the files that hold them, in
 * one call. A node holds the keys below it (bit k: keys[k], k < 64), propagated up as they arrive, through every path
 * that holds a node as content; never through the OS record of a file, and a holder whose first vertex is that record
 * is a file, where the walk stops. Level zero is the capped GIN nomination of laplace_holders: a key held directly by
 * more than cap paths of a kind is a hub, its bit is set in hubs, and it is not walked. Every later level is one
 * direct-container frontier. trunk, when given, keeps the files under it (read down its spine by ID). Each file comes
 * with its unit, the lowest node of its content that holds every key the file holds (their common ancestor, never a key
 * itself), that node's path length (its constituents, runs counted) and tier (its height: a file's path is only its
 * record and its content, so the tier is what tells a whole file from a line), and how many times each key stands
 * below it (the walked DAG's multiplicity). One row a file; a walk that reaches none returns one row with a null file, so hubs, levels
 * and cut are always answered. */
typedef struct { uint64 keys; bool file; } HeldNode;
typedef struct { uint64 epoch,held[2]; } HeldKey;
typedef struct { lp_id holder,key; Datum path; } HeldFirst;
typedef struct { uint64 times,visit; } HeldEdgeTimes;
typedef struct {
    lp_idmap *nodes,*front,*next,*keys;lp_strmap *edges;AttrNumber id,path,mask;int64 cap;uint64 epoch,saturated,visit;
    lp_vec(HeldFirst) first;lp_vec(lp_vertex) vertices;size_t max_nodes;bool cut;MemoryContext owner;
} HeldWalk;
static HeldNode *held_node(HeldWalk *walk,const lp_id *id,bool *fresh)
{
    HeldNode *node=lp_idmap_get(walk->nodes,id,fresh);
    if(*fresh)*node=(HeldNode){0};
    return node;
}
/* One holder of the front: it takes the keys of every front node it holds as content, and each such child is an edge
 * of the walk with the times the holder says it (its runs, summed over the vertices that name it). */
static void held_holder(HeldWalk *walk,const lp_id *holder,Datum value)
{
    Geo geometry=geo_of(value);size_t count=geometry.p.n;if(!count)return;
    lp_vec_reserve(&walk->vertices,count);lp_vertex *v=walk->vertices.v;
    if(lp_path_decode(geometry.p,v,count)!=count)
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_held_files: invalid stored path")));
    bool file=v[0].said==LP_SAID_METADATA,held=false;uint64 gained=0;
    for(size_t k=0;k<count;k++){
        if(v[k].said==LP_SAID_METADATA)continue;
        if(lp_idmap_find(walk->front,&v[k].id)<0)continue;
        HeldNode *child=lp_idmap_lookup(walk->nodes,&v[k].id);
        if(child){gained|=child->keys;held=true;}
    }
    if(!held)return;
    if(lp_idmap_find(walk->nodes,holder)<0 && lp_idmap_count(walk->nodes)>=walk->max_nodes){walk->cut=true;return;}
    bool fresh;HeldNode *node=held_node(walk,holder,&fresh);
    uint64 before=node->keys;node->keys|=gained;node->file|=file;
    uint32 parent=(uint32)lp_idmap_find(walk->nodes,holder);walk->visit++;
    for(size_t k=0;k<count;k++){
        if(v[k].said==LP_SAID_METADATA || lp_idmap_find(walk->front,&v[k].id)<0)continue;
        int64 child=lp_idmap_find(walk->nodes,&v[k].id);if(child<0)continue;
        uint32 pair[2]={parent,(uint32)child};bool added;
        HeldEdgeTimes *edge=lp_strmap_get(walk->edges,pair,sizeof pair,&added);
        if(added || edge->visit!=walk->visit){edge->times=0;edge->visit=walk->visit;}
        edge->times+=v[k].run?v[k].run:1;
    }
    if(!file && (fresh || node->keys!=before))lp_idmap_put(walk->next,holder,NULL);
}
static void held_level(const lp_id *id,Datum path,Datum mask,void *context){(void)mask;held_holder(context,id,path);}
/* Level zero: each key with its direct holders through the capped nomination; a hub is dropped once the scan ends. */
static bool held_first(TupleTableSlot *slot,void *context)
{
    HeldWalk *walk=context;bool missing;
    Datum mask=slot_getattr(slot,walk->mask,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder mask is null")));
    int kind=(lp_pg_mask_native(mask)&UINT64_C(1))?1:0;
    Datum id=slot_getattr(slot,walk->id,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder ID is null")));
    Datum path=slot_getattr(slot,walk->path,&missing);
    if(missing)ereport(ERROR,(errmsg("Native holder path is null")));
    Geo geometry=geo_of(path);
    if(++walk->epoch==0){
        for(size_t i=0;i<lp_idmap_count(walk->keys);i++)((HeldKey *)lp_idmap_at(walk->keys,i))->epoch=0;
        walk->epoch=1;
    }
    for(size_t i=0;i<geometry.p.n;i++){
        lp_id child=lp_path_id(geometry.p,i);int64 at=lp_idmap_find(walk->keys,&child);if(at<0)continue;
        HeldKey *key=lp_idmap_at(walk->keys,(size_t)at);
        if(key->epoch==walk->epoch)continue;key->epoch=walk->epoch;
        if(walk->cap>=0){
            if(key->held[kind]>(uint64)walk->cap)continue;
            if(++key->held[kind]>(uint64)walk->cap){walk->saturated++;continue;}
        }
        MemoryContext previous=MemoryContextSwitchTo(walk->owner);
        HeldFirst first={*(const lp_id *)DatumGetPointer(id),child,PointerGetDatum(PG_DETOAST_DATUM_COPY(path))};
        lp_push(&walk->first,first);MemoryContextSwitchTo(previous);
    }
    return walk->cap<0 || walk->saturated<2*lp_idmap_count(walk->keys);
}
/* The files under a trunk: down its spine by ID, never through a record (the trunk has its source record first as a
 * file has its OS record), to the nodes whose first vertex is such a record. The trunk itself is always read down. */
typedef struct { lp_idmap *files,*next;const lp_id *trunk; } HeldScope;
static void held_scope_row(const lp_id *id,Datum value,Datum mask,void *context)
{
    (void)mask;HeldScope *scope=context;Geo geometry=geo_of(value);
    size_t count=geometry.p.n;if(!count)return;
    lp_vertex *v=palloc(sizeof(lp_vertex)*count);
    if(lp_path_decode(geometry.p,v,count)!=count)
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_held_files: invalid stored path")));
    if(v[0].said==LP_SAID_METADATA && memcmp(id->b,scope->trunk->b,16))lp_idmap_put(scope->files,id,NULL);
    else for(size_t i=0;i<count;i++)if(v[i].said!=LP_SAID_METADATA)lp_idmap_put(scope->next,&v[i].id,NULL);
    pfree(v);
}
/* The walk read down: each node's children among the walked nodes, with the times each is said. */
typedef struct { uint32 *head,*next,*child;uint64 *times;size_t keys,all;lp_idmap *memo; } HeldDown;
/* How many times each key stands below a node, by the multiplicity of the walked DAG (saturating). A key is a leaf. */
static const uint64 *held_counts(HeldWalk *walk,HeldDown *down,uint32 node,int depth)
{
    const lp_id *id=lp_idmap_key(walk->nodes,node);
    uint64 *memo=lp_idmap_lookup(down->memo,id);if(memo)return memo;
    uint64 *counts=palloc0(sizeof(uint64)*down->keys);
    if(node<down->keys)counts[node]=1;
    else if(depth<256)for(uint32 e=down->head[node];e!=UINT32_MAX;e=down->next[e]){
        const uint64 *below=held_counts(walk,down,down->child[e],depth+1);
        for(size_t k=0;k<down->keys;k++){
            uint64 add=below[k]*down->times[e];
            if(below[k] && (add/below[k]!=down->times[e] || counts[k]+add<counts[k]))counts[k]=UINT64_MAX;
            else counts[k]+=add;
        }
    }
    bool fresh;memcpy(lp_idmap_get(down->memo,id,&fresh),counts,sizeof(uint64)*down->keys);pfree(counts);
    return lp_idmap_lookup(down->memo,id);
}
/* A file's unit: down from it, while a walked child holds every key the file holds and is no key itself, to the lowest
 * such node, the keys' lowest common ancestor in its content. */
static uint32 held_unit(HeldWalk *walk,HeldDown *down,uint32 file)
{
    uint64 want=((const HeldNode *)lp_idmap_at(walk->nodes,file))->keys;uint32 at=file;
    for(int depth=0;depth<256;depth++){
        uint32 next=UINT32_MAX;
        for(uint32 e=down->head[at];e!=UINT32_MAX;e=down->next[e]){
            uint32 child=down->child[e];
            if(child>=down->all && ((const HeldNode *)lp_idmap_at(walk->nodes,child))->keys==want){next=child;break;}
        }
        if(next==UINT32_MAX)break;
        at=next;
    }
    return at;
}
typedef struct { lp_idmap *length; } HeldLength;
typedef struct { AttrNumber tier;int16 *tiers; } HeldTiers;
static void held_tier_row(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    HeldTiers *out=context;bool missing;Datum tier=slot_getattr(slot,out->tier,&missing);
    if(missing)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native entity tier is null")));
    out->tiers[key->ordinal-1]=DatumGetInt16(tier);
}
static void held_length_row(const lp_id *id,Datum value,Datum mask,void *context)
{
    (void)mask;HeldLength *read=context;Geo geometry=geo_of(value);bool fresh;
    *(int64 *)lp_idmap_get(read->length,id,&fresh)=(int64)lp_path_len(geometry.p);
}
PG_FUNCTION_INFO_V1(laplace_held_files);
Datum laplace_held_files(PG_FUNCTION_ARGS)
{
    if(PG_ARGISNULL(0))PG_RETURN_NULL();
    TupleDesc descriptor;Tuplestorestate *output=set_begin(fcinfo,"laplace_held_files",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    int64 cap=PG_ARGISNULL(2)?-1:PG_GETARG_INT64(2),most=PG_ARGISNULL(3)?400000:PG_GETARG_INT64(3);
    if(cap<-1 || most<1)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_held_files: cap is -1 or more, max_nodes 1 or more")));
    for(int i=0;i<count;i++)for(int j=0;j<i;j++)if(!memcmp(ids[i].b,ids[j].b,16))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_held_files: keys are distinct")));
    HeldWalk walk={.nodes=lp_idmap_sized(sizeof(HeldNode)),.front=lp_idmap_new(),.next=lp_idmap_new(),
        .keys=lp_idmap_sized(sizeof(HeldKey)),.edges=lp_strmap_sized(sizeof(HeldEdgeTimes)),.cap=cap,.max_nodes=(size_t)most,
        .owner=CurrentMemoryContext};
    lp_idmap *scope=NULL;uint64 hubs=0;int levels=0;
    HeldDown down={.keys=(size_t)Min(count,64),.all=(size_t)Max(count,0),.memo=lp_idmap_sized(sizeof(uint64)*(size_t)Max(Min(count,64),1))};
    HeldLength lengths={lp_idmap_sized(sizeof(int64))};
    PG_TRY();{
        for(int i=0;i<count;i++){
            bool fresh;HeldKey *key=lp_idmap_get(walk.keys,ids+i,&fresh);if(fresh)*key=(HeldKey){0};
            HeldNode *node=held_node(&walk,ids+i,&fresh);if(i<64)node->keys|=UINT64_C(1)<<i;
        }
        if(count){
            const char *selected[]={"entity","path","tier","mask"},*column;Oid target=lp_pg_relation("physicality");
            Relation parent=lp_pg_read_parent(target,&column,selected,lengthof(selected));
            walk.id=get_attnum(target,"entity");walk.path=get_attnum(target,"path");walk.mask=get_attnum(target,"mask");
            uint64 h_cap=cap>=0?((UINT64_C(3)<<60)|((uint64)cap+1)):0;
            scan_container_rows(parent,column,walk.keys,3,NULL,false,0,NULL,0,false,h_cap,held_first,&walk);
            table_close(parent,NoLock);
            for(int i=0;i<count && i<64;i++){
                HeldKey *key=lp_idmap_lookup(walk.keys,ids+i);
                if(cap>=0 && (key->held[0]>(uint64)cap || key->held[1]>(uint64)cap))hubs|=UINT64_C(1)<<i;
            }
            /* level zero: each holder takes the keys it holds that are not hubs */
            for(size_t i=0;i<walk.first.n;i++){
                const HeldFirst *first=walk.first.v+i;
                HeldKey *key=lp_idmap_lookup(walk.keys,&first->key);
                if(cap>=0 && (key->held[0]>(uint64)cap || key->held[1]>(uint64)cap))continue;
                lp_idmap_clear(walk.front);lp_idmap_put(walk.front,&first->key,NULL);
                held_holder(&walk,&first->holder,first->path);
            }
            levels=1;
            while(lp_idmap_count(walk.next) && levels<128 && !walk.cut){
                CHECK_FOR_INTERRUPTS();
                lp_idmap_free(walk.front);walk.front=walk.next;walk.next=lp_idmap_new();
                lp_pg_lookup_containers(lp_idmap_keys(walk.front),lp_idmap_count(walk.front),false,false,held_level,&walk);
                levels++;
            }
        }
        if(!PG_ARGISNULL(1)){
            const lp_id *trunk=(const lp_id *)DatumGetPointer(PG_GETARG_DATUM(1));
            HeldScope scan={lp_idmap_new(),lp_idmap_new(),trunk};
            lp_idmap *front=lp_idmap_new();lp_idmap_put(front,trunk,NULL);
            for(int depth=0;depth<64 && lp_idmap_count(front);depth++){
                lp_pg_lookup_paths(lp_idmap_keys(front),lp_idmap_count(front),false,held_scope_row,&scan);
                lp_idmap_free(front);front=scan.next;scan.next=lp_idmap_new();
            }
            lp_idmap_free(front);lp_idmap_free(scan.next);scope=scan.files;
        }
        /* the walk read down: children by parent, with their times */
        size_t nodes=lp_idmap_count(walk.nodes),edges=lp_strmap_count(walk.edges);
        down.head=palloc_extended(sizeof(uint32)*Max(nodes,1),MCXT_ALLOC_HUGE);
        down.next=palloc_extended(sizeof(uint32)*Max(edges,1),MCXT_ALLOC_HUGE);
        down.child=palloc_extended(sizeof(uint32)*Max(edges,1),MCXT_ALLOC_HUGE);
        down.times=palloc_extended(sizeof(uint64)*Max(edges,1),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<nodes;i++)down.head[i]=UINT32_MAX;
        for(size_t e=0;e<edges;e++){
            size_t length;const uint32 *pair=(const uint32 *)lp_strmap_key(walk.edges,e,&length);
            down.child[e]=pair[1];down.times[e]=((const HeldEdgeTimes *)lp_strmap_at(walk.edges,e))->times;
            down.next[e]=down.head[pair[0]];down.head[pair[0]]=(uint32)e;
        }
        /* each file of the scope that holds a key, and its unit; the units' lengths in one read by ID */
        lp_vec(uint32) files={0},units={0};lp_vec(lp_id) unit_ids={0};
        for(size_t i=0;i<nodes;i++){
            const HeldNode *node=lp_idmap_at(walk.nodes,i);const lp_id *id=lp_idmap_key(walk.nodes,i);
            if(!node->file || !node->keys || (scope && lp_idmap_find(scope,id)<0))continue;
            uint32 unit=held_unit(&walk,&down,(uint32)i);
            lp_push(&files,(uint32)i);lp_push(&units,unit);lp_push(&unit_ids,*lp_idmap_key(walk.nodes,unit));
        }
        int16 *tiers=palloc0(sizeof(int16)*Max(unit_ids.n,1));
        if(unit_ids.n){
            lp_pg_lookup_paths(unit_ids.v,unit_ids.n,false,held_length_row,&lengths);
            const char *selected[]={"id","tier"},*column;Oid target=lp_pg_relation("entity");
            Relation parent=lp_pg_read_parent(target,&column,selected,lengthof(selected));
            HeldTiers read={get_attnum(target,"tier"),tiers};
            PresenceKey *keys=palloc_extended(unit_ids.n*sizeof(*keys),MCXT_ALLOC_HUGE);
            for(size_t i=0;i<unit_ids.n;i++)keys[i]=(PresenceKey){.id=unit_ids.v[i],.ordinal=(int64)i+1};
            lp_pg_indexed_keys(parent,column,keys,unit_ids.n,held_tier_row,&read);
            pfree(keys);table_close(parent,NoLock);
        }
        for(size_t f=0;f<files.n;f++){
            CHECK_FOR_INTERRUPTS();
            const HeldNode *node=lp_idmap_at(walk.nodes,files.v[f]);
            const uint64 *counts=held_counts(&walk,&down,units.v[f],0);
            Datum *cells=palloc(sizeof(Datum)*Max(down.keys,1));
            for(size_t k=0;k<down.keys;k++)cells[k]=Int64GetDatum(counts[k]>(uint64)PG_INT64_MAX?PG_INT64_MAX:(int64)counts[k]);
            ArrayType *array=construct_array(cells,(int)down.keys,INT8OID,8,FLOAT8PASSBYVAL,TYPALIGN_DOUBLE);
            int64 *length=lp_idmap_lookup(lengths.length,&unit_ids.v[f]);
            Datum values[]={id_datum(lp_idmap_key(walk.nodes,files.v[f])),Int64GetDatum((int64)node->keys),Int64GetDatum((int64)hubs),
                Int32GetDatum(levels),BoolGetDatum(walk.cut),id_datum(&unit_ids.v[f]),Int64GetDatum(length?*length:0),PointerGetDatum(array),
                Int16GetDatum(tiers[f])};
            bool nulls[lengthof(values)]={false};set_put(fcinfo,output,descriptor,values,nulls);
            pfree(cells);
        }
        if(!files.n){
            Datum values[]={(Datum)0,Int64GetDatum(0),Int64GetDatum((int64)hubs),Int32GetDatum(levels),BoolGetDatum(walk.cut),(Datum)0,(Datum)0,(Datum)0,(Datum)0};
            bool nulls[lengthof(values)]={true,false,false,false,false,true,true,true,true};set_put(fcinfo,output,descriptor,values,nulls);
        }
        lp_vec_free(&files);lp_vec_free(&units);lp_vec_free(&unit_ids);
    }PG_FINALLY();{
        lp_idmap_free(walk.nodes);lp_idmap_free(walk.front);lp_idmap_free(walk.next);lp_idmap_free(walk.keys);
        lp_strmap_free(walk.edges);lp_idmap_free(down.memo);lp_idmap_free(lengths.length);
        lp_vec_free(&walk.vertices);lp_vec_free(&walk.first);if(scope)lp_idmap_free(scope);
    }PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}
