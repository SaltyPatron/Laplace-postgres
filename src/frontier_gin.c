/* Shared GIN containment scanner and container/holder/claim frontier queries
 * (lp_pg_lookup_containers, laplace_containers, laplace_holders, laplace_claims). */
#include "pg_core.h"
#include "geo.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/gin.h"
#include "access/gin_private.h"
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
#include "utils/syscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "partitioning/partdesc.h"
#include <math.h>
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
                                 lp_idmap *required,bool required_any,int endpoint,lp_idmap *opposite,lp_mask bits,bool assertions,uint64 nom_cap,int16 min_floor,ContainerVisit visit,void *context)
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
    bool prune_tier=(min_floor>=0 && partdesc!=NULL && RelationGetNamespace(parent)==lp_pg_schema() &&
                     strcmp(RelationGetRelationName(parent),"physicality")==0);
    const Oid *canon_parts=NULL;const int16 *min_t=NULL,*max_t=NULL;int n_canon=0;
    if(prune_tier){
        n_canon=lp_pg_canonical_partitions("physicality",&canon_parts,NULL,NULL,NULL);
        (void)lp_pg_canonical_tier_bounds("physicality",&min_t,&max_t);
        if(!min_t || !max_t || n_canon!=nparts)prune_tier=false;
    }
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
        if(prune_tier && canon_parts[p_idx]==relation_id && max_t[p_idx]<=min_floor)continue;
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
static void scan_containers(Relation parent,const char *column,lp_idmap *wanted,lp_idmap *parents,StrategyNumber strategy,lp_mask bits,bool assertions,int16 min_floor)
{
    ContainerParents out={get_attnum(RelationGetRelid(parent),"entity"),parents};
    scan_container_rows(parent,column,wanted,strategy,NULL,false,0,NULL,bits,assertions,0,min_floor,container_parent,&out);
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
        scan_container_rows(parent,column,wanted,3,NULL,false,0,NULL,0,assertions,0,-1,direct_container_visit,&out);
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
        scan_containers(parent,column,wanted,parents,7,out.bits,false,(int16)out.floor);
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
    AttrNumber id,path,tier,mask;int floor,kinds,endpoint;bool per_part,count_only;int64 cap;lp_mask bits;
    uint64 epoch,required_epoch,saturated;Tuplestorestate *pending;MemoryContext owner;
    void (*emit_hit)(const PgHolderHit *hit,Datum mask,void *ctx);
    void *emit_ctx;
} HolderRead;
static void holder_emit(HolderRead *read,size_t key,Datum *values,bool *nulls)
{
    if(read->emit_hit){
        PgHolderHit hit={
            .ordinal=0,
            .entity=nulls[1]?(lp_id){{0}}:*(const lp_id *)DatumGetPointer(values[1]),
            .path=nulls[2]?(Datum)0:values[2],
            .tier=nulls[3]?-1:DatumGetInt16(values[3]),
            .kind=DatumGetInt16(values[5]),
            .hub=DatumGetBool(values[6])
        };
        Datum mask=nulls[4]?(Datum)0:values[4];
        if(!read->per_part){read->emit_hit(&hit,mask,read->emit_ctx);return;}
        const HolderKey *part=lp_idmap_at(read->wanted,key);
        for(int at=part->head;at>=0;at=read->links[at]){
            hit.ordinal=(int64)at+1;
            read->emit_hit(&hit,mask,read->emit_ctx);
        }
        return;
    }
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
    if(!read->per_part && !read->count_only){holder_emit(read,0,values,nulls);return true;}
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
        if(read->count_only){key->held[kind]++;continue;}
        if(read->cap<0){holder_emit(read,(size_t)at,values,nulls);continue;}
        if(key->held[kind]>(uint64)read->cap)continue;
        if(++key->held[kind]>(uint64)read->cap){read->saturated++;continue;}
        values[0]=Int64GetDatum(at+1);
        MemoryContext previous=MemoryContextSwitchTo(read->owner);
        tuplestore_putvalues(read->pending,read->output.descriptor,values,nulls);
        MemoryContextSwitchTo(previous);
    }
    if(read->count_only)return true;
    size_t groups=lp_idmap_count(read->wanted)*((read->kinds&1?1:0)+(read->kinds&2?1:0));
    return read->cap<0 || read->saturated<groups;
}
static void run_holders_query(HolderRead *read,const lp_id *ids,int count,const lp_id *required,int nrequired)
{
    if(!count)return;
    const char *selected[]={"entity","path","tier","mask"},*column;Oid target=lp_pg_relation("physicality");
    Relation parent=lp_pg_read_parent(target,&column,selected,lengthof(selected));
    read->id=get_attnum(target,"entity");read->path=get_attnum(target,"path");
    read->tier=get_attnum(target,"tier");read->mask=get_attnum(target,"mask");
    read->links=palloc_extended((Size)count*sizeof(*read->links),MCXT_ALLOC_HUGE);
    read->wanted=lp_idmap_sized(sizeof(HolderKey));read->required=lp_idmap_sized(sizeof(uint64));read->seen=lp_idmap_new();
    TupleDesc owned_desc=NULL;
    if(!read->count_only && read->cap>=0){
        if(!read->output.descriptor){
            owned_desc=CreateTemplateTupleDesc(7);
            TupleDescInitEntry(owned_desc,1,"ordinal",INT8OID,-1,0);
            TupleDescInitEntry(owned_desc,2,"entity",get_atttype(target,read->id),-1,0);
            TupleDescInitEntry(owned_desc,3,"path",get_atttype(target,read->path),-1,0);
            TupleDescInitEntry(owned_desc,4,"tier",INT2OID,-1,0);
            TupleDescInitEntry(owned_desc,5,"mask",get_atttype(target,read->mask),-1,0);
            TupleDescInitEntry(owned_desc,6,"kind",INT2OID,-1,0);
            TupleDescInitEntry(owned_desc,7,"hub",BOOLOID,-1,0);
            read->output.descriptor=owned_desc;
        }
        read->pending=tuplestore_begin_heap(true,false,work_mem);
    }
    PG_TRY();{
        for(int i=count-1;i>=0;i--){
            bool fresh;size_t at=lp_idmap_put(read->wanted,ids+i,&fresh);HolderKey *key=lp_idmap_at(read->wanted,at);
            if(fresh){memset(key,0,sizeof(*key));key->head=-1;}
            read->links[i]=key->head;key->head=i;
        }
        for(int i=0;i<nrequired;i++){bool fresh;size_t at=lp_idmap_put(read->required,required+i,&fresh);if(fresh)*(uint64 *)lp_idmap_at(read->required,at)=0;}
        uint64 h_cap=(!read->count_only && read->cap>=0)?(((uint64)read->kinds<<60)|((uint64)read->cap+1)):0;
        scan_container_rows(parent,column,read->wanted,3,read->required,false,read->endpoint,NULL,read->bits,false,h_cap,(int16)read->floor,holder_row,read);
        if(read->count_only && !read->emit_hit){
            for(int i=0;i<count;i++){
                int64 at=lp_idmap_find(read->wanted,ids+i);
                const HolderKey *key=lp_idmap_at(read->wanted,(size_t)at);
                Datum idd=id_datum(ids+i);
                Datum v[4]={Int64GetDatum((int64)i+1),idd,Int64GetDatum((int64)key->held[0]),Int64GetDatum((int64)key->held[1])};
                bool n[4]={false,false,false,false};
                set_put(read->output.fcinfo,read->output.output,read->output.descriptor,v,n);
                pfree(DatumGetPointer(idd));
            }
        }
        if(read->pending){
            TupleTableSlot *slot=MakeSingleTupleTableSlot(read->output.descriptor,&TTSOpsMinimalTuple);
            tuplestore_rescan(read->pending);
            while(tuplestore_gettupleslot(read->pending,true,false,slot)){
                CHECK_FOR_INTERRUPTS();slot_getallattrs(slot);
                size_t at=(size_t)DatumGetInt64(slot->tts_values[0])-1;int kind=DatumGetInt16(slot->tts_values[5]);
                if(((HolderKey *)lp_idmap_at(read->wanted,at))->held[kind]<=(uint64)read->cap)
                    holder_emit(read,at,slot->tts_values,slot->tts_isnull);
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);tuplestore_end(read->pending);read->pending=NULL;
            for(size_t at=0;at<lp_idmap_count(read->wanted);at++)for(int kind=0;kind<2;kind++){
                if(((HolderKey *)lp_idmap_at(read->wanted,at))->held[kind]<=(uint64)read->cap)continue;
                Datum values[]={0,0,0,0,0,Int16GetDatum((int16)kind),BoolGetDatum(true)};
                bool nulls[]={false,true,true,true,true,false,false};holder_emit(read,at,values,nulls);
            }
        }
    }PG_FINALLY();{
        if(read->pending){tuplestore_end(read->pending);read->pending=NULL;}
        if(owned_desc)FreeTupleDesc(owned_desc);
        lp_idmap_free(read->wanted);lp_idmap_free(read->required);lp_idmap_free(read->seen);
    }PG_END_TRY();
    pfree(read->links);table_close(parent,NoLock);
}
static void direct_holder_visit(const PgHolderHit *hit,Datum mask,void *ctx)
{
    typedef struct { lp_pg_holder_visit visit; void *context; } DirectHolderCtx;
    DirectHolderCtx *d=ctx; (void)mask;
    d->visit(hit,d->context);
}
void lp_pg_holders_direct(const lp_id *ids,int count,const lp_id *required,int nrequired,
                          bool per_part,int16 floor,int64 cap,int16 kinds,
                          lp_mask bits,int16 endpoint,lp_pg_holder_visit visit,void *context)
{
    if(count<=0||!visit)return;
    if(endpoint<0||endpoint>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("lp_pg_holders_direct: invalid endpoint %d",endpoint)));
    if(kinds==1)bits|=UINT64_C(1);
    if(cap<-1||(!per_part&&cap>=0)||kinds<1||kinds>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("lp_pg_holders_direct: invalid cap or kinds")));
    struct { lp_pg_holder_visit visit; void *context; } cb={visit,context};
    HolderRead read={.owner=CurrentMemoryContext,.per_part=per_part,.floor=floor,.cap=cap,
        .kinds=kinds,.bits=bits,.endpoint=endpoint,.emit_hit=direct_holder_visit,.emit_ctx=&cb};
    run_holders_query(&read,ids,count,required,nrequired);
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
    run_holders_query(&read,ids,count,required,nrequired);
    pfree(ids);pfree(required);PG_RETURN_NULL();
}
PG_FUNCTION_INFO_V1(laplace_holder_counts);
Datum laplace_holder_counts(PG_FUNCTION_ARGS)
{
    TupleDesc td;Tuplestorestate *out=set_begin(fcinfo,"laplace_holder_counts",&td);
    if(PG_ARGISNULL(0))return (Datum)0;
    HolderRead read={.output={fcinfo,out,td},.owner=CurrentMemoryContext,.per_part=true,.count_only=true,
        .floor=PG_ARGISNULL(1)?-1:PG_GETARG_INT16(1),.cap=-1,.kinds=PG_ARGISNULL(2)?3:PG_GETARG_INT16(2)};
    if(read.kinds<1||read.kinds>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holder_counts: kinds must select 1, 2, or 3")));
    if(PG_NARGS()>3 && !PG_ARGISNULL(3) && !read_mask_bits(PG_GETARG_ARRAYTYPE_P(3),&read.bits))return (Datum)0;
    if(PG_NARGS()>4 && !PG_ARGISNULL(4)){
        read.endpoint=PG_GETARG_INT16(4);
        if(read.endpoint<0||read.endpoint>3)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holder_counts: invalid endpoint")));
    }
    if(read.kinds==1)read.bits|=UINT64_C(1);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(count>0)run_holders_query(&read,ids,count,NULL,0);
    pfree(ids);
    return (Datum)0;
}

typedef struct {
    PgHolderHit hit;
    bytea *mask_copy;
    bytea *path_copy;
    bool known;
    double rating,deviation,volatility;
    int64 matches;
} HolderStandingRow;
typedef struct {
    lp_vec(HolderStandingRow) rows;
    lp_idmap *by_entity;
    int *links;
    int nlinks;
    MemoryContext ctx;
} HolderStandingCollect;
static void collect_holder_row(const PgHolderHit *hit,Datum mask,void *ctx)
{
    HolderStandingCollect *c=ctx;
    MemoryContext old=MemoryContextSwitchTo(c->ctx);
    HolderStandingRow r={.hit=*hit};
    if(!hit->hub){
        bytea *p=(bytea *)PG_DETOAST_DATUM(hit->path);
        Size ps=VARSIZE_ANY(p);
        r.path_copy=palloc(ps);memcpy(r.path_copy,p,ps);
        r.hit.path=PointerGetDatum(r.path_copy);
        if(mask){
            bytea *m=(bytea *)PG_DETOAST_DATUM(mask);
            Size ms=VARSIZE_ANY(m);
            r.mask_copy=palloc(ms);memcpy(r.mask_copy,m,ms);
        }
    }
    lp_push(&c->rows,r);
    MemoryContextSwitchTo(old);
}
static void collect_holder_standing(TupleTableSlot *slot,const PresenceKey *key,void *ctx)
{
    typedef struct { HolderStandingCollect *c; AttrNumber st[4]; } StCtx;
    StCtx *sc=ctx;
    int idx=(int)*(int64 *)lp_idmap_at(sc->c->by_entity,(size_t)(key->ordinal-1));
    bool n0=false,n1=false,n2=false,n3=false;
    double rt=DatumGetFloat8(slot_getattr(slot,sc->st[0],&n0));
    double dv=DatumGetFloat8(slot_getattr(slot,sc->st[1],&n1));
    double vl=DatumGetFloat8(slot_getattr(slot,sc->st[2],&n2));
    int64 mt=DatumGetInt64(slot_getattr(slot,sc->st[3],&n3));
    if(n0||n1||n2||n3)return;
    for(int r=idx;r>=0;r=sc->c->links[r]){
        HolderStandingRow *row=&sc->c->rows.v[r];
        row->known=true;row->rating=rt;row->deviation=dv;row->volatility=vl;row->matches=mt;
    }
}
PG_FUNCTION_INFO_V1(laplace_holders_with_standings);
Datum laplace_holders_with_standings(PG_FUNCTION_ARGS)
{
    TupleDesc td;Tuplestorestate *out=set_begin(fcinfo,"laplace_holders_with_standings",&td);
    if(PG_ARGISNULL(0)||PG_ARGISNULL(1)||PG_ARGISNULL(2)||PG_ARGISNULL(3)||PG_ARGISNULL(4)||PG_ARGISNULL(5))
        return (Datum)0;
    bool has_scope=PG_NARGS()>8&&!PG_ARGISNULL(8);
    lp_id *witnesses=NULL;int nwitnesses=0;
    if(has_scope){
        witnesses=ids_of(PG_GETARG_ARRAYTYPE_P(8),&nwitnesses);
        if(!nwitnesses){pfree(witnesses);return (Datum)0;}
    }
    HolderStandingCollect col={.by_entity=lp_idmap_sized(sizeof(int64)),
        .ctx=AllocSetContextCreate(CurrentMemoryContext,"HolderStandingCollect",ALLOCSET_DEFAULT_SIZES)};
    HolderRead read={.owner=CurrentMemoryContext,.per_part=PG_GETARG_BOOL(2),.floor=PG_GETARG_INT16(3),
        .cap=PG_GETARG_INT64(4),.kinds=PG_GETARG_INT16(5),.emit_hit=collect_holder_row,.emit_ctx=&col};
    if(PG_NARGS()>6&&!PG_ARGISNULL(6)&&!read_mask_bits(PG_GETARG_ARRAYTYPE_P(6),&read.bits)){
        if(witnesses)pfree(witnesses);lp_idmap_free(col.by_entity);MemoryContextDelete(col.ctx);return (Datum)0;
    }
    if(PG_NARGS()>7&&!PG_ARGISNULL(7)){
        read.endpoint=PG_GETARG_INT16(7);
        if(read.endpoint<0||read.endpoint>3)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holders_with_standings: invalid endpoint")));
    }
    if(read.kinds==1)read.bits|=UINT64_C(1);
    if(read.cap<-1||(!read.per_part&&read.cap>=0)||read.kinds<1||read.kinds>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_holders_with_standings: invalid cap or kinds")));
    int count,nrequired;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count),*required=ids_of(PG_GETARG_ARRAYTYPE_P(1),&nrequired);
    if(count>0){
        run_holders_query(&read,ids,count,required,nrequired);
        if(has_scope&&col.rows.n>0){
            lp_id *cands=palloc(col.rows.n*sizeof *cands);
            size_t *cand_row=palloc(col.rows.n*sizeof *cand_row);
            size_t ncand=0;
            for(size_t i=0;i<col.rows.n;i++){
                if(!col.rows.v[i].hit.hub){
                    cands[ncand]=col.rows.v[i].hit.entity;
                    cand_row[ncand]=i;
                    ncand++;
                }
            }
            if(ncand>0){
                bool *keep_row=palloc0(col.rows.n*sizeof *keep_row);
                for(size_t i=0;i<col.rows.n;i++)if(col.rows.v[i].hit.hub)keep_row[i]=true;
                bool *allowed=palloc(ncand*sizeof *allowed);
                lp_pg_filter_witness_entities(cands,ncand,witnesses,nwitnesses,allowed);
                for(size_t k=0;k<ncand;k++)if(allowed[k])keep_row[cand_row[k]]=true;
                size_t kept=0;
                for(size_t i=0;i<col.rows.n;i++)if(keep_row[i])col.rows.v[kept++]=col.rows.v[i];
                col.rows.n=kept;
                pfree(allowed);pfree(keep_row);
            }
            pfree(cand_row);pfree(cands);
        }
        if(col.rows.n>0){
            col.links=palloc((Size)col.rows.n*sizeof(int));
            for(size_t i=0;i<col.rows.n;i++){
                if(col.rows.v[i].hit.hub){col.links[i]=-1;continue;}
                bool fresh;size_t at=lp_idmap_put(col.by_entity,&col.rows.v[i].hit.entity,&fresh);
                int64 *head=lp_idmap_at(col.by_entity,at);
                col.links[i]=fresh?-1:(int)*head;
                *head=(int64)i;
            }
            size_t nu=lp_idmap_count(col.by_entity);
            if(nu>0){
                if(has_scope){
                    lp_idmap *solved=lp_idmap_sized(sizeof(lp_standing));
                    Oid idarr=get_fn_expr_argtype(fcinfo->flinfo,0);
                    PG_TRY();{
                        lp_pg_solve_scoped_standings(lp_idmap_keys(col.by_entity),(int)nu,idarr,witnesses,nwitnesses,solved);
                        for(size_t i=0;i<nu;i++){
                            const lp_standing *st=lp_idmap_lookup(solved,lp_idmap_key(col.by_entity,i));
                            if(!st)continue;
                            int idx=(int)*(int64 *)lp_idmap_at(col.by_entity,i);
                            for(int r=idx;r>=0;r=col.links[r]){
                                HolderStandingRow *row=&col.rows.v[r];
                                row->known=true;row->rating=st->rating;row->deviation=st->deviation;
                                row->volatility=st->volatility;row->matches=(int64)st->matches;
                            }
                        }
                    }PG_FINALLY();{
                        lp_idmap_free(solved);
                    }PG_END_TRY();
                }else{
                    const char *scols[]={"claim","rating","deviation","volatility","matches"},*scol;
                    Oid starget=lp_pg_relation("consensus");
                    Relation sparent=lp_pg_read_parent(starget,&scol,scols,lengthof(scols));
                    PresenceKey *keys=palloc_extended(nu*sizeof(*keys),MCXT_ALLOC_HUGE);
                    for(size_t i=0;i<nu;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(col.by_entity,i),.ordinal=(int64)i+1};
                    struct { HolderStandingCollect *c; AttrNumber st[4]; } sctx={.c=&col};
                    for(int j=0;j<4;j++)sctx.st[j]=get_attnum(starget,scols[j+1]);
                    lp_pg_indexed_keys(sparent,scol,keys,nu,collect_holder_standing,&sctx);
                    pfree(keys);table_close(sparent,NoLock);
                }
            }
            pfree(col.links);
            for(size_t i=0;i<col.rows.n;i++){
                const HolderStandingRow *r=&col.rows.v[i];
                Datum v[11]={
                    Int64GetDatum(r->hit.ordinal),
                    r->hit.hub?(Datum)0:id_datum(&r->hit.entity),
                    r->hit.path,
                    Int16GetDatum(r->hit.tier),
                    PointerGetDatum(r->mask_copy),
                    Int16GetDatum(r->hit.kind),
                    BoolGetDatum(r->hit.hub),
                    Float8GetDatum(r->rating),
                    Float8GetDatum(r->deviation),
                    Float8GetDatum(r->volatility),
                    Int64GetDatum(r->matches)
                };
                bool nl[11]={
                    false,r->hit.hub,r->hit.hub,r->hit.hub,r->hit.hub||!r->mask_copy,false,false,
                    !r->known,!r->known,!r->known,!r->known
                };
                set_put(fcinfo,out,td,v,nl);
                if(!r->hit.hub)pfree(DatumGetPointer(v[1]));
            }
        }
    }
    lp_vec_free(&col.rows);
    lp_idmap_free(col.by_entity);
    MemoryContextDelete(col.ctx);
    if(witnesses)pfree(witnesses);
    pfree(ids);pfree(required);
    return (Datum)0;
}

/* Claim selection is another projection of the same GIN/primary-key
 * frontier. Qualification precedes the caller's fan: masks, middle refusals,
 * tier and a real standing must all survive. ID order makes bounded results
 * independent of heap order; per-input duplicates retain their ordinals. */
typedef struct {CarrierLocation carrier;Datum path;int16 tier;bool known;double rating,deviation,volatility;int64 matches;} ClaimStanding;
typedef struct {
    PresenceOutput output;lp_idmap *wanted,*parents,*required,*opposite;lp_id *refuse,*include;int nrefuse,ninclude,*links;
    bool each,done,has_scope;const lp_id *witnesses;int nwitnesses;Oid idarr;
    uint64 limit,sent,saturated,epoch,nom_epoch,nom_saturated;int floor,endpoint;int64 arity;lp_mask bits;
    AttrNumber id,path,tier,mask,standing[4];MemoryContext tuple_context;
    lp_pg_claim_visit visit;void *visit_context;
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
        if(read->visit){
            PgClaimHit hit={.i=0,.entity=key->id,.path=path,.rating=standing->rating,
                .deviation=standing->deviation,.volatility=standing->volatility,.matches=standing->matches};
            read->visit(&hit,read->visit_context);
        }else{
            set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values+1,nulls+1);
        }
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
                    if(read->visit){
                        PgClaimHit hit={.i=(int64)input+1,.entity=key->id,.path=path,.rating=standing->rating,
                            .deviation=standing->deviation,.volatility=standing->volatility,.matches=standing->matches};
                        read->visit(&hit,read->visit_context);
                    }else{
                        values[0]=Int64GetDatum((int64)input+1);set_put(read->output.fcinfo,read->output.output,read->output.descriptor,values,nulls);
                    }
                }
            }
            if(++part->sent[dir]==read->limit)read->saturated++;
        }
        read->done=read->saturated==target_dirs;
    }
}
static void run_claims_query(ClaimRead *read,const lp_id *ids,int count,const lp_id *opposite,int nopposite)
{
    read->wanted=lp_idmap_sized(sizeof(ClaimKey));read->parents=lp_idmap_sized(sizeof(ClaimStanding));read->required=lp_idmap_new();
    read->opposite=nopposite>0?lp_idmap_new():NULL;
    read->links=palloc_extended((Size)count*sizeof(int),MCXT_ALLOC_HUGE);
    PG_TRY();{
        for(int i=0;i<nopposite;i++)lp_idmap_put(read->opposite,opposite+i,NULL);
        for(int i=0;i<read->ninclude;i++)lp_idmap_put(read->required,read->include+i,NULL);
        for(int i=count-1;i>=0;i--){bool fresh;size_t at=lp_idmap_put(read->wanted,ids+i,&fresh);ClaimKey *part=lp_idmap_at(read->wanted,at);
            if(fresh){memset(part,0,sizeof(*part));part->head=-1;}read->links[i]=part->head;part->head=i;}
        const char *column,*entity_columns[]={"id","tier"};Oid target=lp_pg_relation("entity");
        Relation entities=lp_pg_read_parent(target,&column,entity_columns,lengthof(entity_columns));
        size_t n=lp_idmap_count(read->wanted);PresenceKey *keys=palloc_extended(n*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<n;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(read->wanted,i),.ordinal=(int64)i+1};
        read->tier=get_attnum(target,"tier");lp_pg_indexed_keys(entities,column,keys,n,claim_key_tier,read);
        table_close(entities,NoLock);pfree(keys);
        int16 gin_min_floor=n>0?PG_INT16_MAX:-1;
        for(size_t i=0;i<n;i++){
            int kf=((ClaimKey *)lp_idmap_at(read->wanted,i))->floor;
            read->floor=Max(read->floor,kf);
            gin_min_floor=Min(gin_min_floor,(int16)kf);
        }
        if(!read->each)gin_min_floor=(int16)read->floor;
        read->tuple_context=AllocSetContextCreate(CurrentMemoryContext,"Native selected claim",ALLOCSET_DEFAULT_SIZES);
        const char *path_columns[]={"entity","path","tier","mask"};target=lp_pg_relation("physicality");
        Relation paths=lp_pg_read_parent(target,&column,path_columns,lengthof(path_columns));
        read->id=get_attnum(target,"entity");read->path=get_attnum(target,"path");read->tier=get_attnum(target,"tier");read->mask=get_attnum(target,"mask");
        uint64 nom_cap=(read->each && read->limit<UINT64_MAX)?Max(read->limit,UINT64_C(64)):0;
        scan_container_rows(paths,column,read->wanted,read->each?3:7,read->required,true,read->endpoint,read->opposite,read->bits,false,nom_cap,gin_min_floor,claim_nominate,read);
        table_close(paths,NoLock);
        n=lp_idmap_count(read->parents);keys=palloc_extended((n?n:1)*sizeof(*keys),MCXT_ALLOC_HUGE);
        for(size_t i=0;i<n;i++)keys[i]=(PresenceKey){.id=*lp_idmap_key(read->parents,i),.ordinal=(int64)i+1};
        if(read->has_scope){
            if(n>0&&read->nwitnesses>0){
                lp_idmap *solved=lp_idmap_sized(sizeof(lp_standing));
                PG_TRY();{
                    lp_pg_solve_scoped_standings(lp_idmap_keys(read->parents),(int)n,read->idarr,read->witnesses,read->nwitnesses,solved);
                    for(size_t i=0;i<n;i++){
                        const lp_standing *st=lp_idmap_lookup(solved,&keys[i].id);
                        if(!st)continue;
                        ClaimStanding *standing=lp_idmap_at(read->parents,(size_t)keys[i].ordinal-1);
                        standing->known=true;standing->rating=st->rating;standing->deviation=st->deviation;
                        standing->volatility=st->volatility;standing->matches=(int64)st->matches;
                    }
                }PG_FINALLY();{lp_idmap_free(solved);}PG_END_TRY();
            }
        }else{
            const char *standing_columns[]={"claim","rating","deviation","volatility","matches"};target=lp_pg_relation("consensus");
            Relation standings=lp_pg_read_parent(target,&column,standing_columns,lengthof(standing_columns));
            for(int i=0;i<4;i++)read->standing[i]=get_attnum(target,standing_columns[i+1]);
            lp_pg_indexed_keys(standings,column,keys,n,claim_standing,read);table_close(standings,NoLock);
        }
        for(size_t i=0;i<n && !read->done;i++){
            if(((ClaimStanding *)lp_idmap_at(read->parents,(size_t)keys[i].ordinal-1))->known)
                claim_emit_cached(read,keys+i);
        }
        MemoryContextDelete(read->tuple_context);read->tuple_context=NULL;pfree(keys);
    }PG_FINALLY();{if(read->tuple_context)MemoryContextDelete(read->tuple_context);lp_idmap_free(read->wanted);lp_idmap_free(read->parents);lp_idmap_free(read->required);lp_idmap_free(read->opposite);}PG_END_TRY();
    pfree(read->links);
}
void lp_pg_claims_each_scoped_direct(const lp_id *ids,int count,int64 fan,lp_mask bits,
                                     const lp_id *refuse,int nrefuse,int16 endpoint,
                                     const lp_id *include,int ninclude,int64 arity,
                                     const lp_id *opposite,int nopposite,
                                     bool has_scope,const lp_id *witnesses,int nwitnesses,Oid idarr,
                                     lp_pg_claim_visit visit,void *context)
{
    if(count<=0||!visit||fan==0)return;
    if(has_scope&&nwitnesses<=0)return;
    if(endpoint<0||endpoint>3)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native claim endpoint must be 0 (any), 1 (first), 2 (last) or 3 (either)")));
    if(arity<0)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native claim arity must be zero or positive")));
    if(opposite && nopposite==0)return;
    if(include && ninclude==0)return;
    if(opposite && endpoint!=1 && endpoint!=2)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native opposite endpoint requires first or last endpoint selection")));
    ClaimRead read={
        .each=true,.limit=fan<0?UINT64_MAX:(uint64)fan,.endpoint=endpoint,.arity=arity,.bits=bits|UINT64_C(1),
        .has_scope=has_scope,.witnesses=witnesses,.nwitnesses=nwitnesses,.idarr=idarr,
        .refuse=(lp_id *)refuse,.nrefuse=nrefuse,.include=(lp_id *)include,.ninclude=ninclude,
        .visit=visit,.visit_context=context
    };
    run_claims_query(&read,ids,count,opposite,nopposite);
}
void lp_pg_claims_each_direct(const lp_id *ids,int count,int64 fan,lp_mask bits,
                              const lp_id *refuse,int nrefuse,int16 endpoint,
                              const lp_id *include,int ninclude,int64 arity,
                              const lp_id *opposite,int nopposite,
                              lp_pg_claim_visit visit,void *context)
{
    lp_pg_claims_each_scoped_direct(ids,count,fan,bits,refuse,nrefuse,endpoint,include,ninclude,arity,opposite,nopposite,false,NULL,0,InvalidOid,visit,context);
}
static Datum read_claims(FunctionCallInfo fcinfo,bool each)
{
    ClaimRead read={.output.fcinfo=fcinfo,.each=each,.limit=UINT64_MAX,.bits=UINT64_C(1)};
    read.output.output=set_begin(fcinfo,each?"laplace_claims_each":"laplace_claims",&read.output.descriptor);
    if(PG_ARGISNULL(0))return (Datum)0;
    lp_id *scope_ids=NULL;
    if(PG_NARGS()>8&&!PG_ARGISNULL(8)){
        int nwit=0;scope_ids=ids_of(PG_GETARG_ARRAYTYPE_P(8),&nwit);
        if(!nwit){pfree(scope_ids);return (Datum)0;}
        read.has_scope=true;read.witnesses=scope_ids;read.nwitnesses=nwit;
        read.idarr=get_fn_expr_argtype(fcinfo->flinfo,0);
    }
    if(PG_NARGS()>4){
        if(PG_ARGISNULL(4)){if(scope_ids)pfree(scope_ids);return (Datum)0;}
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
        read.limit=(uint64)limit;if(!limit){if(scope_ids)pfree(scope_ids);return (Datum)0;}
    }
    if(!PG_ARGISNULL(2)){
        lp_mask extra=0;
        if(!read_mask_bits(PG_GETARG_ARRAYTYPE_P(2),&extra)){if(scope_ids)pfree(scope_ids);return (Datum)0;}
        read.bits|=extra;
    }
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    read.refuse=PG_ARGISNULL(3)?NULL:ids_of(PG_GETARG_ARRAYTYPE_P(3),&read.nrefuse);
    if(!count){pfree(ids);if(read.refuse)pfree(read.refuse);if(scope_ids)pfree(scope_ids);return (Datum)0;}
    if(PG_NARGS()>5 && !PG_ARGISNULL(5)){
        read.include=ids_of(PG_GETARG_ARRAYTYPE_P(5),&read.ninclude);
        if(!read.ninclude){pfree(ids);if(read.refuse)pfree(read.refuse);pfree(read.include);if(scope_ids)pfree(scope_ids);return (Datum)0;}
    }
    lp_id *opposite=NULL;int nopposite=0;
    if(PG_NARGS()>7 && !PG_ARGISNULL(7)){
        if(read.endpoint!=1 && read.endpoint!=2)
            ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native opposite endpoint requires first or last endpoint selection")));
        opposite=ids_of(PG_GETARG_ARRAYTYPE_P(7),&nopposite);
        if(!nopposite){pfree(ids);if(read.refuse)pfree(read.refuse);if(read.include)pfree(read.include);pfree(opposite);if(scope_ids)pfree(scope_ids);return (Datum)0;}
    }
    run_claims_query(&read,ids,count,opposite,nopposite);
    pfree(ids);if(read.refuse)pfree(read.refuse);if(read.include)pfree(read.include);if(opposite)pfree(opposite);if(scope_ids)pfree(scope_ids);return (Datum)0;
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
            scan_container_rows(parent,column,walk.keys,3,NULL,false,0,NULL,0,false,h_cap,-1,held_first,&walk);
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

/* ---------------------------------------------------------------- generic batched upward closure (held_up in DB) */
typedef struct {
    lp_id entity;
    int16 tier;
    uint64 keys_mask;
    bool is_file, is_metadata, is_hub, expanded;
    int32 level;
    lp_vec(int32) child_idxs;
    lp_vec(int16) child_saids;
    lp_vec(int32) child_runs;
    lp_vec(int32) child_ordinals;
} HeldClosureNode;

typedef struct {
    int16 mode; /* 0 = content (skip metadata, stop at file), 1 = name (only file metadata, stop at file), 2 = all */
    size_t max_nodes;
    bool cut;
    MemoryContext ctx;
    lp_idmap *by_id; /* lp_id -> int32 node_idx */
    lp_vec(HeldClosureNode) nodes;
    lp_idmap *front_map; /* lp_id -> int32 node_idx (front in current level) */
    lp_idmap *next_map;
    lp_idmap *level_seen;
    const lp_id *front_ids;
    size_t front_n;
    int32 level;
    lp_vec(lp_vertex) vbuf;
} HeldClosureWalk;

static int32 held_closure_ensure(HeldClosureWalk *w, const lp_id *id, int32 level, bool *fresh_out){
    bool fresh;
    int32 *pidx = lp_idmap_get(w->by_id, id, &fresh);
    if (fresh) {
        *pidx = (int32)w->nodes.n;
        HeldClosureNode n = { .entity = *id, .tier = -1, .level = level };
        lp_push(&w->nodes, n);
    }
    if (fresh_out) *fresh_out = fresh;
    return *pidx;
}

static void held_closure_seed_path(const lp_id *id, Datum path, Datum mask, void *context){
    (void)mask;
    HeldClosureWalk *w = context;
    int32 *pidx = lp_idmap_lookup(w->by_id, id);
    if (!pidx) return;
    Geo g = geo_of(path);
    if (g.p.n == 0) return;
    lp_vec_reserve(&w->vbuf, g.p.n);
    if (lp_path_decode(g.p, w->vbuf.v, g.p.n) != g.p.n) return;
    HeldClosureNode *n = &w->nodes.v[*pidx];
    if (w->vbuf.v[0].said == LP_SAID_METADATA) {
        n->is_metadata = true;
        if (g.p.n >= 2) n->is_file = true;
    }
}

static void held_closure_hit(const PgHolderHit *hit, void *context){
    HeldClosureWalk *w = context;
    if (w->cut) return;
    if (hit->hub) {
        int64 ord = hit->ordinal - 1;
        if (ord >= 0 && (size_t)ord < w->front_n) {
            int32 *pidx = lp_idmap_lookup(w->by_id, &w->front_ids[ord]);
            if (pidx) w->nodes.v[*pidx].is_hub = true;
        }
        return;
    }
    if (!hit->path) return;
    bool fresh_level;
    lp_idmap_get(w->level_seen, &hit->entity, &fresh_level);
    if (!fresh_level) return;

    Geo g = geo_of(hit->path);
    size_t nv = g.p.n;
    if (nv == 0) return;
    lp_vec_reserve(&w->vbuf, nv);
    lp_vertex *v = w->vbuf.v;
    if (lp_path_decode(g.p, v, nv) != nv)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("laplace_held_closure: invalid stored path")));

    bool meta = (v[0].said == LP_SAID_METADATA);
    bool file = (nv >= 2 && meta);
    bool matched = false;
    uint64 gained = 0;

    for (size_t k = 0; k < nv; k++) {
        if (w->mode == 0 && v[k].said == LP_SAID_METADATA) continue;
        if (w->mode == 1 && file && k > 0) continue;
        int32 *cidx = lp_idmap_lookup(w->front_map, &v[k].id);
        if (!cidx) continue;
        gained |= w->nodes.v[*cidx].keys_mask;
        matched = true;
    }
    if (!matched) return;

    if (lp_idmap_find(w->by_id, &hit->entity) < 0 && w->nodes.n >= w->max_nodes) {
        w->cut = true;
        return;
    }
    bool fresh;
    int32 pidx = held_closure_ensure(w, &hit->entity, w->level, &fresh);
    HeldClosureNode *p = &w->nodes.v[pidx];
    uint64 before = p->keys_mask;
    p->keys_mask |= gained;
    p->is_metadata |= meta;
    p->is_file |= file;
    p->tier = hit->tier;

    for (size_t k = 0; k < nv; k++) {
        if (w->mode == 0 && v[k].said == LP_SAID_METADATA) continue;
        if (w->mode == 1 && file && k > 0) continue;
        int32 *cidx = lp_idmap_lookup(w->front_map, &v[k].id);
        if (!cidx) continue;
        lp_push(&p->child_idxs, *cidx);
        lp_push(&p->child_saids, (int16)v[k].said);
        lp_push(&p->child_runs, (int32)(v[k].run ? v[k].run : 1));
        lp_push(&p->child_ordinals, (int32)k);
    }

    bool should_push = (w->mode == 2) ? fresh : (!file && (fresh || p->keys_mask != before));
    if (should_push)
        lp_idmap_put(w->next_map, &hit->entity, NULL);
}

PG_FUNCTION_INFO_V1(laplace_held_closure);
Datum laplace_held_closure(PG_FUNCTION_ARGS){
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_held_closure", &td);
    if (PG_ARGISNULL(0)) return (Datum)0;
    int count = 0;
    lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &count);
    int16 mode = (!PG_ARGISNULL(1)) ? PG_GETARG_INT16(1) : 0;
    int64 cap = (!PG_ARGISNULL(2)) ? PG_GETARG_INT64(2) : -1;
    int32 max_levels = (!PG_ARGISNULL(3)) ? PG_GETARG_INT32(3) : 32;
    int32 max_nodes = (!PG_ARGISNULL(4)) ? PG_GETARG_INT32(4) : 400000;
    if (mode < 0 || mode > 2 || cap < -1 || max_levels < 0 || max_nodes < 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace_held_closure: invalid parameters")));
    if (count <= 0) { pfree(ids); return (Datum)0; }

    HeldClosureWalk w = {
        .mode = mode,
        .max_nodes = (size_t)max_nodes,
        .ctx = CurrentMemoryContext,
        .by_id = lp_idmap_sized(sizeof(int32)),
        .front_map = lp_idmap_sized(sizeof(int32)),
        .next_map = lp_idmap_new(),
        .level_seen = lp_idmap_new()
    };

    PG_TRY(); {
        for (int i = 0; i < count; i++) {
            int32 idx = held_closure_ensure(&w, ids + i, 0, NULL);
            if (i < 64) w.nodes.v[idx].keys_mask |= (UINT64_C(1) << i);
            *(int32 *)lp_idmap_get(w.front_map, ids + i, NULL) = idx;
        }
        lp_pg_lookup_paths(lp_idmap_keys(w.front_map), lp_idmap_count(w.front_map), false, held_closure_seed_path, &w);

        int32 levels = 0;
        while (lp_idmap_count(w.front_map) > 0 && levels < max_levels && !w.cut) {
            CHECK_FOR_INTERRUPTS();
            levels++;
            w.level = levels;
            lp_idmap_clear(w.next_map);
            lp_idmap_clear(w.level_seen);
            w.front_ids = lp_idmap_keys(w.front_map);
            w.front_n = lp_idmap_count(w.front_map);
            lp_pg_holders_direct(w.front_ids, (int)w.front_n, NULL, 0, (levels == 1 && cap >= 0), -1, (levels == 1 ? cap : -1), 3, 0, 0, held_closure_hit, &w);
            for (size_t i = 0; i < w.front_n; i++) {
                int32 *idx = lp_idmap_lookup(w.front_map, &w.front_ids[i]);
                if (idx) w.nodes.v[*idx].expanded = true;
            }
            lp_idmap_clear(w.front_map);
            const lp_id *nx = lp_idmap_keys(w.next_map);
            size_t nxn = lp_idmap_count(w.next_map);
            for (size_t i = 0; i < nxn; i++) {
                int32 *idx = lp_idmap_lookup(w.by_id, &nx[i]);
                if (idx) *(int32 *)lp_idmap_get(w.front_map, &nx[i], NULL) = *idx;
            }
        }

        if (w.nodes.n > 0) {
            lp_id *all_ids = palloc(sizeof(lp_id) * w.nodes.n);
            int16 *tiers = palloc(sizeof(int16) * w.nodes.n);
            for (size_t i = 0; i < w.nodes.n; i++) all_ids[i] = w.nodes.v[i].entity;
            lp_pg_lookup_entity_tiers(all_ids, w.nodes.n, tiers);
            for (size_t i = 0; i < w.nodes.n; i++)
                if (w.nodes.v[i].tier < 0 && tiers[i] >= 0) w.nodes.v[i].tier = tiers[i];
            pfree(all_ids); pfree(tiers);
        }

        for (size_t i = 0; i < w.nodes.n; i++) {
            CHECK_FOR_INTERRUPTS();
            const HeldClosureNode *n = &w.nodes.v[i];
            int nec = (int)n->child_idxs.n;
            Datum *d_c = nec ? palloc(sizeof(Datum) * (size_t)nec) : NULL;
            Datum *d_s = nec ? palloc(sizeof(Datum) * (size_t)nec) : NULL;
            Datum *d_r = nec ? palloc(sizeof(Datum) * (size_t)nec) : NULL;
            Datum *d_o = nec ? palloc(sizeof(Datum) * (size_t)nec) : NULL;
            for (int k = 0; k < nec; k++) {
                d_c[k] = Int32GetDatum(n->child_idxs.v[k]);
                d_s[k] = Int16GetDatum(n->child_saids.v[k]);
                d_r[k] = Int32GetDatum(n->child_runs.v[k]);
                d_o[k] = Int32GetDatum(n->child_ordinals.v[k]);
            }
            ArrayType *a_c = construct_array(d_c, nec, INT4OID, 4, true, TYPALIGN_INT);
            ArrayType *a_s = construct_array(d_s, nec, INT2OID, 2, true, TYPALIGN_SHORT);
            ArrayType *a_r = construct_array(d_r, nec, INT4OID, 4, true, TYPALIGN_INT);
            ArrayType *a_o = construct_array(d_o, nec, INT4OID, 4, true, TYPALIGN_INT);
            if (d_c) { pfree(d_c); pfree(d_s); pfree(d_r); pfree(d_o); }

            Datum vals[15] = {
                Int32GetDatum((int32)i),
                id_datum(&n->entity),
                Int16GetDatum(n->tier),
                Int64GetDatum((int64)n->keys_mask),
                BoolGetDatum(n->is_file),
                BoolGetDatum(n->is_metadata),
                BoolGetDatum(n->is_hub),
                BoolGetDatum(n->expanded),
                Int32GetDatum(n->level),
                PointerGetDatum(a_c),
                PointerGetDatum(a_s),
                PointerGetDatum(a_r),
                PointerGetDatum(a_o),
                BoolGetDatum(w.cut),
                Int32GetDatum(levels)
            };
            bool nl[15] = { false };
            set_put(fcinfo, ts, td, vals, nl);
            pfree(DatumGetPointer(vals[1]));
            pfree(a_c); pfree(a_s); pfree(a_r); pfree(a_o);
        }
    } PG_FINALLY(); {
        for (size_t i = 0; i < w.nodes.n; i++) {
            lp_vec_free(&w.nodes.v[i].child_idxs);
            lp_vec_free(&w.nodes.v[i].child_saids);
            lp_vec_free(&w.nodes.v[i].child_runs);
            lp_vec_free(&w.nodes.v[i].child_ordinals);
        }
        lp_vec_free(&w.nodes); lp_vec_free(&w.vbuf);
        lp_idmap_free(w.by_id); lp_idmap_free(w.front_map);
        lp_idmap_free(w.next_map); lp_idmap_free(w.level_seen);
        pfree(ids);
    } PG_END_TRY();
    return (Datum)0;
}

/* Microsecond trajectory salience over physicality_paths GIN + LineStringZM
 * (continuous self-information I(e), local span contraction / PMI, and
 * M-outcome / attestation WIN vs LOSS differential), zero heap/TOAST scans. */
static bool sal_is_glue(const lp_tier0_record *t0, const lp_layout *flags,
                        const lp_field *gc, const lp_field *ws, int32 ws_yes,
                        bool has_crlf, const lp_id *crlf, const lp_id *id)
{
    if (has_crlf && lp_id_eq(id, crlf)) return true;
    int64_t cp = lp_tier0_codepoint(t0, id);
    if (cp < 0) return false;
    if (cp == '\r' || cp == '\n') return true;
    if (flags && ws && ws_yes >= 0 && lp_flags_get(flags, (uint32)cp, ws) == (uint32)ws_yes)
        return true;
    if (flags && gc) {
        uint32 v = lp_flags_get(flags, (uint32)cp, gc);
        if (v < gc->nvalues) {
            const char *nm = flags->value[gc->first + v].name;
            if (nm[0] == 'P' || nm[0] == 'Z' || nm[0] == 'C') return true;
        }
    }
    return false;
}

static int64 sal_part_rows(Relation table, Oid relid, Oid pk_oid, int64 pending, Snapshot snap)
{
    float4 reltuples = -1.0f;
    HeapTuple ctup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (HeapTupleIsValid(ctup)) {
        reltuples = ((Form_pg_class)GETSTRUCT(ctup))->reltuples;
        ReleaseSysCache(ctup);
    }
    BlockNumber nblocks = RelationGetNumberOfBlocks(table);
    if ((reltuples <= 0.0f || (reltuples <= 200.0f && nblocks <= 16)) && nblocks <= 256 && OidIsValid(pk_oid)) {
        Relation pk_idx = index_open(pk_oid, AccessShareLock);
        IndexScanDesc pk_scan = index_beginscan(table, pk_idx, snap, NULL, 0, 0);
        int64 count = 0;
        ItemPointer tid;
        index_rescan(pk_scan, NULL, 0, NULL, 0);
        while ((tid = index_getnext_tid(pk_scan, ForwardScanDirection)) != NULL && ItemPointerIsValid(tid)) {
            CHECK_FOR_INTERRUPTS();
            count++;
        }
        index_endscan(pk_scan);
        index_close(pk_idx, NoLock);
        return count;
    }
    if (reltuples > 0.0f) return (int64)llround((double)reltuples) + pending;
    return Max(pending, (int64)nblocks * 32);
}

static Datum sal_path_member_key(const lp_id *id)
{
    uint8 bytes[17];
    bytes[0] = 0;
    memcpy(bytes + 1, id->b, 16);
    return PointerGetDatum(bytes_datum(bytes, sizeof bytes));
}

static int64 sal_tbm_consume_count(TIDBitmap *tmp)
{
    if (!tmp || tbm_is_empty(tmp)) {
        if (tmp) tbm_free(tmp);
        return 0;
    }
    OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
    TBMPrivateIterator *it = tbm_begin_private_iterate(tmp);
    TBMIterateResult res;
    int64 count = 0;
    while (tbm_private_iterate(it, &res)) {
        if (res.lossy) count += 32;
        else count += tbm_extract_page_tuple(&res, offsets, TBM_MAX_TUPLES_PER_PAGE);
    }
    tbm_end_private_iterate(it);
    tbm_free(tmp);
    return count;
}

static int64 sal_tbm_count_copy(const TIDBitmap *src, Size bm_mem)
{
    if (!src || tbm_is_empty(src)) return 0;
    TIDBitmap *tmp = tbm_create(bm_mem, NULL);
    tbm_union(tmp, src);
    return sal_tbm_consume_count(tmp);
}

static int64 sal_tbm_intersect_count(const TIDBitmap *a, const TIDBitmap *b, Size bm_mem)
{
    if (!a || !b || tbm_is_empty(a) || tbm_is_empty(b)) return 0;
    TIDBitmap *tmp = tbm_create(bm_mem, NULL);
    tbm_union(tmp, a);
    tbm_intersect(tmp, b);
    return sal_tbm_consume_count(tmp);
}

typedef struct { int ua, ub; int64 co; bool stored; } SalPair;
typedef struct { lp_id entity; Datum path; } SalHolder;

static uint32 sal_simple_lower(uint32 cp)
{
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) return cp + 32;
    if (cp >= 0x0100 && cp <= 0x017E) {
        if (cp == 0x0130 || cp == 0x0149 || cp == 0x0178) return cp == 0x0178 ? 0x00FF : cp;
        if (cp <= 0x0137) return (cp & 1) == 0 ? cp + 1 : cp;
        if (cp >= 0x0139 && cp <= 0x0148) return (cp & 1) == 1 ? cp + 1 : cp;
        if (cp >= 0x014A && cp <= 0x0177) return (cp & 1) == 0 ? cp + 1 : cp;
        if (cp >= 0x0179 && cp <= 0x017E) return (cp & 1) == 1 ? cp + 1 : cp;
    }
    if ((cp >= 0x0391 && cp <= 0x03A1) || (cp >= 0x03A3 && cp <= 0x03AB)) return cp + 32;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 32;
    if (cp == 0x0401) return 0x0451;
    return cp;
}

typedef struct {
    const lp_tier0_record *t0;
    lp_idmap *umap;
    const lp_id *u_ids;
    int *u_to_l;
    lp_idmap *lmap;
    lp_id *l_ids;
    int nl;
} SalLowerCtx;

static void sal_t1_path_visit(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    SalLowerCtx *ctx = context;
    int64 u_at = lp_idmap_find(ctx->umap, id);
    if (u_at < 0) return;
    int u = *(int *)lp_idmap_at(ctx->umap, (size_t)u_at);
    if (ctx->u_to_l[u] >= 0) return;

    bytea *det = (bytea *)PG_DETOAST_DATUM(path);
    Geo g = geo_of(PointerGetDatum(det));
    lp_vertex stack_v[64];
    lp_vertex *verts = g.p.n <= 64 ? stack_v : palloc(sizeof(lp_vertex) * g.p.n);
    size_t nv = lp_path_decode(g.p, verts, g.p.n);

    size_t tot_cp = 0;
    for (size_t i = 0; i < nv; i++) tot_cp += verts[i].run ? (size_t)verts[i].run : 1;
    if (tot_cp > 0 && tot_cp <= 256) {
        lp_id lc_stack[64];
        lp_id *lc = tot_cp <= 64 ? lc_stack : palloc(sizeof(lp_id) * tot_cp);
        bool all_t0 = true, changed = false;
        size_t pos = 0;
        for (size_t i = 0; i < nv && all_t0; i++) {
            int64_t cp = lp_tier0_codepoint(ctx->t0, &verts[i].id);
            if (cp < 0) { all_t0 = false; break; }
            uint32 lcp = sal_simple_lower((uint32)cp);
            if (lcp != (uint32)cp) changed = true;
            const lp_tier0_record *lr = lp_tier0_at(ctx->t0, lcp);
            if (!lr) { all_t0 = false; break; }
            size_t run = verts[i].run ? (size_t)verts[i].run : 1;
            for (size_t r = 0; r < run; r++) lc[pos++] = lr->id;
        }
        if (all_t0 && changed && pos > 0) {
            lp_id lid;
            if (pos == 1) lid = lc[0];
            else lp_id_compose(lc, pos, &lid);
            if (!lp_id_eq(&lid, &ctx->u_ids[u])) {
                bool fresh = false;
                size_t l_at = lp_idmap_put(ctx->lmap, &lid, &fresh);
                int *lslot = lp_idmap_at(ctx->lmap, l_at);
                if (fresh) {
                    *lslot = ctx->nl;
                    ctx->l_ids[ctx->nl++] = lid;
                }
                ctx->u_to_l[u] = *lslot;
            }
        }
        if (lc != lc_stack) pfree(lc);
    }

    if (verts != stack_v) pfree(verts);
    if ((Pointer)det != DatumGetPointer(path)) pfree(det);
}
typedef struct {
    lp_idmap *wanted;
    lp_idmap *holders;
    lp_mask bits;
    AttrNumber id_att, path_att, mask_att;
    MemoryContext hctx;
} SalLossCollect;

static bool sal_loss_visit(TupleTableSlot *slot, void *context)
{
    SalLossCollect *lc = context;
    bool missing = false;
    if (lc->bits) {
        Datum mv = slot_getattr(slot, lc->mask_att, &missing);
        if (missing || (lp_pg_mask_native(mv) & lc->bits) != lc->bits) return true;
    }
    Datum ev = slot_getattr(slot, lc->id_att, &missing);
    if (missing) return true;
    const lp_id *eid = (const lp_id *)DatumGetPointer(ev);
    Datum pv = slot_getattr(slot, lc->path_att, &missing);
    if (missing) return true;
    bytea *det = (bytea *)PG_DETOAST_DATUM(pv);
    Geo g = geo_of(PointerGetDatum(det));
    bool any_active = false;
    for (size_t i = 0; i < g.p.n; i++) {
        lp_id cid = lp_path_id(g.p, i);
        int64 at = lp_idmap_find(lc->wanted, &cid);
        if (at >= 0) {
            HolderKey *hk = lp_idmap_at(lc->wanted, (size_t)at);
            hk->held[0]++;
            if (hk->held[0] <= 64) any_active = true;
        }
    }
    if (!any_active) return true;
    bool fresh = false;
    size_t h_at = lp_idmap_put(lc->holders, eid, &fresh);
    if (fresh) {
        Size sz = VARSIZE_ANY(det);
        bytea *copy = MemoryContextAlloc(lc->hctx, sz);
        memcpy(copy, det, sz);
        SalHolder *sh = lp_idmap_at(lc->holders, h_at);
        sh->entity = *eid;
        sh->path = PointerGetDatum(copy);
    }
    return true;
}

void lp_pg_salience(const lp_id *occ, int nocc, const lp_id *scope, int nscope, PgSalience *out)
{
    if (nocc <= 0 || !out) return;
    memset(out, 0, sizeof(PgSalience) * (size_t)nocc);

    const lp_tier0_record *t0 = lp_pg_tier0();
    const lp_layout *flags = lp_pg_flags();
    const lp_field *gc = flags ? lp_flags_field(flags, "gc") : NULL;
    const lp_field *ws = flags ? lp_flags_field(flags, "White_Space") : NULL;
    int32 ws_yes = (flags && ws) ? lp_flags_value(flags, ws, "Yes") : -1;
    if (flags && ws && ws_yes < 0) ws_yes = lp_flags_value(flags, ws, "Y");
    lp_id crlf = { { 0 } };
    bool has_crlf = false;
    const lp_tier0_record *cr_rec = lp_tier0_at(t0, 13);
    const lp_tier0_record *lf_rec = lp_tier0_at(t0, 10);
    if (cr_rec && lf_rec) {
        lp_id cr_lf[2] = { cr_rec->id, lf_rec->id };
        lp_id_compose(cr_lf, 2, &crlf);
        has_crlf = true;
    }

    int16 *occ_tiers = palloc(sizeof(int16) * (size_t)nocc);
    lp_pg_lookup_entity_tiers(occ, (size_t)nocc, occ_tiers);
    bool *is_glue = palloc0(sizeof(bool) * (size_t)nocc);
    int *occ_to_u = palloc(sizeof(int) * (size_t)nocc);
    lp_idmap *umap = lp_idmap_sized(sizeof(int));
    int nu = 0, ng = 0;
    int16 min_active_tier = INT16_MAX, min_any_tier = INT16_MAX;

    for (int i = 0; i < nocc; i++) {
        if (occ_tiers[i] < 0 && lp_tier0_codepoint(t0, &occ[i]) >= 0) occ_tiers[i] = 0;
        out[i].tier = occ_tiers[i];
        is_glue[i] = sal_is_glue(t0, flags, gc, ws, ws_yes, has_crlf, &crlf, &occ[i]);
        if (occ_tiers[i] >= 0 && occ_tiers[i] < min_any_tier) min_any_tier = occ_tiers[i];
        if (!is_glue[i]) {
            ng++;
            if (occ_tiers[i] >= 0 && occ_tiers[i] < min_active_tier) min_active_tier = occ_tiers[i];
        }
        bool fresh = false;
        size_t at = lp_idmap_put(umap, &occ[i], &fresh);
        int *slot = lp_idmap_at(umap, at);
        if (fresh) *slot = nu++;
        occ_to_u[i] = *slot;
    }
    if (min_active_tier == INT16_MAX)
        min_active_tier = (min_any_tier == INT16_MAX) ? -1 : min_any_tier;

    const lp_id *u_ids = lp_idmap_keys(umap);
    int16 *u_tiers = palloc(sizeof(int16) * (size_t)nu);
    bool *u_glue = palloc0(sizeof(bool) * (size_t)nu);
    for (int i = 0; i < nocc; i++) {
        u_tiers[occ_to_u[i]] = occ_tiers[i];
        if (is_glue[i]) u_glue[occ_to_u[i]] = true;
    }

    int *ng_pos = palloc(sizeof(int) * (size_t)(ng ? ng : 1));
    int ng_k = 0;
    for (int i = 0; i < nocc; i++) if (!is_glue[i]) ng_pos[ng_k++] = i;

    int npairs = 0;
    SalPair *pairs = palloc(sizeof(SalPair) * (size_t)(ng > 1 ? ng - 1 : 1));
    int *step_pair = palloc(sizeof(int) * (size_t)(ng > 1 ? ng - 1 : 1));
    for (int k = 0; k < ng - 1; k++) {
        int ua = occ_to_u[ng_pos[k]], ub = occ_to_u[ng_pos[k + 1]];
        if (ua == ub) { step_pair[k] = -1; continue; }
        int lo = ua < ub ? ua : ub, hi = ua < ub ? ub : ua;
        int found = -1;
        for (int j = 0; j < npairs; j++)
            if (pairs[j].ua == lo && pairs[j].ub == hi) { found = j; break; }
        if (found < 0) {
            found = npairs++;
            pairs[found] = (SalPair){ lo, hi, 0, false };
        }
        step_pair[k] = found;
    }
    if (ng > 1) {
        lp_id *cand_ids = palloc(sizeof(lp_id) * (size_t)(2 * (ng - 1)));
        int16 *cand_tiers = palloc(sizeof(int16) * (size_t)(2 * (ng - 1)));
        int *cand_step = palloc(sizeof(int) * (size_t)(2 * (ng - 1)));
        int ncand = 0;
        for (int k = 0; k < ng - 1; k++) {
            if (step_pair[k] < 0) continue;
            int pa = ng_pos[k], pb = ng_pos[k + 1];
            lp_id full_id;
            lp_id_compose(&occ[pa], (size_t)(pb - pa + 1), &full_id);
            cand_ids[ncand] = full_id;
            cand_step[ncand] = step_pair[k];
            ncand++;
            if (pb - pa > 1) {
                lp_id pair_id;
                lp_id two[2] = { occ[pa], occ[pb] };
                lp_id_compose(two, 2, &pair_id);
                cand_ids[ncand] = pair_id;
                cand_step[ncand] = step_pair[k];
                ncand++;
            }
        }
        if (ncand > 0) {
            lp_pg_lookup_entity_tiers(cand_ids, (size_t)ncand, cand_tiers);
            for (int c = 0; c < ncand; c++)
                if (cand_tiers[c] >= 0) pairs[cand_step[c]].stored = true;
        }
        pfree(cand_step); pfree(cand_tiers); pfree(cand_ids);
    }

    int64 *df = palloc0(sizeof(int64) * (size_t)nu);
    int64 *l_df = palloc0(sizeof(int64) * (size_t)nu);
    int *u_to_l = palloc(sizeof(int) * (size_t)nu);
    for (int u = 0; u < nu; u++) u_to_l[u] = -1;
    lp_id *l_ids = palloc(sizeof(lp_id) * (size_t)nu);
    lp_idmap *lmap = lp_idmap_sized(sizeof(int));
    int nl = 0;
    {
        lp_id *t1_ids = palloc(sizeof(lp_id) * (size_t)nu);
        int nt1 = 0;
        for (int u = 0; u < nu; u++) {
            if (!u_glue[u] && u_tiers[u] == 1) t1_ids[nt1++] = u_ids[u];
        }
        if (nt1 > 0) {
            SalLowerCtx lctx = { t0, umap, u_ids, u_to_l, lmap, l_ids, 0 };
            lp_pg_lookup_paths(t1_ids, (size_t)nt1, false, sal_t1_path_visit, &lctx);
            nl = lctx.nl;
        }
        pfree(t1_ids);
    }
    int64 total = 0;
    double *info_bits = palloc0(sizeof(double) * (size_t)nu);
    double *loss_c = palloc0(sizeof(double) * (size_t)nu);

    Oid phys_oid = lp_pg_relation("physicality");
    Relation phys_parent = table_open(phys_oid, AccessShareLock);
    Oid element = get_atttype(phys_oid, get_attnum(phys_oid, "entity"));
    Oid array_type = get_array_type(element);

    const Oid *canon_parts = NULL, *canon_pks = NULL;
    const int16 *min_t = NULL, *max_t = NULL;
    int n_canon = lp_pg_canonical_partitions("physicality", &canon_parts, &canon_pks, NULL, NULL);
    (void)lp_pg_canonical_tier_bounds("physicality", &min_t, &max_t);
    int nparts = n_canon > 0 ? n_canon : 1;

    static Oid cached_sal_parent = InvalidOid, cached_sal_idx[256];
    static int cached_sal_col[256];
    static uint64 cached_sal_gen = 0;
    if (cached_sal_gen != lp_pg_binding_generation()) {
        memset(cached_sal_idx, 0, sizeof(cached_sal_idx));
        memset(cached_sal_col, 0, sizeof(cached_sal_col));
        cached_sal_parent = InvalidOid;
        cached_sal_gen = lp_pg_binding_generation();
    }
    if (cached_sal_parent != phys_oid) {
        memset(cached_sal_idx, 0, sizeof(cached_sal_idx));
        memset(cached_sal_col, 0, sizeof(cached_sal_col));
        cached_sal_parent = phys_oid;
    }

    Snapshot snap = GetActiveSnapshot();
    Size bm_mem = Min((Size)work_mem * 1024, (Size)4 * 1024 * 1024);
    TIDBitmap **part_bms = palloc0(sizeof(TIDBitmap *) * (size_t)nu);
    bool *part_high_df = palloc0(sizeof(bool) * (size_t)nu);
    int64 *part_est_df = palloc0(sizeof(int64) * (size_t)nu);

    PG_TRY(); {
        for (int p = 0; p < nparts; p++) {
            CHECK_FOR_INTERRUPTS();
            Oid part_oid = n_canon > 0 ? canon_parts[p] : phys_oid;
            Oid pk_oid = (n_canon > 0 && canon_pks) ? canon_pks[p] : InvalidOid;
            if (n_canon > 0 && min_t && max_t && min_active_tier >= 0 && max_t[p] <= min_active_tier)
                continue;

            Relation table = table_open(part_oid, AccessShareLock);
            if (table->rd_rel->relkind == RELKIND_PARTITIONED_TABLE || RelationGetNumberOfBlocks(table) == 0) {
                table_close(table, NoLock);
                continue;
            }
            AttrNumber path_att = get_attnum(part_oid, "path");
            int index_col = 0;
            Oid operation = InvalidOid;
            Oid *slot_id = (p < 256) ? &cached_sal_idx[p] : NULL;
            int *slot_col = (p < 256) ? &cached_sal_col[p] : NULL;
            Relation index = container_index(table, path_att, array_type, 7, NIL, &index_col, &operation, slot_id, slot_col);

            bool has_pending = false;
            int64 pending_tups = 0;
            if (RelationGetNumberOfBlocks(index) > GIN_METAPAGE_BLKNO) {
                Buffer mbuf = ReadBuffer(index, GIN_METAPAGE_BLKNO);
                LockBuffer(mbuf, GIN_SHARE);
                GinMetaPageData *meta = GinPageGetMeta(BufferGetPage(mbuf));
                has_pending = (meta->head != InvalidBlockNumber);
                if (meta->nPendingHeapTuples > 0) pending_tups = meta->nPendingHeapTuples;
                UnlockReleaseBuffer(mbuf);
            }

            int64 p_rows = sal_part_rows(table, part_oid, pk_oid, pending_tups, snap);
            total += p_rows;

            memset(part_high_df, 0, sizeof(bool) * (size_t)nu);
            memset(part_est_df, 0, sizeof(int64) * (size_t)nu);
            GinState ginstate;
            initGinState(&ginstate, index);

            for (int u = 0; u < nu; u++) {
                if (n_canon > 0 && max_t && u_tiers[u] >= 0 && max_t[p] <= u_tiers[u]) continue;
                Datum kdatum = sal_path_member_key(&u_ids[u]);
                GinBtreeData btreeEntry;
                ginPrepareEntryScan(&btreeEntry, (OffsetNumber)index_col, kdatum, GIN_CAT_NORM_KEY, &ginstate);
                GinBtreeStack *stackEntry = ginFindLeafPage(&btreeEntry, true, false);
                Page page = BufferGetPage(stackEntry->buffer);
                bool needUnlock = true;

                if (btreeEntry.findItem(&btreeEntry, stackEntry)) {
                    IndexTuple itup = (IndexTuple)PageGetItem(page, PageGetItemId(page, stackEntry->off));
                    if (GinIsPostingTree(itup)) {
                        if (u_glue[u]) {
                            part_est_df[u] += Max((int64)512, p_rows / 4);
                            part_high_df[u] = true;
                        } else {
                            BlockNumber rootBlk = GinGetPostingTree(itup);
                            LockBuffer(stackEntry->buffer, GIN_UNLOCK);
                            needUnlock = false;

                            GinBtreeData btreeData;
                            GinBtreeStack *stack = ginScanBeginPostingTree(&btreeData, index, rootBlk);
                            uint32 pred_pages = stack->predictNumber > 0 ? stack->predictNumber : 1;
                            Page lpage = BufferGetPage(stack->buffer);
                            ItemPointerData minItem;
                            ItemPointerSetMin(&minItem);
                            int nfirst = 0;
                            ItemPointer firstItems = NULL;
                            if (!GinPageIsDeleted(lpage))
                                firstItems = GinDataLeafPageGetItems(lpage, &nfirst, minItem);

                            if (pred_pages > 8 || (int64)pred_pages * nfirst > 4096) {
                                part_est_df[u] += (int64)pred_pages * (int64)(nfirst > 0 ? nfirst : 512);
                                part_high_df[u] = true;
                                if (firstItems) pfree(firstItems);
                                LockBuffer(stack->buffer, GIN_UNLOCK);
                                freeGinBtreeStack(stack);
                            } else {
                                Buffer lbuf = stack->buffer;
                                IncrBufferRefCount(lbuf);
                                freeGinBtreeStack(stack);
                                part_bms[u] = tbm_create(bm_mem, NULL);
                                if (firstItems) {
                                    if (nfirst > 0) tbm_add_tuples(part_bms[u], firstItems, nfirst, false);
                                    pfree(firstItems);
                                }
                                uint32 walked = 1;
                                int64 seen_items = nfirst;
                                while (!GinPageRightMost(lpage) && walked < 16) {
                                    lbuf = ginStepRight(lbuf, index, GIN_SHARE);
                                    lpage = BufferGetPage(lbuf);
                                    walked++;
                                    if (!GinPageIsDeleted(lpage)) {
                                        int nitems = 0;
                                        ItemPointer items = GinDataLeafPageGetItems(lpage, &nitems, minItem);
                                        if (items) {
                                            if (nitems > 0) {
                                                tbm_add_tuples(part_bms[u], items, nitems, false);
                                                seen_items += nitems;
                                            }
                                            pfree(items);
                                        }
                                    }
                                }
                                bool more = !GinPageRightMost(lpage);
                                UnlockReleaseBuffer(lbuf);
                                if (more) {
                                    int64 avg = walked > 0 ? (seen_items / walked) : 512;
                                    part_est_df[u] += Max(seen_items, (int64)Max(pred_pages, walked) * Max(avg, (int64)1));
                                    tbm_free(part_bms[u]);
                                    part_bms[u] = NULL;
                                    part_high_df[u] = true;
                                }
                            }
                        }
                    } else if (GinGetNPosting(itup) > 0) {
                        if (u_glue[u]) {
                            part_est_df[u] += GinGetNPosting(itup);
                        } else {
                            int nlist = 0;
                            ItemPointer items = ginReadTuple(&ginstate, (OffsetNumber)index_col, itup, &nlist);
                            if (items) {
                                if (nlist > 0) {
                                    part_bms[u] = tbm_create(bm_mem, NULL);
                                    tbm_add_tuples(part_bms[u], items, nlist, false);
                                }
                                pfree(items);
                            }
                        }
                    }
                }
                if (needUnlock) LockBuffer(stackEntry->buffer, GIN_UNLOCK);
                freeGinBtreeStack(stackEntry);
                pfree(DatumGetPointer(kdatum));
            }

            /* Fast count-only GIN estimation for lowercase case-variants (tier 1 words). */
            for (int l = 0; l < nl; l++) {
                if (n_canon > 0 && max_t && max_t[p] <= 1) break;
                Datum kdatum = sal_path_member_key(&l_ids[l]);
                GinBtreeData btreeEntry;
                ginPrepareEntryScan(&btreeEntry, (OffsetNumber)index_col, kdatum, GIN_CAT_NORM_KEY, &ginstate);
                GinBtreeStack *stackEntry = ginFindLeafPage(&btreeEntry, true, false);
                Page page = BufferGetPage(stackEntry->buffer);
                bool needUnlock = true;
                if (btreeEntry.findItem(&btreeEntry, stackEntry)) {
                    IndexTuple itup = (IndexTuple)PageGetItem(page, PageGetItemId(page, stackEntry->off));
                    if (GinIsPostingTree(itup)) {
                        BlockNumber rootBlk = GinGetPostingTree(itup);
                        LockBuffer(stackEntry->buffer, GIN_UNLOCK);
                        needUnlock = false;
                        GinBtreeData btreeData;
                        GinBtreeStack *stack = ginScanBeginPostingTree(&btreeData, index, rootBlk);
                        uint32 pred_pages = stack->predictNumber > 0 ? stack->predictNumber : 1;
                        Page lpage = BufferGetPage(stack->buffer);
                        ItemPointerData minItem;
                        ItemPointerSetMin(&minItem);
                        int nfirst = 0;
                        ItemPointer firstItems = NULL;
                        if (!GinPageIsDeleted(lpage))
                            firstItems = GinDataLeafPageGetItems(lpage, &nfirst, minItem);
                        if (pred_pages > 8 || (int64)pred_pages * nfirst > 4096) {
                            l_df[l] += (int64)pred_pages * (int64)(nfirst > 0 ? nfirst : 512);
                            if (firstItems) pfree(firstItems);
                            LockBuffer(stack->buffer, GIN_UNLOCK);
                            freeGinBtreeStack(stack);
                        } else {
                            Buffer lbuf = stack->buffer;
                            IncrBufferRefCount(lbuf);
                            freeGinBtreeStack(stack);
                            int64 seen_items = nfirst;
                            if (firstItems) pfree(firstItems);
                            uint32 walked = 1;
                            while (!GinPageRightMost(lpage) && walked < 16) {
                                lbuf = ginStepRight(lbuf, index, GIN_SHARE);
                                lpage = BufferGetPage(lbuf);
                                walked++;
                                if (!GinPageIsDeleted(lpage)) {
                                    int nitems = 0;
                                    ItemPointer items = GinDataLeafPageGetItems(lpage, &nitems, minItem);
                                    if (items) { seen_items += nitems; pfree(items); }
                                }
                            }
                            bool more = !GinPageRightMost(lpage);
                            UnlockReleaseBuffer(lbuf);
                            if (more) {
                                int64 avg = walked > 0 ? (seen_items / walked) : 512;
                                l_df[l] += Max(seen_items, (int64)Max(pred_pages, walked) * Max(avg, (int64)1));
                            } else {
                                l_df[l] += seen_items;
                            }
                        }
                    } else if (GinGetNPosting(itup) > 0) {
                        l_df[l] += GinGetNPosting(itup);
                    }
                }
                if (needUnlock) LockBuffer(stackEntry->buffer, GIN_UNLOCK);
                freeGinBtreeStack(stackEntry);
                pfree(DatumGetPointer(kdatum));
            }

            if (has_pending) {
                bool need_pending = false;
                for (int u = 0; u < nu; u++) {
                    if (!(u_glue[u] || part_high_df[u]) && !(n_canon > 0 && max_t && u_tiers[u] >= 0 && max_t[p] <= u_tiers[u])) {
                        need_pending = true;
                        break;
                    }
                }
                if (need_pending) {
                    Buffer mbuf = ReadBuffer(index, GIN_METAPAGE_BLKNO);
                    LockBuffer(mbuf, GIN_SHARE);
                    BlockNumber pblk = GinPageGetMeta(BufferGetPage(mbuf))->head;
                    if (pblk == InvalidBlockNumber) {
                        UnlockReleaseBuffer(mbuf);
                    } else {
                        Buffer pbuf = ReadBuffer(index, pblk);
                        LockBuffer(pbuf, GIN_SHARE);
                        UnlockReleaseBuffer(mbuf);
                        for (;;) {
                            CHECK_FOR_INTERRUPTS();
                            Page ppage = BufferGetPage(pbuf);
                            if (!GinPageIsDeleted(ppage)) {
                                OffsetNumber maxoff = PageGetMaxOffsetNumber(ppage);
                                for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++) {
                                    IndexTuple itup = (IndexTuple)PageGetItem(ppage, PageGetItemId(ppage, off));
                                    if (gintuple_get_attrnum(&ginstate, itup) != (OffsetNumber)index_col) continue;
                                    GinNullCategory cat = GIN_CAT_NORM_KEY;
                                    Datum kd = gintuple_get_key(&ginstate, itup, &cat);
                                    if (cat != GIN_CAT_NORM_KEY || VARSIZE_ANY_EXHDR(kd) != 17) continue;
                                    const uint8 *kb = (const uint8 *)VARDATA_ANY(kd);
                                    if (kb[0] != 0) continue;
                                    int64 l_at = nl > 0 ? lp_idmap_find(lmap, (const lp_id *)(kb + 1)) : -1;
                                    if (l_at >= 0) l_df[*(int *)lp_idmap_at(lmap, (size_t)l_at)]++;
                                    int64 u_at = lp_idmap_find(umap, (const lp_id *)(kb + 1));
                                    if (u_at < 0) continue;
                                    int u = *(int *)lp_idmap_at(umap, (size_t)u_at);
                                    if (u_glue[u] || part_high_df[u] ||
                                        (n_canon > 0 && max_t && u_tiers[u] >= 0 && max_t[p] <= u_tiers[u]))
                                        continue;
                                    if (!part_bms[u]) part_bms[u] = tbm_create(bm_mem, NULL);
                                    tbm_add_tuples(part_bms[u], &itup->t_tid, 1, false);
                                }
                            }
                            BlockNumber next_blk = GinPageGetOpaque(ppage)->rightlink;
                            if (next_blk == InvalidBlockNumber) {
                                UnlockReleaseBuffer(pbuf);
                                break;
                            }
                            Buffer nbuf = ReadBuffer(index, next_blk);
                            LockBuffer(nbuf, GIN_SHARE);
                            UnlockReleaseBuffer(pbuf);
                            pbuf = nbuf;
                        }
                    }
                }
            }

            index_close(index, NoLock);
            table_close(table, NoLock);

            for (int u = 0; u < nu; u++) {
                int64 u_p_df = part_est_df[u] + (part_bms[u] ? sal_tbm_count_copy(part_bms[u], bm_mem) : 0);
                part_est_df[u] = u_p_df;
                df[u] += u_p_df;
            }
            for (int k = 0; k < npairs; k++) {
                int pa = pairs[k].ua, pb = pairs[k].ub;
                if (!part_high_df[pa] && !part_high_df[pb]) {
                    if (part_bms[pa] && part_bms[pb])
                        pairs[k].co += sal_tbm_intersect_count(part_bms[pa], part_bms[pb], bm_mem);
                } else if (part_est_df[pa] > 0 && part_est_df[pb] > 0 && p_rows > 0) {
                    int64 denom = Max(p_rows, Max(part_est_df[pa], part_est_df[pb]));
                    int64 exp_co = (int64)llround(((double)part_est_df[pa] * (double)part_est_df[pb]) / (double)denom);
                    pairs[k].co += Max((int64)0, Min(exp_co, Min(part_est_df[pa], part_est_df[pb])));
                }
            }
            for (int u = 0; u < nu; u++) {
                if (part_bms[u]) { tbm_free(part_bms[u]); part_bms[u] = NULL; }
            }
        }
    } PG_FINALLY(); {
        for (int u = 0; u < nu; u++) {
            if (part_bms[u]) { tbm_free(part_bms[u]); part_bms[u] = NULL; }
        }
    } PG_END_TRY();
    pfree(part_est_df);
    pfree(part_high_df);
    pfree(part_bms);

    int64 *eff_df = palloc(sizeof(int64) * (size_t)nu);
    double *eff_info = palloc(sizeof(double) * (size_t)nu);
    for (int u = 0; u < nu; u++) {
        int64 ldf = (u_to_l[u] >= 0) ? l_df[u_to_l[u]] : 0;
        eff_df[u] = (ldf > df[u]) ? (df[u] + ldf) : df[u];
        if (total < eff_df[u]) total = eff_df[u];
    }
    for (int u = 0; u < nu; u++) {
        info_bits[u] = -log2((double)(df[u] + 1) / (double)(total + 1));
        eff_info[u] = -log2((double)(eff_df[u] + 1) / (double)(total + 1));
    }

    /* Loss contrast over bounded claim & physicality_assertions holders + attestation fallback. */
    if (ng > 0 && (!scope || nscope > 0)) {
        const char *selected[] = { "entity", "path", "mask" }, *path_col = NULL;
        Relation loss_parent = lp_pg_read_parent(phys_oid, &path_col, selected, lengthof(selected));
        AttrNumber id_att = get_attnum(phys_oid, "entity");
        AttrNumber p_att = get_attnum(phys_oid, "path");
        AttrNumber m_att = get_attnum(phys_oid, "mask");
        MemoryContext hctx = AllocSetContextCreate(CurrentMemoryContext, "SalienceLoss", ALLOCSET_DEFAULT_SIZES);
        lp_idmap *wanted_c = lp_idmap_sized(sizeof(HolderKey));
        lp_idmap *wanted_a = lp_idmap_sized(sizeof(HolderKey));
        lp_idmap *holders = lp_idmap_sized(sizeof(SalHolder));
        lp_idmap *scope_map = nscope > 0 ? lp_idmap_new() : NULL;
        double *win_w = palloc0(sizeof(double) * (size_t)nu);
        double *loss_w = palloc0(sizeof(double) * (size_t)nu);

        PG_TRY(); {
            for (int u = 0; u < nu; u++) {
                if (u_glue[u] || df[u] <= 0 || df[u] > 4096) continue;
                bool fr;
                HolderKey *kc = lp_idmap_get(wanted_c, &u_ids[u], &fr);
                if (fr) memset(kc, 0, sizeof(*kc));
                HolderKey *ka = lp_idmap_get(wanted_a, &u_ids[u], &fr);
                if (fr) memset(ka, 0, sizeof(*ka));
            }
            if (scope_map) for (int s = 0; s < nscope; s++) lp_idmap_put(scope_map, &scope[s], NULL);

            if (lp_idmap_count(wanted_c) > 0) {
                uint64 nom_cap = (UINT64_C(2) << 60) | 64;
                SalLossCollect lc = { wanted_c, holders, UINT64_C(1), id_att, p_att, m_att, hctx };
                scan_container_rows(loss_parent, path_col, wanted_c, 3, NULL, false, 0, NULL, UINT64_C(1), false, nom_cap, min_active_tier, sal_loss_visit, &lc);

                lc.wanted = wanted_a;
                lc.bits = 0;
                scan_container_rows(loss_parent, path_col, wanted_a, 3, NULL, false, 0, NULL, 0, true, nom_cap, min_active_tier, sal_loss_visit, &lc);
            }

            size_t nh = lp_idmap_count(holders);
            if (nh > 0) {
                bool *h_allow = palloc(sizeof(bool) * nh);
                if (nscope > 0) {
                    lp_id *h_ids = palloc(sizeof(lp_id) * nh);
                    for (size_t h = 0; h < nh; h++) h_ids[h] = ((SalHolder *)lp_idmap_at(holders, h))->entity;
                    lp_pg_filter_witness_entities(h_ids, nh, scope, nscope, h_allow);
                    pfree(h_ids);
                } else {
                    for (size_t h = 0; h < nh; h++) h_allow[h] = true;
                }

                lp_id *all_claims = palloc(sizeof(lp_id) * nh);
                int n_all = 0;
                lp_idmap *h_claim_map = lp_idmap_sized(sizeof(size_t));
                for (size_t h = 0; h < nh; h++) {
                    if (!h_allow[h]) continue;
                    SalHolder *sh = lp_idmap_at(holders, h);
                    all_claims[n_all++] = sh->entity;
                    size_t at = lp_idmap_put(h_claim_map, &sh->entity, NULL);
                    *(size_t *)lp_idmap_at(h_claim_map, at) = h;
                }

                double *h_trust = palloc0(sizeof(double) * nh);
                double *h_att_win = palloc0(sizeof(double) * nh);
                double *h_att_loss = palloc0(sizeof(double) * nh);
                if (n_all > 0) {
                    PgAttrRows att_rows = { 0 };
                    lp_pg_read_attestation_pk(all_claims, n_all, scope_map, &att_rows, NULL);
                    for (size_t r = 0; r < att_rows.n; r++) {
                        PgAttrRow *ar = &att_rows.v[r];
                        int64 hm = lp_idmap_find(h_claim_map, &ar->claim);
                        if (hm < 0) continue;
                        size_t h = *(size_t *)lp_idmap_at(h_claim_map, (size_t)hm);
                        double mean_s = (ar->games > 0.0) ? (ar->score / ar->games) : 0.5;
                        if (mean_s < 0.0) mean_s = 0.0;
                        if (mean_s > 1.0) mean_s = 1.0;
                        double w = ar->trust > 0.0 ? ar->trust : 1.0;
                        h_trust[h] += w;
                        h_att_win[h] += w * mean_s;
                        h_att_loss[h] += w * (1.0 - mean_s);
                    }
                    lp_vec_free(&att_rows);
                }

                for (size_t h = 0; h < nh; h++) {
                    if (!h_allow[h]) continue;
                    SalHolder *sh = lp_idmap_at(holders, h);
                    Geo g = geo_of(sh->path);
                    lp_vertex stack_v[64];
                    lp_vertex *verts = g.p.n <= 64 ? stack_v : palloc(sizeof(lp_vertex) * g.p.n);
                    size_t nv = lp_path_decode(g.p, verts, g.p.n);

                    double p_sum = 0.0, p_cnt = 0.0;
                    for (size_t v = 0; v < nv; v++) {
                        if (verts[v].said == LP_SAID_RECORD || verts[v].said == LP_SAID_CLAIM || verts[v].outcome != 0) {
                            double sc = lp_outcome_score(verts[v].outcome, verts[v].spare);
                            double run = verts[v].run ? (double)verts[v].run : 1.0;
                            p_sum += sc * run;
                            p_cnt += run;
                        }
                    }

                    bool seen_u_stack[64];
                    bool *seen_u = nu <= 64 ? seen_u_stack : palloc0(sizeof(bool) * (size_t)nu);
                    if (nu <= 64) memset(seen_u, 0, sizeof(bool) * (size_t)nu);

                    if (p_cnt > 0.0) {
                        double p_mean = p_sum / p_cnt;
                        double hw = h_trust[h] > 0.0 ? h_trust[h] : 1.0;
                        for (size_t v = 0; v < nv; v++) {
                            int64 u_at = lp_idmap_find(umap, &verts[v].id);
                            if (u_at < 0) continue;
                            int u = *(int *)lp_idmap_at(umap, (size_t)u_at);
                            if (u_glue[u] || seen_u[u]) continue;
                            seen_u[u] = true;
                            double u_score = p_mean;
                            if (verts[v].said == LP_SAID_RECORD || verts[v].said == LP_SAID_CLAIM || verts[v].outcome != 0)
                                u_score = lp_outcome_score(verts[v].outcome, verts[v].spare);
                            if (u_score < 0.0) u_score = 0.0;
                            if (u_score > 1.0) u_score = 1.0;
                            win_w[u] += hw * u_score;
                            loss_w[u] += hw * (1.0 - u_score);
                        }
                    } else if (h_att_win[h] + h_att_loss[h] > 0.0) {
                        for (size_t v = 0; v < nv; v++) {
                            int64 u_at = lp_idmap_find(umap, &verts[v].id);
                            if (u_at < 0) continue;
                            int u = *(int *)lp_idmap_at(umap, (size_t)u_at);
                            if (u_glue[u] || seen_u[u]) continue;
                            seen_u[u] = true;
                            win_w[u] += h_att_win[h];
                            loss_w[u] += h_att_loss[h];
                        }
                    }

                    if (seen_u != seen_u_stack) pfree(seen_u);
                    if (verts != stack_v) pfree(verts);
                }

                lp_idmap_free(h_claim_map);
                pfree(all_claims);
                pfree(h_trust);
                pfree(h_att_win);
                pfree(h_att_loss);
                pfree(h_allow);
            }

            for (int u = 0; u < nu; u++) {
                double d = win_w[u] + loss_w[u];
                loss_c[u] = d > 0.0 ? (loss_w[u] - win_w[u]) / (d + 1.0) : 0.0;
            }
        } PG_FINALLY(); {
            pfree(win_w); pfree(loss_w);
            if (scope_map) lp_idmap_free(scope_map);
            lp_idmap_free(holders);
            lp_idmap_free(wanted_a);
            lp_idmap_free(wanted_c);
            MemoryContextDelete(hctx);
            table_close(loss_parent, NoLock);
        } PG_END_TRY();
    }

    table_close(phys_parent, NoLock);

    /* Span lift (local span contraction & PMI) and normalized salience. */
    double mean_info = 0.0;
    if (ng > 0) {
        for (int k = 0; k < ng; k++) mean_info += eff_info[occ_to_u[ng_pos[k]]];
        mean_info /= (double)ng;
    }

    double *ng_lift_sum = palloc0(sizeof(double) * (size_t)(ng ? ng : 1));
    int *ng_lift_cnt = palloc0(sizeof(int) * (size_t)(ng ? ng : 1));
    double scale_denom = Max(16.0, 0.01 * (double)total);

    for (int k = 0; k < ng - 1; k++) {
        int pidx = step_pair[k];
        if (pidx < 0) continue;
        int ua = occ_to_u[ng_pos[k]], ub = occ_to_u[ng_pos[k + 1]];
        int64 co = pairs[pidx].co;
        double la = 0.0, lb = 0.0;
        if (co > 0 || pairs[pidx].stored) {
            double i_co = -log2((double)(co + 1) / (double)(total + 1));
            double pmi = eff_info[ua] + eff_info[ub] - i_co;
            double pmi_lift = pmi - log2(1.0 + (double)co / scale_denom);
            int64 min_df = Min(eff_df[ua], eff_df[ub]);
            double max_cond = (double)(co + 1) / (double)(min_df + 1);
            double base_lift = (max_cond < 0.5)
                ? log2(2.0 * max_cond)
                : (pmi_lift < 0.0 ? pmi_lift : pmi_lift * max_cond);
            if (pairs[pidx].stored) base_lift += 1.0;

            double sa = 1.0 / (double)(eff_df[ua] + 1), sb = 1.0 / (double)(eff_df[ub] + 1);
            double sh_a = sa / (sa + sb), sh_b = sb / (sa + sb);
            la = (base_lift >= 0.0 ? base_lift * sh_a : base_lift)
               + 0.5 * (eff_info[ua] - eff_info[ub]);
            lb = (base_lift >= 0.0 ? base_lift * sh_b : base_lift)
               + 0.5 * (eff_info[ub] - eff_info[ua]);
            if (base_lift < 0.0) {
                double cap_neg = 0.25 * base_lift;
                if (la > cap_neg) la = cap_neg;
                if (lb > cap_neg) lb = cap_neg;
            }
        }
        ng_lift_sum[k] += la; ng_lift_cnt[k]++;
        ng_lift_sum[k + 1] += lb; ng_lift_cnt[k + 1]++;
    }

    for (int i = 0; i < nocc; i++) {
        int u = occ_to_u[i];
        out[i].df = df[u];
        out[i].total = total;
        out[i].info_bits = info_bits[u];
        out[i].loss_contrast = loss_c[u];
        out[i].span_lift = is_glue[i] ? -1.0 : 0.0;
    }
    for (int k = 0; k < ng; k++) {
        int pos = ng_pos[k];
        int u = occ_to_u[pos];
        if (ng >= 2 && ng_lift_cnt[k] > 0) {
            double avg_nb = ng_lift_sum[k] / (double)ng_lift_cnt[k];
            out[pos].span_lift = 0.75 * avg_nb + 0.25 * (eff_info[u] - mean_info);
        } else {
            out[pos].span_lift = 0.0;
        }
    }

    double *raw = palloc0(sizeof(double) * (size_t)nocc);
    double raw_sum = 0.0;
    for (int i = 0; i < nocc; i++) {
        if (is_glue[i]) { out[i].salience = 0.0; continue; }
        int u = occ_to_u[i];
        double inv_df = 1.0 / sqrt((double)(eff_df[u] + 1));
        if (u_tiers[u] < 0 && df[u] == 0) inv_df *= 0.25;
        double clamped_lift = out[i].span_lift < -4.0 ? -4.0 : (out[i].span_lift > 4.0 ? 4.0 : out[i].span_lift);
        double lift_f = exp(0.25 * clamped_lift);
        double loss_f = 1.0 + (loss_c[u] > 0.0 ? loss_c[u] : 0.0);
        raw[i] = inv_df * lift_f * loss_f;
        raw_sum += raw[i];
    }
    if (raw_sum > 0.0) {
        for (int i = 0; i < nocc; i++) out[i].salience = raw[i] / raw_sum;
    }

    pfree(raw); pfree(ng_lift_cnt); pfree(ng_lift_sum);
    pfree(eff_info); pfree(eff_df);
    pfree(l_df); pfree(u_to_l); pfree(l_ids); lp_idmap_free(lmap);
    pfree(loss_c); pfree(info_bits); pfree(df);
    pfree(step_pair); pfree(pairs); pfree(ng_pos);
    pfree(u_glue); pfree(u_tiers);
    lp_idmap_free(umap);
    pfree(occ_to_u); pfree(is_glue); pfree(occ_tiers);
}

PG_FUNCTION_INFO_V1(laplace_salience);
Datum laplace_salience(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_salience", &td);
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    int nocc = 0;
    lp_id *occ = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nocc);
    if (nocc <= 0) { pfree(occ); PG_RETURN_NULL(); }

    lp_id *scope = NULL;
    int nscope = 0;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        scope = ids_of(PG_GETARG_ARRAYTYPE_P(1), &nscope);

    PgSalience *sal = palloc(sizeof(PgSalience) * (size_t)nocc);
    lp_pg_salience(occ, nocc, scope, nscope, sal);

    for (int i = 0; i < nocc; i++) {
        Datum v[9] = {
            Int32GetDatum(i + 1),
            id_datum(&occ[i]),
            Int16GetDatum(sal[i].tier),
            Int64GetDatum(sal[i].df),
            Int64GetDatum(sal[i].total),
            Float8GetDatum(sal[i].info_bits),
            Float8GetDatum(sal[i].span_lift),
            Float8GetDatum(sal[i].loss_contrast),
            Float8GetDatum(sal[i].salience)
        };
        bool nl[9] = { false, false, false, false, false, false, false, false, false };
        set_put(fcinfo, ts, td, v, nl);
        pfree(DatumGetPointer(v[1]));
    }
    pfree(sal);
    if (scope) pfree(scope);
    pfree(occ);
    PG_RETURN_NULL();
}
