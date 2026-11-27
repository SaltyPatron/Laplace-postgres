/* Partition-routed B-tree primary index kernel and ID-addressed frontier reads
 * (entity, physicality, consensus, witness, and presence membership). */
#include "pg_core.h"
#include "geo.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/tupconvert.h"
#include "utils/fmgroids.h"
#include "catalog/namespace.h"
#include "laplace/consensus.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "partitioning/partdesc.h"
#include "partitioning/partbounds.h"

typedef struct {
    const char *name;
    Oid parent;
    int nparts;
    bool by_tier;
    int16 min_tier[256];
    int16 max_tier[256];
    Oid parts[256];
    Oid pks[256];
    Oid aux[256];
    Oid aux2[256];
} CanonicalPartDir;

static CanonicalPartDir part_dirs[4] = {
    {.name = "entity"}, {.name = "physicality"}, {.name = "attestation"}, {.name = "consensus"}
};
static uint64 part_dir_gen;

static Oid find_unique_col_btree(Relation part, AttrNumber target_col)
{
    if (target_col == InvalidAttrNumber) return InvalidOid;
    List *idxs = RelationGetIndexList(part);
    ListCell *lc;
    Oid found = InvalidOid;
    foreach(lc, idxs) {
        Oid ioid = lfirst_oid(lc);
        Relation idx = index_open(ioid, AccessShareLock);
        if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index &&
            idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive &&
            idx->rd_index->indisunique && idx->rd_index->indnkeyatts >= 1 &&
            idx->rd_index->indkey.values[0] == target_col) {
            found = ioid;
            bool is_pk = idx->rd_index->indisprimary;
            index_close(idx, NoLock);
            if (is_pk) break;
            continue;
        }
        index_close(idx, NoLock);
    }
    list_free(idxs);
    return found;
}

static void refresh_part_dir(CanonicalPartDir *dir)
{
    Oid target = lp_pg_relation(dir->name);
    if (dir->parent == target && dir->nparts > 0) return;
    memset(dir->parts, 0, sizeof(dir->parts));
    memset(dir->pks, 0, sizeof(dir->pks));
    memset(dir->aux, 0, sizeof(dir->aux));
    memset(dir->aux2, 0, sizeof(dir->aux2));
    Relation parent = table_open(target, AccessShareLock);
    PartitionKey pk = RelationGetPartitionKey(parent);
    PartitionDesc pd = parent->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ? RelationGetPartitionDesc(parent, true) : NULL;
    int n = pd ? pd->nparts : 1;
    if (n > 256) ereport(ERROR, (errmsg("Native partition directory: %s has %d partitions (> 256)", dir->name, n)));
    bool is_entity = strcmp(dir->name, "entity") == 0;
    bool is_physicality = strcmp(dir->name, "physicality") == 0;
    bool is_attestation = strcmp(dir->name, "attestation") == 0;
    AttrNumber a_tier = get_attnum(target, "tier");
    AttrNumber unique_col = is_entity ? get_attnum(target, "id") : is_physicality ? get_attnum(target, "entity") : InvalidAttrNumber;
    dir->by_tier = (pk && pd && pk->strategy == PARTITION_STRATEGY_RANGE && pk->partnatts == 1 &&
                    a_tier != InvalidAttrNumber && pk->partattrs[0] == a_tier);
    for (int i = 0; i < n; i++) {
        dir->min_tier[i] = PG_INT16_MIN;
        dir->max_tier[i] = PG_INT16_MAX;
    }
    if (dir->by_tier && pd->boundinfo && pd->boundinfo->ndatums >= 2) {
        PartitionBoundInfo bi = pd->boundinfo;
        for (int b = 0; b + 1 < bi->ndatums; b++) {
            int pi = bi->indexes[b + 1];
            if (pi < 0 || pi >= n) continue;
            int16 lo = (bi->kind[b][0] == PARTITION_RANGE_DATUM_MINVALUE)
                ? PG_INT16_MIN : DatumGetInt16(bi->datums[b][0]);
            int16 hi = (bi->kind[b + 1][0] == PARTITION_RANGE_DATUM_MAXVALUE)
                ? PG_INT16_MAX : (int16)(DatumGetInt16(bi->datums[b + 1][0]) - 1);
            dir->min_tier[pi] = lo;
            dir->max_tier[pi] = hi;
        }
    }
    for (int i = 0; i < n; i++) {
        Oid poid = pd ? pd->oids[i] : target;
        dir->parts[i] = poid;
        Relation part = table_open(poid, AccessShareLock);
        if (is_entity || is_physicality) {
            AttrNumber part_col = get_attnum(poid, is_entity ? "id" : "entity");
            Oid uoid = find_unique_col_btree(part, part_col);
            dir->pks[i] = OidIsValid(uoid) ? uoid : RelationGetPrimaryKeyIndex(part, false);
        } else {
            dir->pks[i] = RelationGetPrimaryKeyIndex(part, false);
        }
        (void)unique_col;
        if (is_entity || is_attestation) {
            List *idxs = RelationGetIndexList(part);
            ListCell *lc;
            AttrNumber pc = is_attestation ? get_attnum(poid, "claim") : InvalidAttrNumber;
            AttrNumber pw = is_attestation ? get_attnum(poid, "witness") : InvalidAttrNumber;
            AttrNumber pl = is_attestation ? get_attnum(poid, "lineage") : InvalidAttrNumber;
            foreach(lc, idxs) {
                Oid ioid = lfirst_oid(lc);
                Relation idx = index_open(ioid, AccessShareLock);
                if (idx->rd_index && idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive) {
                    if (is_entity && idx->rd_rel->relam == GIST_AM_OID && idx->rd_index->indnkeyatts >= 1) {
                        char *opfname = get_opfamily_name(idx->rd_opfamily[0], true);
                        if (opfname && strcmp(opfname, "laplace_point4d_ops") == 0 && !OidIsValid(dir->aux[i]))
                            dir->aux[i] = ioid;
                        else if (opfname && strcmp(opfname, "laplace_angular4d_ops") == 0 && !OidIsValid(dir->aux2[i]))
                            dir->aux2[i] = ioid;
                        if (opfname) pfree(opfname);
                    } else if (is_attestation && idx->rd_rel->relam == BTREE_AM_OID) {
                        if (!OidIsValid(dir->pks[i]) && idx->rd_index->indisunique &&
                            idx->rd_index->indnkeyatts >= 3 && pc != InvalidAttrNumber &&
                            pw != InvalidAttrNumber && pl != InvalidAttrNumber &&
                            idx->rd_index->indkey.values[0] == pc &&
                            idx->rd_index->indkey.values[1] == pw &&
                            idx->rd_index->indkey.values[2] == pl)
                            dir->pks[i] = ioid;
                        else if (!OidIsValid(dir->aux[i]) && idx->rd_index->indnkeyatts >= 2 &&
                                 pw != InvalidAttrNumber && pl != InvalidAttrNumber &&
                                 idx->rd_index->indkey.values[0] == pw &&
                                 idx->rd_index->indkey.values[1] == pl)
                            dir->aux[i] = ioid;
                    }
                }
                index_close(idx, NoLock);
            }
            list_free(idxs);
        }
        table_close(part, NoLock);
    }
    table_close(parent, NoLock);
    dir->parent = target;
    dir->nparts = n;
}

int lp_pg_canonical_partitions(const char *relname, const Oid **parts, const Oid **pks, const Oid **aux, const Oid **aux2)
{
    uint64 gen = lp_pg_binding_generation();
    if (part_dir_gen != gen) {
        for (size_t i = 0; i < lengthof(part_dirs); i++) { part_dirs[i].parent = InvalidOid; part_dirs[i].nparts = 0; }
        part_dir_gen = gen;
    }
    CanonicalPartDir *dir = NULL;
    for (size_t i = 0; i < lengthof(part_dirs); i++) if (strcmp(part_dirs[i].name, relname) == 0) { dir = &part_dirs[i]; break; }
    if (!dir) ereport(ERROR, (errmsg("Native partition directory: unknown canonical relation %s", relname)));
    refresh_part_dir(dir);
    if (parts) *parts = dir->parts;
    if (pks) *pks = dir->pks;
    if (aux) *aux = dir->aux;
    if (aux2) *aux2 = dir->aux2;
    return dir->nparts;
}

int lp_pg_canonical_tier_bounds(const char *relname, const int16 **min_tiers, const int16 **max_tiers)
{
    (void)lp_pg_canonical_partitions(relname, NULL, NULL, NULL, NULL);
    CanonicalPartDir *dir = NULL;
    for (size_t i = 0; i < lengthof(part_dirs); i++) if (strcmp(part_dirs[i].name, relname) == 0) { dir = &part_dirs[i]; break; }
    if (min_tiers) *min_tiers = dir->min_tier;
    if (max_tiers) *max_tiers = dir->max_tier;
    return dir->nparts;
}

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

static int tier_probe_priority(int16 lo, int16 hi, bool is_paths)
{
    if (lo <= 1 && hi >= 1) return is_paths ? 2 : 0;
    if (lo <= 3 && hi >= 3) return is_paths ? 0 : 1;
    if (lo <= 2 && hi >= 2) return is_paths ? 1 : 2;
    if (lo <= 4 && hi >= 4) return 3;
    if (lo <= 5 && hi >= 5) return 4;
    if (lo <= 6 && hi >= 6) return 5;
    if (lo <= 7 && hi >= 7) return 6;
    if (lo >= 8) return 7;
    return 8;
}

/* Shared indexed membership kernel for client frontiers and the final flat
 * overlap verification. Neither path reads coordinates to establish identity. */
static void indexed_keys_until(Relation parent,const char *column,PresenceKey *keys,size_t count,
                               NativeKeyVisit visit,void *context,const bool *done)
{
    if(!count)return;
    Oid target=RelationGetRelid(parent);
    AttrNumber attribute=get_attnum(target,column);
    AttrNumber tier_att=get_attnum(target,"tier");
    PartitionKey key=RelationGetPartitionKey(parent);
    bool by_tier=(key && key->strategy==PARTITION_STRATEGY_RANGE && key->partnatts==1 &&
                  tier_att!=InvalidAttrNumber && key->partattrs[0]==tier_att);
    if((key && !by_tier && (key->strategy!=PARTITION_STRATEGY_RANGE || key->partnatts!=1
        || key->partattrs[0]!=attribute)) || (!key && parent->rd_rel->relkind!=RELKIND_RELATION))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native presence requires canonical ID range partitions")));
    Oid native_id=get_atttype(target,attribute);
    size_t array_ids=(MaxAllocSize-ARR_OVERHEAD_NONULLS(1))/sizeof(lp_id);

    if(by_tier){
        lp_sort(keys,(size_t)count,sizeof(*keys),presence_order);
        typedef struct { size_t begin,end; } UniqueSpan;
        UniqueSpan *spans=palloc_extended(count*sizeof(*spans),MCXT_ALLOC_HUGE);
        lp_id *rem_ids=palloc_extended(count*sizeof(lp_id),MCXT_ALLOC_HUGE);
        size_t *rem_u=palloc_extended(count*sizeof(size_t),MCXT_ALLOC_HUGE);
        bool *matched=palloc_extended(count*sizeof(bool),MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
        size_t nu=0;
        for(size_t i=0;i<count;){
            size_t j=i+1;
            while(j<count && lp_id_eq(&keys[j].id,&keys[i].id))j++;
            spans[nu]=(UniqueSpan){i,j};
            rem_ids[nu]=keys[i].id;
            rem_u[nu]=nu;
            nu++;
            i=j;
        }

        PartitionDesc pd=RelationGetPartitionDesc(parent,true);
        const char *relname=RelationGetRelationName(parent);
        bool is_canon=(RelationGetNamespace(parent)==lp_pg_schema() &&
                       (strcmp(relname,"entity")==0 || strcmp(relname,"physicality")==0));
        bool is_paths=(strcmp(relname,"physicality")==0);
        const Oid *parts=NULL,*pks=NULL;
        const int16 *min_t=NULL,*max_t=NULL;
        int nparts=0;
        Oid local_parts[256],local_pks[256];
        int order[256],prio[256];
        if(is_canon){
            nparts=lp_pg_canonical_partitions(relname,&parts,&pks,NULL,NULL);
            (void)lp_pg_canonical_tier_bounds(relname,&min_t,&max_t);
            for(int p=0;p<nparts;p++){order[p]=p;prio[p]=tier_probe_priority(min_t[p],max_t[p],is_paths);}
            for(int a=1;a<nparts;a++){int k=order[a],pk=prio[a],b=a-1;while(b>=0 && prio[b]>pk){order[b+1]=order[b];prio[b+1]=prio[b];b--;}order[b+1]=k;prio[b+1]=pk;}
        }else{
            nparts=pd?pd->nparts:0;
            if(nparts>256)ereport(ERROR,(errmsg("Native tier presence: too many partitions (%d)",nparts)));
            for(int p=0;p<nparts;p++){local_parts[p]=pd->oids[p];local_pks[p]=InvalidOid;order[p]=p;}
            parts=local_parts;pks=local_pks;
        }

        size_t remaining=nu;
        PG_TRY();{
            for(int oi=0;oi<nparts && remaining>0 && (!done || !*done);oi++){
                CHECK_FOR_INTERRUPTS();
                int p=order[oi];
                Relation table=table_open(parts[p],AccessShareLock);
                AttrNumber stored_attribute=get_attnum(parts[p],column);
                TupleConversionMap *conversion=lp_pg_tuple_conversion(RelationGetDescr(table),RelationGetDescr(parent));
                TupleTableSlot *projected=conversion?MakeSingleTupleTableSlot(RelationGetDescr(parent),&TTSOpsVirtual):NULL;
                Oid primary=pks[p];
                if(!OidIsValid(primary))primary=find_unique_col_btree(table,stored_attribute);
                if(!OidIsValid(primary))primary=RelationGetPrimaryKeyIndex(table,false);
                if(!OidIsValid(primary))ereport(ERROR,(errmsg("Native ID reader: relation partition has no unique ID index")));
                Relation index=index_open(primary,AccessShareLock);
                Oid equality=get_opfamily_member(index->rd_opfamily[0],native_id,native_id,BTEqualStrategyNumber);
                TupleTableSlot *slot=table_slot_create(table,NULL);

                for(size_t c0=0;c0<remaining && (!done || !*done);){
                    size_t c1=Min(remaining,c0+array_ids);
                    Datum *values=palloc((Size)(c1-c0)*sizeof(Datum));
                    for(size_t i=c0;i<c1;i++)values[i-c0]=PointerGetDatum(&rem_ids[i]);
                    ArrayType *wanted=construct_array(values,(int)(c1-c0),native_id,sizeof(lp_id),false,TYPALIGN_CHAR);
                    pfree(values);
                    ScanKeyData scan_key;
                    ScanKeyEntryInitialize(&scan_key,SK_SEARCHARRAY,1,BTEqualStrategyNumber,native_id,
                        index->rd_indcollation[0],get_opcode(equality),PointerGetDatum(wanted));
                    IndexScanDesc scan=index_beginscan(table,index,GetActiveSnapshot(),NULL,1,0);
                    index_rescan(scan,&scan_key,1,NULL,0);
                    size_t at=c0;
                    while((!done || !*done) && index_getnext_slot(scan,ForwardScanDirection,slot)){
                        CHECK_FOR_INTERRUPTS();bool missing;
                        Datum found=slot_getattr(slot,stored_attribute,&missing);
                        if(missing)ereport(ERROR,(errmsg("Native ID reader: null primary ID")));
                        const lp_id *id=(const lp_id *)DatumGetPointer(found);
                        TupleTableSlot *row=conversion?execute_attr_map_slot(conversion->attrMap,slot,projected):slot;
                        while(at<c1 && memcmp(rem_ids[at].b,id->b,sizeof(lp_id))<0)at++;
                        if(at<c1 && lp_id_eq(&rem_ids[at],id)){
                            size_t ui=rem_u[at];
                            matched[ui]=true;
                            for(size_t k=spans[ui].begin;k<spans[ui].end && (!done || !*done);k++)
                                visit(row,&keys[k],context);
                        }
                        if(projected)ExecClearTuple(projected);
                        ExecClearTuple(slot);
                    }
                    index_endscan(scan);
                    pfree(wanted);
                    c0=c1;
                }

                ExecDropSingleTupleTableSlot(slot);
                if(projected){ExecDropSingleTupleTableSlot(projected);free_conversion_map(conversion);}
                index_close(index,NoLock);
                table_close(table,NoLock);

                size_t keep=0;
                for(size_t i=0;i<remaining;i++){
                    if(!matched[rem_u[i]]){
                        rem_ids[keep]=rem_ids[i];
                        rem_u[keep]=rem_u[i];
                        keep++;
                    }
                }
                remaining=keep;
            }
        }PG_FINALLY();{
            pfree(spans);pfree(rem_ids);pfree(rem_u);pfree(matched);
        }PG_END_TRY();
        return;
    }

    PartitionDesc partitions=key?RelationGetPartitionDesc(parent,true):NULL;
    for(size_t i=0;i<count;i++)keys[i].partition=lp_pg_enrichment_partition(target,key,partitions,PointerGetDatum(&keys[i].id));
    lp_sort(keys,(size_t)count,sizeof(*keys),presence_order);
    for(size_t begin=0;begin<count && (!done || !*done);){
        CHECK_FOR_INTERRUPTS();size_t end=begin+1;
        while(end<count && keys[end].partition==keys[begin].partition)end++;
        /* The PostgreSQL array representation bounds one index search, not
         * the admitted frontier. Continue with the remaining IDs if needed. */
        if(end-begin>array_ids)end=begin+array_ids;
        Relation table=table_open(keys[begin].partition,AccessShareLock);
        AttrNumber stored_attribute=get_attnum(RelationGetRelid(table),column);
        TupleConversionMap *conversion=lp_pg_tuple_conversion(RelationGetDescr(table),RelationGetDescr(parent));
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
        TupleConversionMap *conversion=lp_pg_tuple_conversion(RelationGetDescr(table),RelationGetDescr(parent));
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
    bool attestations=target==get_relname_relid("attestation",schema);
    if(!paths && !standings && !attestations && target!=get_relname_relid("entity",schema))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native ID target must be an installed canonical relation")));
    *column=paths?"entity":(standings||attestations)?"claim":"id";
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
    const char *selected[]={target==lp_pg_relation("physicality")?"entity":(target==lp_pg_relation("consensus")||target==lp_pg_relation("attestation"))?"claim":"id","path","mask"};
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

PG_FUNCTION_INFO_V1(laplace_standings_scoped);
Datum laplace_standings_scoped(PG_FUNCTION_ARGS)
{
    if(PG_ARGISNULL(0)){
        TupleDesc td;
        set_begin(fcinfo,"laplace_standings_scoped",&td);
        return (Datum)0;
    }
    if(PG_NARGS()<2||PG_ARGISNULL(1))return stored_rows(fcinfo,2);
    TupleDesc td;
    Tuplestorestate *result=set_begin(fcinfo,"laplace_standings_scoped",&td);
    int nw=0;lp_id *witnesses=ids_of(PG_GETARG_ARRAYTYPE_P(1),&nw);
    if(!nw){pfree(witnesses);return (Datum)0;}
    int count=0;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(!count){pfree(ids);pfree(witnesses);return (Datum)0;}
    lp_idmap *solved=lp_idmap_sized(sizeof(lp_standing));
    PG_TRY();{
        lp_pg_solve_scoped_standings(ids,count,get_fn_expr_argtype(fcinfo->flinfo,0),witnesses,nw,solved);
        for(int i=0;i<count;i++){
            const lp_standing *st=lp_idmap_lookup(solved,&ids[i]);
            if(!st)continue;
            Datum values[6]={
                Int64GetDatum((int64)i+1),id_datum(&ids[i]),
                Float8GetDatum(st->rating),Float8GetDatum(st->deviation),
                Float8GetDatum(st->volatility),Int64GetDatum((int64)st->matches)
            };
            bool missing[6]={false};
            set_put(fcinfo,result,td,values,missing);
            pfree(DatumGetPointer(values[1]));
        }
    }PG_FINALLY();{
        lp_idmap_free(solved);
        pfree(ids);pfree(witnesses);
    }PG_END_TRY();
    return (Datum)0;
}

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

typedef struct {
    lp_id id, lineage;
    double trust;
    bool present, has_lineage;
} WitnessPick;

static int cmp_lp_id(const void *a, const void *b)
{
    return memcmp(((const lp_id *)a)->b, ((const lp_id *)b)->b, sizeof(lp_id));
}

static int witness_pick_better(const WitnessPick *a, const WitnessPick *b)
{
    if (!b->present) return 1;
    if (a->has_lineage != b->has_lineage) return !a->has_lineage ? 1 : -1;
    if (!a->has_lineage) return 0;
    return memcmp(a->lineage.b, b->lineage.b, sizeof(lp_id)) < 0 ? 1 : 0;
}

static bool witness_read_slot(TupleTableSlot *slot, AttrNumber aw, AttrNumber al, AttrNumber at, WitnessPick *out)
{
    bool nw = true, nl = true, nt = true;
    Datum dw = slot_getattr(slot, aw, &nw);
    Datum dl = slot_getattr(slot, al, &nl);
    Datum dt = slot_getattr(slot, at, &nt);
    if (nw || nt)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Native witness frontier: null witness metadata")));
    memcpy(out->id.b, DatumGetPointer(dw), sizeof(lp_id));
    out->has_lineage = !nl;
    if (!nl) memcpy(out->lineage.b, DatumGetPointer(dl), sizeof(lp_id));
    else memset(out->lineage.b, 0, sizeof(lp_id));
    out->trust = DatumGetFloat8(dt);
    out->present = true;
    return true;
}

void lp_pg_lookup_witnesses(const lp_id *ids,size_t count,bool lineage,lp_pg_witness_visit visit,void *context)
{
    if(!count)return;
    Oid target=lp_pg_relation("attestation");
    const char *selected[]={"witness","trust","lineage"};
    const char *column;
    Relation parent=lp_pg_read_parent(target,&column,selected,lineage?3:2);
    Oid native_id=get_atttype(target,get_attnum(target,"witness"));
    table_close(parent,NoLock);

    if(count>MaxAllocHugeSize/sizeof(lp_id))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("Native witness frontier exceeds addressable memory")));
    lp_id *uniq=palloc_extended(count*sizeof(lp_id),MCXT_ALLOC_HUGE);
    memcpy(uniq,ids,count*sizeof(lp_id));
    lp_sort(uniq,count,sizeof(lp_id),cmp_lp_id);
    size_t nuniq=0;
    for(size_t i=0;i<count;i++)
        if(nuniq==0||!lp_id_eq(&uniq[nuniq-1],&uniq[i]))
            uniq[nuniq++]=uniq[i];
    WitnessPick *best=palloc_extended(nuniq*sizeof(WitnessPick),MCXT_ALLOC_HUGE|MCXT_ALLOC_ZERO);

    const Oid *parts=NULL,*aux=NULL;
    int nparts=lp_pg_canonical_partitions("attestation",&parts,NULL,&aux,NULL);
    Snapshot snap=GetActiveSnapshot();
    for(int p=0;p<nparts;p++){
        if(!OidIsValid(aux[p]))
            ereport(ERROR,(errmsg("Native witness frontier: attestation partition missing attestation_witness_lineage index")));
        Relation part=table_open(parts[p],AccessShareLock);
        Relation idx=index_open(aux[p],AccessShareLock);
        AttrNumber aw=get_attnum(parts[p],"witness"),al=get_attnum(parts[p],"lineage"),at=get_attnum(parts[p],"trust");
        Oid eq_op=get_opfamily_member(idx->rd_opfamily[0],native_id,native_id,BTEqualStrategyNumber);
        RegProcedure eq_proc=get_opcode(eq_op);
        IndexScanDesc scan=index_beginscan(part,idx,snap,NULL,1,0);
        TupleTableSlot *slot=table_slot_create(part,NULL);
        ScanKeyData sk;
        for(size_t i=0;i<nuniq;i++){
            if((i&255)==0)CHECK_FOR_INTERRUPTS();
            ScanKeyEntryInitialize(&sk,0,1,BTEqualStrategyNumber,native_id,idx->rd_indcollation[0],eq_proc,PointerGetDatum(&uniq[i]));
            index_rescan(scan,&sk,1,NULL,0);
            if(index_getnext_slot(scan,ForwardScanDirection,slot)){
                WitnessPick cand;
                witness_read_slot(slot,aw,al,at,&cand);
                if(witness_pick_better(&cand,&best[i])>0)best[i]=cand;
                ExecClearTuple(slot);
            }
        }
        ExecDropSingleTupleTableSlot(slot);index_endscan(scan);
        index_close(idx,NoLock);table_close(part,NoLock);
    }

    for(size_t i=0;i<nuniq;i++)
        if(best[i].present)
            visit(&uniq[i],(lineage&&best[i].has_lineage)?&best[i].lineage:NULL,best[i].trust,context);
    pfree(best);pfree(uniq);
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

static void witness_trust_emit(const lp_id *id,const lp_id *lineage,double trust,void *context)
{
    PresenceOutput *out=context;
    Datum v[3]={id_datum(id),lineage?id_datum(lineage):(Datum)0,Float8GetDatum(trust)};
    bool n[3]={false,lineage==NULL,false};
    set_put(out->fcinfo,out->output,out->descriptor,v,n);
    pfree(DatumGetPointer(v[0]));
    if(lineage)pfree(DatumGetPointer(v[1]));
}

typedef struct {
    Relation part,idx;
    TupleTableSlot *slot;
    IndexScanDesc scan;
    RegProcedure gt_proc;
    AttrNumber aw,al,at;
    bool active;
    WitnessPick cur;
} WitnessSkipPart;

void lp_pg_scan_all_witnesses(lp_pg_witness_visit visit,void *context)
{
    if(!visit)return;
    Oid target=lp_pg_relation("attestation");
    const char *selected[]={"witness","lineage","trust"};
    const char *column;
    Relation parent=lp_pg_read_parent(target,&column,selected,3);
    Oid native_id=get_atttype(target,get_attnum(target,"witness"));
    table_close(parent,NoLock);

    const Oid *parts=NULL,*aux=NULL;
    int nparts=lp_pg_canonical_partitions("attestation",&parts,NULL,&aux,NULL);
    WitnessSkipPart *skips=palloc0((Size)nparts*sizeof(*skips));
    Snapshot snap=GetActiveSnapshot();
    int active_count=0;
    for(int p=0;p<nparts;p++){
        WitnessSkipPart *sp=&skips[p];
        if(!OidIsValid(aux[p]))
            ereport(ERROR,(errmsg("laplace_witness_trust: missing attestation_witness_lineage index")));
        sp->part=table_open(parts[p],AccessShareLock);
        sp->idx=index_open(aux[p],AccessShareLock);
        sp->aw=get_attnum(parts[p],"witness");
        sp->al=get_attnum(parts[p],"lineage");
        sp->at=get_attnum(parts[p],"trust");
        Oid gt_op=get_opfamily_member(sp->idx->rd_opfamily[0],native_id,native_id,BTGreaterStrategyNumber);
        sp->gt_proc=get_opcode(gt_op);
        sp->slot=table_slot_create(sp->part,NULL);
        IndexScanDesc seed=index_beginscan(sp->part,sp->idx,snap,NULL,0,0);
        index_rescan(seed,NULL,0,NULL,0);
        if(index_getnext_slot(seed,ForwardScanDirection,sp->slot)){
            witness_read_slot(sp->slot,sp->aw,sp->al,sp->at,&sp->cur);
            ExecClearTuple(sp->slot);
            sp->scan=index_beginscan(sp->part,sp->idx,snap,NULL,1,0);
            sp->active=true;
            active_count++;
        }
        index_endscan(seed);
    }

    while(active_count>0){
        CHECK_FOR_INTERRUPTS();
        lp_id min_w;bool have_min=false;
        for(int p=0;p<nparts;p++){
            if(!skips[p].active)continue;
            if(!have_min||memcmp(skips[p].cur.id.b,min_w.b,sizeof(lp_id))<0){
                min_w=skips[p].cur.id;have_min=true;
            }
        }
        if(!have_min)break;
        WitnessPick best={0};
        for(int p=0;p<nparts;p++){
            if(!skips[p].active||!lp_id_eq(&skips[p].cur.id,&min_w))continue;
            if(witness_pick_better(&skips[p].cur,&best)>0)best=skips[p].cur;
            ScanKeyData sk;
            ScanKeyEntryInitialize(&sk,0,1,BTGreaterStrategyNumber,native_id,
                skips[p].idx->rd_indcollation[0],skips[p].gt_proc,PointerGetDatum(&min_w));
            index_rescan(skips[p].scan,&sk,1,NULL,0);
            if(index_getnext_slot(skips[p].scan,ForwardScanDirection,skips[p].slot)){
                witness_read_slot(skips[p].slot,skips[p].aw,skips[p].al,skips[p].at,&skips[p].cur);
                ExecClearTuple(skips[p].slot);
            }else{
                skips[p].active=false;
                active_count--;
            }
        }
        visit(&min_w,best.has_lineage?&best.lineage:NULL,best.trust,context);
    }

    for(int p=0;p<nparts;p++){
        if(skips[p].scan)index_endscan(skips[p].scan);
        if(skips[p].slot)ExecDropSingleTupleTableSlot(skips[p].slot);
        if(skips[p].idx)index_close(skips[p].idx,NoLock);
        if(skips[p].part)table_close(skips[p].part,NoLock);
    }
    pfree(skips);
}

PG_FUNCTION_INFO_V1(laplace_witness_trust);
Datum laplace_witness_trust(PG_FUNCTION_ARGS)
{
    PresenceOutput out={.fcinfo=fcinfo};
    out.output=set_begin(fcinfo,"laplace_witness_trust",&out.descriptor);
    if(PG_NARGS()>0&&!PG_ARGISNULL(0)){
        int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
        if(count>0)lp_pg_lookup_witnesses(ids,(size_t)count,true,witness_trust_emit,&out);
        pfree(ids);
        return (Datum)0;
    }
    lp_pg_scan_all_witnesses(witness_trust_emit,&out);
    return (Datum)0;
}

typedef struct {
    lp_pg_entity_coord_visit visit;
    void *context;
    AttrNumber a_tier,a_coord,a_hilbert;
    MemoryContext tuple_context;
} EntityCoordRead;

static void entity_coord_visit(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    EntityCoordRead *read=context;
    bool nt=false,nc=false,nh=false;
    Datum dt=slot_getattr(slot,read->a_tier,&nt);
    Datum dc=slot_getattr(slot,read->a_coord,&nc);
    Datum dh=slot_getattr(slot,read->a_hilbert,&nh);
    if(nt||nc||nh)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("Native entity coord lookup: null column")));
    MemoryContext prev=MemoryContextSwitchTo(read->tuple_context);
    read->visit(key->ordinal-1,&key->id,DatumGetInt16(dt),dc,DatumGetInt64(dh),read->context);
    MemoryContextSwitchTo(prev);
    MemoryContextReset(read->tuple_context);
}

void lp_pg_lookup_entity_coords(const lp_id *ids,size_t count,lp_pg_entity_coord_visit visit,void *context)
{
    if(!count)return;
    if(count>MaxAllocHugeSize/sizeof(PresenceKey))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("Native entity coord lookup exceeds addressable memory")));
    Oid target=lp_pg_relation("entity");
    const char *selected[]={"id","tier","coord","hilbert"};
    const char *column;
    Relation parent=lp_pg_read_parent(target,&column,selected,4);
    PresenceKey *keys=palloc_extended(count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(size_t i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    EntityCoordRead read={
        .visit=visit,.context=context,
        .a_tier=get_attnum(target,"tier"),.a_coord=get_attnum(target,"coord"),.a_hilbert=get_attnum(target,"hilbert"),
        .tuple_context=AllocSetContextCreate(CurrentMemoryContext,"Native entity coord tuple",ALLOCSET_DEFAULT_SIZES)
    };
    lp_pg_indexed_keys(parent,column,keys,count,entity_coord_visit,&read);
    MemoryContextDelete(read.tuple_context);pfree(keys);table_close(parent,NoLock);
}

static void entity_tier_visit(TupleTableSlot *slot,const PresenceKey *key,void *context)
{
    typedef struct { AttrNumber a_tier; int16 *tiers; } TierOut;
    TierOut *out=context;bool nt=false;
    Datum dt=slot_getattr(slot,out->a_tier,&nt);
    out->tiers[key->ordinal-1]=nt?-1:DatumGetInt16(dt);
}

void lp_pg_lookup_entity_tiers(const lp_id *ids,size_t count,int16 *tiers)
{
    if(!count)return;
    for(size_t i=0;i<count;i++)tiers[i]=-1;
    if(count>MaxAllocHugeSize/sizeof(PresenceKey))
        ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("Native entity tier lookup exceeds addressable memory")));
    Oid target=lp_pg_relation("entity");
    const char *selected[]={"id","tier"};
    const char *column;
    Relation parent=lp_pg_read_parent(target,&column,selected,2);
    PresenceKey *keys=palloc_extended(count*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(size_t i=0;i<count;i++)keys[i]=(PresenceKey){.id=ids[i],.ordinal=(int64)i+1};
    struct { AttrNumber a_tier; int16 *tiers; } out={get_attnum(target,"tier"),tiers};
    lp_pg_indexed_keys(parent,column,keys,count,entity_tier_visit,&out);
    pfree(keys);table_close(parent,NoLock);
}

static void sql_entity_coord_emit(int64 ordinal,const lp_id *id,int16 tier,Datum coord,int64 hilbert,void *context)
{
    PresenceOutput *out=context;
    Geo g=geo_of(coord);
    const double *p=xyzm(&g);
    Datum v[8]={
        Int64GetDatum(ordinal+1),id_datum(id),Int16GetDatum(tier),
        Float8GetDatum(p[0]),Float8GetDatum(p[1]),Float8GetDatum(p[2]),Float8GetDatum(p[3]),
        Int64GetDatum(hilbert)
    };
    bool n[8]={false};
    set_put(out->fcinfo,out->output,out->descriptor,v,n);
    pfree(DatumGetPointer(v[1]));
}

PG_FUNCTION_INFO_V1(laplace_entity_coords);
Datum laplace_entity_coords(PG_FUNCTION_ARGS)
{
    PresenceOutput out={.fcinfo=fcinfo};
    out.output=set_begin(fcinfo,"laplace_entity_coords",&out.descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(count>0)lp_pg_lookup_entity_coords(ids,(size_t)count,sql_entity_coord_emit,&out);
    pfree(ids);
    return (Datum)0;
}

void lp_pg_read_attestation_pk(const lp_id *claims,int n,const lp_idmap *roots,PgAttrRows *out,lp_idmap *witnesses)
{
    if(n<=0)return;
    Oid target=lp_pg_relation("attestation");
    const char *selected[]={"claim","witness","lineage","games","tokens","score","position","trust"};
    const char *column;
    Relation parent=lp_pg_read_parent(target,&column,selected,8);
    Oid native_id=get_atttype(target,get_attnum(target,"claim"));
    PartitionKey pkey=RelationGetPartitionKey(parent);
    PartitionDesc pdesc=pkey?RelationGetPartitionDesc(parent,true):NULL;
    PresenceKey *keys=palloc_extended((Size)n*sizeof(*keys),MCXT_ALLOC_HUGE);
    for(int i=0;i<n;i++)
        keys[i]=(PresenceKey){
            .partition=lp_pg_enrichment_partition(target,pkey,pdesc,PointerGetDatum(&claims[i])),
            .id=claims[i],.ordinal=0
        };
    table_close(parent,NoLock);

    lp_sort(keys,(size_t)n,sizeof(*keys),presence_order);
    size_t nuniq=0;
    for(size_t i=0;i<(size_t)n;i++)
        if(nuniq==0||!lp_id_eq(&keys[nuniq-1].id,&keys[i].id))
            keys[nuniq++]=keys[i];

    const Oid *parts=NULL,*pks=NULL;
    int nparts=lp_pg_canonical_partitions("attestation",&parts,&pks,NULL,NULL);
    Snapshot snap=GetActiveSnapshot();
    size_t array_ids=(MaxAllocSize-ARR_OVERHEAD_NONULLS(1))/sizeof(lp_id);

    for(size_t begin=0;begin<nuniq;){
        CHECK_FOR_INTERRUPTS();
        size_t end=begin+1;
        while(end<nuniq&&keys[end].partition==keys[begin].partition)end++;
        if(end-begin>array_ids)end=begin+array_ids;

        Oid partOid=keys[begin].partition;
        Relation part=table_open(partOid,AccessShareLock);
        Oid pkOid=InvalidOid;
        for(int p=0;p<nparts;p++)if(parts[p]==partOid){pkOid=pks[p];break;}
        if(!OidIsValid(pkOid))pkOid=RelationGetPrimaryKeyIndex(part,false);
        if(!OidIsValid(pkOid))ereport(ERROR,(errmsg("Native attestation reader: partition has no primary index")));
        Relation idx=index_open(pkOid,AccessShareLock);

        Datum *vals=palloc((Size)(end-begin)*sizeof(Datum));
        for(size_t i=begin;i<end;i++)vals[i-begin]=PointerGetDatum(&keys[i].id);
        ArrayType *wanted=construct_array(vals,(int)(end-begin),native_id,sizeof(lp_id),false,TYPALIGN_CHAR);
        pfree(vals);

        Oid eq_op=get_opfamily_member(idx->rd_opfamily[0],native_id,native_id,BTEqualStrategyNumber);
        ScanKeyData sk;
        ScanKeyEntryInitialize(&sk,SK_SEARCHARRAY,1,BTEqualStrategyNumber,native_id,
            idx->rd_indcollation[0],get_opcode(eq_op),PointerGetDatum(wanted));
        IndexScanDesc scan=index_beginscan(part,idx,snap,NULL,1,0);
        index_rescan(scan,&sk,1,NULL,0);

        AttrNumber ac=get_attnum(partOid,"claim"),aw=get_attnum(partOid,"witness"),
                   al=get_attnum(partOid,"lineage"),ag=get_attnum(partOid,"games"),
                   atk=get_attnum(partOid,"tokens"),asc=get_attnum(partOid,"score"),
                   ap=get_attnum(partOid,"position"),at=get_attnum(partOid,"trust");
        TupleTableSlot *slot=table_slot_create(part,NULL);
        while(index_getnext_slot(scan,ForwardScanDirection,slot)){
            CHECK_FOR_INTERRUPTS();
            bool missing;Datum val;
            PgAttrRow row={.witness=1};
            val=slot_getattr(slot,ac,&missing);
            if(missing)ereport(ERROR,(errmsg("laplace attestation: null claim")));
            memcpy(row.claim.b,DatumGetPointer(val),16);
            val=slot_getattr(slot,aw,&missing);
            if(missing)ereport(ERROR,(errmsg("laplace attestation: null witness")));
            memcpy(row.trunk.b,DatumGetPointer(val),16);
            if(roots&&lp_idmap_find(roots,&row.trunk)<0){ExecClearTuple(slot);continue;}
            val=slot_getattr(slot,al,&missing);
            row.has_dependence=!missing;
            if(!missing)memcpy(row.dependence.b,DatumGetPointer(val),16);
            val=slot_getattr(slot,ag,&missing);
            int64 games=missing?0:DatumGetInt64(val);
            val=slot_getattr(slot,atk,&missing);
            int64 tokens=missing?games:DatumGetInt64(val);
            val=slot_getattr(slot,asc,&missing);
            double mean_score=missing?0.0:DatumGetFloat8(val);
            val=slot_getattr(slot,ap,&missing);
            row.position=missing?0:(uint32_t)DatumGetInt32(val);
            val=slot_getattr(slot,at,&missing);
            row.trust=missing?0.0:DatumGetFloat8(val);
            row.source_trust=row.trust;
            row.games=(double)games;
            row.tokens=(double)(tokens>=games?tokens:games);
            row.score=(double)games*mean_score;
            if(witnesses){
                bool fresh;
                PgSeriesWitness *w=lp_idmap_get(witnesses,&row.trunk,&fresh);
                if(fresh||!row.has_dependence){
                    w->trust=row.trust;
                    w->has_lineage=row.has_dependence;
                    if(row.has_dependence)w->lineage=row.dependence;
                }
            }
            lp_push(out,row);
            ExecClearTuple(slot);
        }
        ExecDropSingleTupleTableSlot(slot);index_endscan(scan);pfree(wanted);
        index_close(idx,NoLock);table_close(part,NoLock);
        begin=end;
    }
    pfree(keys);
}

typedef lp_vec(lp_id) FiIdVec;
typedef struct { FiIdVec *ids; } WitnessListCtx;
static void collect_witness_id(const lp_id *id,const lp_id *lineage,double trust,void *context)
{
    (void)lineage;(void)trust;
    WitnessListCtx *ctx=context;
    lp_push(ctx->ids,*id);
}
static void collect_trunk_start(const lp_id *id,Datum path,Datum mask,void *context)
{
    (void)id;(void)mask;
    Geo g=geo_of(path);
    if(g.p.n>0){
        lp_id first=lp_path_id(g.p,0);
        lp_idmap_put((lp_idmap *)context,&first,NULL);
    }
}

PG_FUNCTION_INFO_V1(laplace_present_trunks);
Datum laplace_present_trunks(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts=set_begin(fcinfo,"laplace_present_trunks",&td);
    if(PG_ARGISNULL(0))return (Datum)0;
    int count=0;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    if(count<=0){pfree(ids);return (Datum)0;}
    FiIdVec witnesses={0};
    lp_idmap *started=lp_idmap_new();
    PG_TRY();{
        WitnessListCtx wctx={&witnesses};
        lp_pg_scan_all_witnesses(collect_witness_id,&wctx);
        if(witnesses.n>0)
            lp_pg_lookup_paths(witnesses.v,witnesses.n,false,collect_trunk_start,started);
        for(int i=0;i<count;i++){
            if(lp_idmap_find(started,&ids[i])<0)continue;
            Datum v[2]={Int64GetDatum((int64)i+1),id_datum(&ids[i])};
            bool nl[2]={false,false};
            set_put(fcinfo,ts,td,v,nl);
            pfree(DatumGetPointer(v[1]));
        }
    }PG_FINALLY();{
        lp_vec_free(&witnesses);
        lp_idmap_free(started);
        pfree(ids);
    }PG_END_TRY();
    return (Datum)0;
}

typedef struct {
    Relation part, idx;
    TupleTableSlot *slot;
    IndexScanDesc scan;
    AttrNumber a_ent, a_tier, a_path, a_mask;
    lp_id cur_id;
    Datum cur_tier, cur_path, cur_mask;
    bool active;
} LibPagePartScan;

static bool lib_page_next(LibPagePartScan *sc, lp_mask file_mask, MemoryContext row_ctx)
{
    while (index_getnext_slot(sc->scan, ForwardScanDirection, sc->slot)) {
        CHECK_FOR_INTERRUPTS();
        bool ne = true, nt = true, np = true, nm = true;
        Datum de = slot_getattr(sc->slot, sc->a_ent, &ne);
        Datum dt = slot_getattr(sc->slot, sc->a_tier, &nt);
        Datum dp = slot_getattr(sc->slot, sc->a_path, &np);
        Datum dm = slot_getattr(sc->slot, sc->a_mask, &nm);
        if (!ne && !nt && !np && !nm && (lp_pg_mask_native(dm) & file_mask) != 0) {
            MemoryContext old = MemoryContextSwitchTo(row_ctx);
            memcpy(sc->cur_id.b, DatumGetPointer(de), sizeof(lp_id));
            sc->cur_tier = dt;
            sc->cur_path = PointerGetDatum(PG_DETOAST_DATUM_COPY(dp));
            sc->cur_mask = PointerGetDatum(PG_DETOAST_DATUM_COPY(dm));
            MemoryContextSwitchTo(old);
            ExecClearTuple(sc->slot);
            sc->active = true;
            return true;
        }
        ExecClearTuple(sc->slot);
    }
    sc->active = false;
    return false;
}

/* Keyset-paginated library trunks across canonical physicality partitions in entity ID order. */
PG_FUNCTION_INFO_V1(laplace_library_page);
Datum laplace_library_page(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_library_page", &td);
    bool has_after = !PG_ARGISNULL(0);
    lp_id after = {0};
    if (has_after) {
        const lp_id *p = (const lp_id *)PG_GETARG_POINTER(0);
        after = *p;
    }
    int page_limit = PG_ARGISNULL(1) ? 64 : PG_GETARG_INT32(1);
    if (page_limit < 1) page_limit = 1;
    if (page_limit > 1000) page_limit = 1000;

    lp_mask file_mask = lp_pg_kind_mask("file");
    Oid target = lp_pg_relation("physicality");
    const char *selected[] = {"entity", "tier", "path", "mask"};
    const char *column;
    Relation parent = lp_pg_read_parent(target, &column, selected, 4);
    AttrNumber a_ent = get_attnum(target, "entity");
    Oid native_id = get_atttype(target, a_ent);
    PartitionKey pkey = RelationGetPartitionKey(parent);
    bool by_id = (pkey && pkey->partnatts == 1 && pkey->partattrs[0] == a_ent);
    table_close(parent, NoLock);

    const Oid *parts = NULL, *pks = NULL;
    int nparts = lp_pg_canonical_partitions("physicality", &parts, &pks, NULL, NULL);
    Snapshot snap = GetActiveSnapshot();
    MemoryContext caller = CurrentMemoryContext;
    MemoryContext row_ctx = AllocSetContextCreate(caller, "laplace_library_page row", ALLOCSET_DEFAULT_SIZES);
    int emitted = 0;

    if (!by_id) {
        LibPagePartScan *scans = palloc0((Size)Max(nparts, 1) * sizeof(*scans));
        PG_TRY(); {
            for (int p = 0; p < nparts; p++) {
                LibPagePartScan *sc = &scans[p];
                sc->part = table_open(parts[p], AccessShareLock);
                sc->a_ent = get_attnum(parts[p], "entity");
                sc->a_tier = get_attnum(parts[p], "tier");
                sc->a_path = get_attnum(parts[p], "path");
                sc->a_mask = get_attnum(parts[p], "mask");
                Oid pk_oid = pks[p];
                if (!OidIsValid(pk_oid)) pk_oid = find_unique_col_btree(sc->part, sc->a_ent);
                if (!OidIsValid(pk_oid)) ereport(ERROR, (errmsg("laplace_library_page: missing entity B-tree on partition %d", p)));
                sc->idx = index_open(pk_oid, AccessShareLock);
                sc->slot = table_slot_create(sc->part, NULL);
                ScanKeyData sk;
                int nkeys = 0;
                if (has_after) {
                    Oid gt_op = get_opfamily_member(sc->idx->rd_opfamily[0], native_id, native_id, BTGreaterStrategyNumber);
                    ScanKeyEntryInitialize(&sk, 0, 1, BTGreaterStrategyNumber, native_id,
                                           sc->idx->rd_indcollation[0], get_opcode(gt_op), PointerGetDatum(&after));
                    nkeys = 1;
                }
                sc->scan = index_beginscan(sc->part, sc->idx, snap, NULL, nkeys, 0);
                index_rescan(sc->scan, nkeys ? &sk : NULL, nkeys, NULL, 0);
                (void)lib_page_next(sc, file_mask, row_ctx);
            }
            while (emitted < page_limit) {
                CHECK_FOR_INTERRUPTS();
                int best = -1;
                for (int p = 0; p < nparts; p++) {
                    if (!scans[p].active) continue;
                    if (best < 0 || memcmp(scans[p].cur_id.b, scans[best].cur_id.b, sizeof(lp_id)) < 0)
                        best = p;
                }
                if (best < 0) break;
                LibPagePartScan *sc = &scans[best];
                Datum d_ent = id_datum(&sc->cur_id);
                Datum v[4] = { d_ent, sc->cur_tier, sc->cur_path, sc->cur_mask };
                bool nl[4] = { false, false, false, false };
                set_put(fcinfo, ts, td, v, nl);
                pfree(DatumGetPointer(d_ent));
                pfree(DatumGetPointer(sc->cur_path));
                pfree(DatumGetPointer(sc->cur_mask));
                emitted++;
                if (emitted < page_limit)
                    (void)lib_page_next(sc, file_mask, row_ctx);
            }
        } PG_FINALLY(); {
            for (int p = 0; p < nparts; p++) {
                if (scans[p].scan) index_endscan(scans[p].scan);
                if (scans[p].slot) ExecDropSingleTupleTableSlot(scans[p].slot);
                if (scans[p].idx) index_close(scans[p].idx, NoLock);
                if (scans[p].part) table_close(scans[p].part, NoLock);
            }
            pfree(scans);
            MemoryContextSwitchTo(caller);
            MemoryContextDelete(row_ctx);
        } PG_END_TRY();
        return (Datum)0;
    }

    int start_p = (has_after && nparts == 256) ? (int)after.b[0] : 0;

    PG_TRY();
    {
        for (int p = start_p; p < nparts && emitted < page_limit; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(parts[p], AccessShareLock);
            Oid pk_oid = pks[p];
            if (!OidIsValid(pk_oid)) pk_oid = RelationGetPrimaryKeyIndex(part, false);
            if (!OidIsValid(pk_oid)) {
                table_close(part, NoLock);
                ereport(ERROR, (errmsg("laplace_library_page: missing primary key on physicality partition %d", p)));
            }
            Relation idx = index_open(pk_oid, AccessShareLock);
            TupleTableSlot *slot = table_slot_create(part, NULL);

            bool use_key = has_after && (nparts != 256 || p == start_p);
            ScanKeyData sk;
            int nkeys = 0;
            if (use_key) {
                Oid gt_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTGreaterStrategyNumber);
                RegProcedure gt_proc = get_opcode(gt_op);
                ScanKeyEntryInitialize(&sk, 0, 1, BTGreaterStrategyNumber, native_id,
                                       idx->rd_indcollation[0], gt_proc, PointerGetDatum(&after));
                nkeys = 1;
            }
            IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, nkeys, 0);
            index_rescan(scan, nkeys ? &sk : NULL, nkeys, NULL, 0);
            while (emitted < page_limit && index_getnext_slot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool ne = true, nt = true, np = true, nm = true;
                Datum de = slot_getattr(slot, a_ent, &ne);
                Datum dt = slot_getattr(slot, get_attnum(parts[p], "tier"), &nt);
                Datum dp = slot_getattr(slot, get_attnum(parts[p], "path"), &np);
                Datum dm = slot_getattr(slot, get_attnum(parts[p], "mask"), &nm);
                if (!ne && !nt && !np && !nm && (lp_pg_mask_native(dm) & file_mask) != 0) {
                    MemoryContextSwitchTo(row_ctx);
                    Datum v[4] = { de, dt, dp, dm };
                    bool nl[4] = { false, false, false, false };
                    set_put(fcinfo, ts, td, v, nl);
                    MemoryContextSwitchTo(caller);
                    MemoryContextReset(row_ctx);
                    emitted++;
                }
                ExecClearTuple(slot);
            }
            index_endscan(scan);
            ExecDropSingleTupleTableSlot(slot);
            index_close(idx, NoLock);
            table_close(part, NoLock);
        }
    }
    PG_FINALLY();
    {
        MemoryContextSwitchTo(caller);
        MemoryContextDelete(row_ctx);
    }
    PG_END_TRY();
    return (Datum)0;
}

static void present_mask_row(TupleTableSlot *slot, const PresenceKey *key, void *context)
{
    (void)slot;
    ((Datum *)context)[key->ordinal - 1] = BoolGetDatum(true);
}

/* Direct-AM boolean presence bitmap aligned with the caller's entity ID array. */
PG_FUNCTION_INFO_V1(laplace_present_mask);
Datum laplace_present_mask(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    int count = 0;
    lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &count);
    const char *column;
    Relation parent = lp_pg_presence_parent(lp_pg_relation("entity"), &column, 1);
    if (count <= 0) {
        table_close(parent, NoLock);
        pfree(ids);
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(BOOLOID));
    }
    if ((Size)count > MaxAllocHugeSize / sizeof(PresenceKey))
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("laplace_present_mask: array exceeds addressable memory")));
    Datum *present = palloc(sizeof(Datum) * (Size)count);
    for (int i = 0; i < count; i++) present[i] = BoolGetDatum(false);
    PresenceKey *keys = palloc_extended((Size)count * sizeof(*keys), MCXT_ALLOC_HUGE);
    for (int i = 0; i < count; i++) keys[i] = (PresenceKey){ .id = ids[i], .ordinal = (int64)i + 1 };
    lp_pg_indexed_keys(parent, column, keys, (size_t)count, present_mask_row, present);
    table_close(parent, NoLock);
    ArrayType *out = construct_array(present, count, BOOLOID, sizeof(bool), true, TYPALIGN_CHAR);
    pfree(keys);
    pfree(present);
    pfree(ids);
    PG_RETURN_ARRAYTYPE_P(out);
}

static void known_files_mark_bit(TupleTableSlot *slot, const PresenceKey *key, void *context)
{
    (void)slot;
    typedef struct { lp_idmap *state; const lp_id *uniq; uint8 bit; } MarkCtx;
    MarkCtx *ctx = context;
    uint8 *flags = lp_idmap_lookup(ctx->state, &ctx->uniq[key->ordinal - 1]);
    if (flags) *flags |= ctx->bit;
}

/* Direct-AM file trunk presence: IDs present in both canonical entity and physicality. */
PG_FUNCTION_INFO_V1(laplace_known_files);
Datum laplace_known_files(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_known_files", &td);
    if (PG_ARGISNULL(0)) return (Datum)0;
    int count = 0;
    lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &count);
    const char *ent_col = NULL, *phys_col = NULL;
    Relation ent_parent = lp_pg_presence_parent(lp_pg_relation("entity"), &ent_col, 1);
    Relation phys_parent = lp_pg_presence_parent(lp_pg_relation("physicality"), &phys_col, 1);
    if (count <= 0) {
        table_close(phys_parent, NoLock);
        table_close(ent_parent, NoLock);
        pfree(ids);
        return (Datum)0;
    }

    lp_idmap *state = lp_idmap_sized(sizeof(uint8));
    PG_TRY();
    {
        for (int i = 0; i < count; i++) {
            bool fresh = false;
            uint8 *f = lp_idmap_get(state, &ids[i], &fresh);
            if (fresh) *f = 0;
        }
        size_t nuniq = lp_idmap_count(state);
        const lp_id *uniq = lp_idmap_keys(state);
        PresenceKey *keys = palloc_extended(nuniq * sizeof(*keys), MCXT_ALLOC_HUGE);
        for (size_t i = 0; i < nuniq; i++) keys[i] = (PresenceKey){ .id = uniq[i], .ordinal = (int64)i + 1 };

        struct { lp_idmap *state; const lp_id *uniq; uint8 bit; } mctx = { state, uniq, 1 };
        lp_pg_indexed_keys(ent_parent, ent_col, keys, nuniq, known_files_mark_bit, &mctx);

        size_t nphys = 0;
        for (size_t i = 0; i < nuniq; i++) {
            uint8 *f = lp_idmap_at(state, i);
            if ((*f & 1) != 0) keys[nphys++] = (PresenceKey){ .id = uniq[i], .ordinal = (int64)i + 1 };
        }
        if (nphys > 0) {
            mctx.bit = 2;
            lp_pg_indexed_keys(phys_parent, phys_col, keys, nphys, known_files_mark_bit, &mctx);
        }
        pfree(keys);

        for (int i = 0; i < count; i++) {
            uint8 *f = lp_idmap_lookup(state, &ids[i]);
            if (f && *f == 3) {
                Datum v[2] = { Int64GetDatum((int64)i + 1), id_datum(&ids[i]) };
                bool nl[2] = { false, false };
                set_put(fcinfo, ts, td, v, nl);
                pfree(DatumGetPointer(v[1]));
            }
        }
    }
    PG_FINALLY();
    {
        table_close(phys_parent, NoLock);
        table_close(ent_parent, NoLock);
        lp_idmap_free(state);
        pfree(ids);
    }
    PG_END_TRY();
    return (Datum)0;
}

/* Direct-AM total attestation games per claim ID, keyed by request ordinal. */
PG_FUNCTION_INFO_V1(laplace_attestation_games);
Datum laplace_attestation_games(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_attestation_games", &td);
    if (PG_ARGISNULL(0)) return (Datum)0;
    int count = 0;
    lp_id *ids = ids_of(PG_GETARG_ARRAYTYPE_P(0), &count);
    if (count <= 0) {
        pfree(ids);
        return (Datum)0;
    }

    PgAttrRows rows = {0};
    lp_idmap *sums = lp_idmap_sized(sizeof(int64));
    PG_TRY();
    {
        lp_pg_read_attestation_pk(ids, count, NULL, &rows, NULL);
        for (size_t i = 0; i < rows.n; i++) {
            const PgAttrRow *r = &rows.v[i];
            int64 add = (int64)r->games;
            bool fresh = false;
            int64 *acc = lp_idmap_get(sums, &r->claim, &fresh);
            if (fresh) *acc = 0;
            if (add > 0 && *acc > PG_INT64_MAX - add)
                ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("laplace_attestation_games: sum exceeds bigint")));
            *acc += add;
        }
        for (int i = 0; i < count; i++) {
            int64 *acc = lp_idmap_lookup(sums, &ids[i]);
            if (acc) {
                Datum v[2] = { Int64GetDatum((int64)i + 1), Int64GetDatum(*acc) };
                bool nl[2] = { false, false };
                set_put(fcinfo, ts, td, v, nl);
            }
        }
    }
    PG_FINALLY();
    {
        lp_vec_free(&rows);
        lp_idmap_free(sums);
        pfree(ids);
    }
    PG_END_TRY();
    return (Datum)0;
}

static void standing_count_visit(TupleTableSlot *slot, const PresenceKey *key, void *context)
{
    (void)slot;
    (void)key;
    (*(int64 *)context)++;
}

/* Direct-AM count of distinct requested claim IDs with and without consensus rows. */
PG_FUNCTION_INFO_V1(laplace_standing_counts);
Datum laplace_standing_counts(PG_FUNCTION_ARGS)
{
    TupleDesc desc;
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("laplace_standing_counts requires composite result")));
    const char *selected[] = { "claim" }, *column = NULL;
    Relation parent = lp_pg_read_parent(lp_pg_relation("consensus"), &column, selected, 1);

    int count = 0;
    lp_id *ids = PG_ARGISNULL(0) ? NULL : ids_of(PG_GETARG_ARRAYTYPE_P(0), &count);
    int64 nuniq = 0, matched = 0;
    if (count > 0) {
        lp_sort(ids, (size_t)count, sizeof(lp_id), cmp_lp_id);
        size_t u = 1;
        for (int i = 1; i < count; i++)
            if (!lp_id_eq(&ids[u - 1], &ids[i])) ids[u++] = ids[i];
        nuniq = (int64)u;
        PresenceKey *keys = palloc_extended(u * sizeof(*keys), MCXT_ALLOC_HUGE);
        for (size_t i = 0; i < u; i++) keys[i] = (PresenceKey){ .id = ids[i], .ordinal = (int64)i + 1 };
        lp_pg_indexed_keys(parent, column, keys, u, standing_count_visit, &matched);
        pfree(keys);
    }
    if (ids) pfree(ids);
    table_close(parent, NoLock);

    Datum vals[2] = { Int64GetDatum(matched), Int64GetDatum(nuniq - matched) };
    bool nulls[2] = { false, false };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), vals, nulls)));
}

/* Direct-AM attestation count: total rows when NULL, or distinct claims for one witness. */
PG_FUNCTION_INFO_V1(laplace_attestation_count);
Datum laplace_attestation_count(PG_FUNCTION_ARGS)
{
    Oid target = lp_pg_relation("attestation");
    const char *selected[] = { "claim", "witness" }, *column = NULL;
    Relation parent = lp_pg_read_parent(target, &column, selected, 2);
    Oid native_id = get_atttype(target, get_attnum(target, "witness"));
    table_close(parent, NoLock);

    const Oid *parts = NULL, *aux = NULL;
    int nparts = lp_pg_canonical_partitions("attestation", &parts, NULL, &aux, NULL);
    Snapshot snap = GetActiveSnapshot();
    bool by_witness = (PG_NARGS() > 0 && !PG_ARGISNULL(0));
    int64 total = 0;

    if (!by_witness) {
        for (int p = 0; p < nparts; p++) {
            CHECK_FOR_INTERRUPTS();
            Relation part = table_open(parts[p], AccessShareLock);
            TableScanDesc scan = table_beginscan(part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                total++;
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            table_endscan(scan);
            table_close(part, NoLock);
        }
        PG_RETURN_INT64(total);
    }

    lp_id witness;
    memcpy(witness.b, PG_GETARG_POINTER(0), sizeof(lp_id));
    lp_idmap *claims = lp_idmap_new();
    PG_TRY();
    {
        for (int p = 0; p < nparts; p++) {
            CHECK_FOR_INTERRUPTS();
            if (!OidIsValid(aux[p]))
                ereport(ERROR, (errmsg("laplace_attestation_count: missing attestation_witness_lineage index")));
            Relation part = table_open(parts[p], AccessShareLock);
            Relation idx = index_open(aux[p], AccessShareLock);
            AttrNumber ac = get_attnum(parts[p], "claim");
            Oid eq_op = get_opfamily_member(idx->rd_opfamily[0], native_id, native_id, BTEqualStrategyNumber);
            ScanKeyData sk;
            ScanKeyEntryInitialize(&sk, 0, 1, BTEqualStrategyNumber, native_id,
                                   idx->rd_indcollation[0], get_opcode(eq_op), PointerGetDatum(&witness));
            IndexScanDesc scan = index_beginscan(part, idx, snap, NULL, 1, 0);
            index_rescan(scan, &sk, 1, NULL, 0);
            TupleTableSlot *slot = table_slot_create(part, NULL);
            while (index_getnext_slot(scan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                bool isnull = true;
                Datum dc = slot_getattr(slot, ac, &isnull);
                if (!isnull) lp_idmap_put(claims, (const lp_id *)DatumGetPointer(dc), NULL);
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            index_endscan(scan);
            index_close(idx, NoLock);
            table_close(part, NoLock);
        }
        total = (int64)lp_idmap_count(claims);
    }
    PG_FINALLY();
    {
        lp_idmap_free(claims);
    }
    PG_END_TRY();
    PG_RETURN_INT64(total);
}
