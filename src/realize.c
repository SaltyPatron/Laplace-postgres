/* Recomposition and path realization: entities back to their text, display
 * labels, claim previews, reference resolution, and constituent outputs. */
#include "pg_core.h"
#include "geo.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

typedef struct { lp_realize_child *kid; size_t nkid; bool have, tuple, opaque, claim, scalar; } TNode;
static size_t tnode(lp_idmap *t, const lp_id *id){
    bool fresh; size_t i = lp_idmap_put(t, id, &fresh);
    if (fresh) *(TNode *)lp_idmap_at(t,i) = (TNode){ .have=lp_tier0_codepoint(lp_pg_tier0(),id)>=0 };
    return i;
}

typedef struct { lp_idmap *nodes; const lp_idmap *inspect; MemoryContext keep; bool labels; lp_mask tuple,opaque,claim,file; size_t found; } TextFrontier;
static void text_frontier_row(const lp_id *id,Datum path,Datum mask,void *context){
    TextFrontier *front=context;front->found++;
    lp_mask carrier=front->labels?lp_pg_mask_native(mask):0;
    bool tuple=(carrier&front->tuple)!=0,opaque=!tuple && (carrier&front->opaque)!=0;
    bool claim=(carrier&front->claim)!=0,file=(carrier&front->file)!=0;
    bool inspect=claim && front->inspect && lp_idmap_find(front->inspect,id)>=0;
    Geo geometry=geo_of(path);lp_path p=geometry.p;
    /* A row role is not an exclusive modality. The same scalar composition
     * can also have been used as a record, claim, or tuple (for example, "en" or "01").
     * Its actual Unicode surface remains readable in every containing path. */
    bool scalar_text=p.n>0;
    for(size_t i=0;i<p.n;i++){
        lp_id child=lp_path_id(p,i);
        if(lp_tier0_codepoint(lp_pg_tier0(),&child)<0){scalar_text=false;break;}
    }
    if(opaque && scalar_text)opaque=false;
    if(opaque && !inspect && (file || p.n==0 || p.n>64)){
        TNode *me=lp_idmap_at(front->nodes,tnode(front->nodes,id));me->have=true;me->opaque=true;return;
    }
    lp_realize_child *kid=MemoryContextAlloc(front->keep,sizeof(lp_realize_child)*(p.n?p.n:1));
    for(size_t i=0;i<p.n;i++){
        kid[i]=(lp_realize_child){lp_path_id(p,i),lp_path_run(p,i),lp_path_said(p,i)};
        tnode(front->nodes,&kid[i].id);
    }
    TNode *me=lp_idmap_at(front->nodes,tnode(front->nodes,id));
    me->kid=kid;me->nkid=p.n;me->have=true;me->tuple=tuple;me->opaque=opaque;me->claim=claim;me->scalar=scalar_text;
}

static void read_levels(lp_idmap *t, MemoryContext keep, bool labels,const lp_idmap *inspect){
    TextFrontier front={.nodes=t,.inspect=inspect,.keep=keep,.labels=labels};
    if(labels){
        const char *roles[]={"tuple","file","record","claim"};
        for(size_t i=0;i<lengthof(roles);i++){
            lp_mask value=lp_pg_kind_mask(roles[i]);
            if(i==0)front.tuple=value;else front.opaque|=value;
            if(i==1)front.file=value;
            if(i==3)front.claim=value;
        }
    }
    size_t begin=0,end=lp_idmap_count(t),window=frontier_window_ids();
    while(begin<end){
      /* Only newly discovered nodes form the next frontier. Shared descendants
       * of every requested root are read once in this Native context. */
      for(size_t at=begin;at<end;){
        size_t stop=at+Min(window,end-at),nwant=0;
        lp_id *want=palloc((stop-at)*sizeof(lp_id));
        for(;at<stop;at++)if(!((TNode *)lp_idmap_at(t,at))->have)want[nwant++]=*lp_idmap_key(t,at);
        if(!nwant){pfree(want);continue;}
        front.found=0;lp_pg_lookup_paths(want,nwant,labels,text_frontier_row,&front);
        if(!labels && front.found!=nwant)ereport(ERROR,(errmsg("laplace_text: no physicality for an entity")));
        if(labels)for(size_t i=0;i<nwant;i++){
          TNode *me=lp_idmap_at(t,(size_t)lp_idmap_find(t,want+i));
          if(!me->have){me->have=true;me->opaque=true;}
        }
        pfree(want);
      }
      begin=end;end=lp_idmap_count(t);
    }
}

/* The walk itself is Native's (lp_realize): exact text and display labels,
 * runs, what is no part of text, and the cycle check. This file owns the
 * fetch: one frontier a DAG level, read before anything is rendered. */
typedef struct { lp_idmap *nodes; StringInfo out; } TextSink;
static int text_node(void *context,const lp_id *id,lp_realize_node *out){
    CHECK_FOR_INTERRUPTS();
    lp_idmap *nodes=((TextSink *)context)->nodes;int64 at=lp_idmap_find(nodes,id);
    if(at<0)return LP_REALIZE_ABSENT;
    TNode *node=lp_idmap_at(nodes,(size_t)at);
    if(!node->have)return LP_REALIZE_ABSENT;
    out->child=node->kid;out->n=node->nkid;
    out->flags=(node->tuple?LP_REALIZE_TUPLE:0)|(node->opaque?LP_REALIZE_OPAQUE:0)|(node->scalar?LP_REALIZE_SCALAR:0);
    return LP_REALIZE_KNOWN;
}
static int text_bytes(void *context,const uint8_t *bytes,size_t n){
    appendBinaryStringInfo(((TextSink *)context)->out,(const char *)bytes,(int)n);return 0;
}
static void render_mode(lp_realizer *walk,lp_idmap *t,const lp_id *id,StringInfo out,uint32 mode){
    TextSink sink={t,out};lp_realize_result result;
    if(!lp_realize(walk,lp_pg_tier0(),id,mode,text_node,text_bytes,&sink,&result))
        ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),
            errmsg(mode==LP_REALIZE_LABELS?"laplace_labels: cyclic composition":"laplace_text: cyclic composition")));
}
static void render(lp_realizer *walk,lp_idmap *t,const lp_id *id,StringInfo out){render_mode(walk,t,id,out,LP_REALIZE_TEXT);}
static void render_label(lp_realizer *walk,lp_idmap *t,const lp_id *id,StringInfo out){render_mode(walk,t,id,out,LP_REALIZE_LABELS);}

static int utf8_truncate_bytes(const char *s, int len, int32 max_bytes)
{
    if (max_bytes <= 0 || len <= max_bytes) return len;
    int cut = (int)max_bytes;
    while (cut > 0 && (((unsigned char)s[cut] & 0xC0u) == 0x80u)) cut--;
    return cut;
}

void lp_pg_realize_labels_map(const lp_id *ids, size_t count, int32 max_text,
                              bool exact_text, MemoryContext ctx, lp_idmap *out_map)
{
    if (!count || !out_map) return;
    MemoryContext keep = AllocSetContextCreate(CurrentMemoryContext, "lp_pg_realize_labels_map", ALLOCSET_DEFAULT_SIZES);
    lp_idmap *t = lp_idmap_sized(sizeof(TNode));
    lp_realizer *walk = lp_realizer_new();
    PG_TRY();
    {
        for (size_t i = 0; i < count; i++) {
            if (lp_idmap_find(out_map, &ids[i]) < 0) tnode(t, &ids[i]);
        }
        if (lp_idmap_count(t) > 0) {
            read_levels(t, keep, !exact_text, NULL);
            for (size_t i = 0; i < count; i++) {
                bool fresh = false;
                PgLabelEntry *ent = lp_idmap_get(out_map, &ids[i], &fresh);
                if (!fresh) continue;
                StringInfoData out;
                initStringInfo(&out);
                if (exact_text) render(walk, t, &ids[i], &out);
                else render_label(walk, t, &ids[i], &out);
                bool is_hex = false;
                if (out.len == 32) {
                    char hex[33];
                    lp_id_hex(&ids[i], hex);
                    if (memcmp(out.data, hex, 32) == 0) is_hex = true;
                }
                int cut = utf8_truncate_bytes(out.data, out.len, max_text);
                char *copy = MemoryContextAlloc(ctx, (Size)cut + 1);
                if (cut > 0) memcpy(copy, out.data, (size_t)cut);
                copy[cut] = '\0';
                ent->text = copy;
                ent->len = cut;
                ent->is_hex = is_hex;
                pfree(out.data);
            }
        }
    }
    PG_FINALLY();
    {
        lp_idmap_free(t);
        lp_realizer_free(walk);
        MemoryContextDelete(keep);
    }
    PG_END_TRY();
}

PG_FUNCTION_INFO_V1(laplace_text);
Datum laplace_text(PG_FUNCTION_ARGS){
    StringInfoData out; initStringInfo(&out); MemoryContext keep = CurrentMemoryContext;
    const lp_id *id = (const lp_id *)PG_GETARG_POINTER(0);
    lp_idmap *t = lp_idmap_sized(sizeof(TNode)); lp_realizer *walk = lp_realizer_new(); tnode(t, id);
    PG_TRY();{
      read_levels(t,keep,false,NULL);
      render(walk,t,id,&out);
    }PG_FINALLY();{lp_idmap_free(t);lp_realizer_free(walk);}PG_END_TRY();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}

static Datum realize_batch(FunctionCallInfo fcinfo,bool labels){
    TupleDesc td;Tuplestorestate *ts=set_begin(fcinfo,labels ? "laplace_labels" : "laplace_text_batch",&td);
    MemoryContext keep=CurrentMemoryContext;
    int n;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n);
    lp_idmap *t=lp_idmap_sized(sizeof(TNode));lp_realizer *walk=lp_realizer_new();
    PG_TRY();{
      for(int i=0;i<n;i++)tnode(t,ids+i);
      read_levels(t,keep,labels,NULL);
      for(int i=0;i<n;i++){
        StringInfoData out;initStringInfo(&out);
        if(labels)render_label(walk,t,ids+i,&out);else render(walk,t,ids+i,&out);
        text *content=cstring_to_text_with_len(out.data,out.len);
        Datum values[]={Int64GetDatum((int64)i+1),PointerGetDatum(ids+i),PointerGetDatum(content)};
        bool nulls[]={false,false,false};set_put(fcinfo,ts,td,values,nulls);
        pfree(content);pfree(out.data);
      }
    }PG_FINALLY();{lp_idmap_free(t);lp_realizer_free(walk);}PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_text_batch);
Datum laplace_text_batch(PG_FUNCTION_ARGS){return realize_batch(fcinfo,false);}
PG_FUNCTION_INFO_V1(laplace_labels);
Datum laplace_labels(PG_FUNCTION_ARGS){return realize_batch(fcinfo,true);}

/* Single-ID display label directly on the Native DAG (NULL when input is NULL,
 * or when null_if_hex is requested and the ID has no realized label). */
PG_FUNCTION_INFO_V1(laplace_label_text);
Datum laplace_label_text(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    bytea *b = PG_GETARG_BYTEA_PP(0);
    if (VARSIZE_ANY_EXHDR(b) != sizeof(lp_id))
        ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                        errmsg("a blake3 ID is 16 bytes (got %zu)", (size_t)VARSIZE_ANY_EXHDR(b))));
    bool null_if_hex = (PG_NARGS() > 1 && !PG_ARGISNULL(1)) ? PG_GETARG_BOOL(1) : false;
    lp_id id;
    memcpy(id.b, VARDATA_ANY(b), sizeof(lp_id));

    MemoryContext keep = CurrentMemoryContext;
    lp_idmap *t = lp_idmap_sized(sizeof(TNode));
    lp_realizer *walk = lp_realizer_new();
    StringInfoData out;
    initStringInfo(&out);
    text *res = NULL;
    PG_TRY();
    {
        tnode(t, &id);
        read_levels(t, keep, true, NULL);
        render_label(walk, t, &id, &out);
        bool suppress = false;
        if (null_if_hex && out.len == 32) {
            char hex[33];
            lp_id_hex(&id, hex);
            if (memcmp(out.data, hex, 32) == 0) suppress = true;
        }
        if (!suppress) res = cstring_to_text_with_len(out.data, out.len);
    }
    PG_FINALLY();
    {
        lp_idmap_free(t);
        lp_realizer_free(walk);
        pfree(out.data);
    }
    PG_END_TRY();
    if (!res) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(res);
}

/* Batch display realization preserving exact input ordinals and NULL slots. */
PG_FUNCTION_INFO_V1(laplace_label_array);
Datum laplace_label_array(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0)) PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));
    ArrayType *arr = PG_GETARG_ARRAYTYPE_P(0);
    if (ARR_NDIM(arr) > 1)
        ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), errmsg("laplace_label_array: expected 1-D bytea[]")));
    bool null_if_hex = (PG_NARGS() > 1 && !PG_ARGISNULL(1)) ? PG_GETARG_BOOL(1) : false;

    Datum *elems = NULL;
    bool *nulls = NULL;
    int count = 0;
    deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT, &elems, &nulls, &count);
    if (count <= 0) {
        if (elems) pfree(elems);
        if (nulls) pfree(nulls);
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));
    }

    MemoryContext keep = CurrentMemoryContext;
    lp_id *ids = palloc0(sizeof(lp_id) * (Size)count);
    bool *valid = palloc0(sizeof(bool) * (Size)count);
    Datum *out_elems = palloc0(sizeof(Datum) * (Size)count);
    bool *out_nulls = palloc0(sizeof(bool) * (Size)count);
    lp_idmap *t = lp_idmap_sized(sizeof(TNode));
    lp_idmap *cache = lp_idmap_sized(sizeof(Datum));
    lp_realizer *walk = lp_realizer_new();

    PG_TRY();
    {
        for (int i = 0; i < count; i++) {
            if (nulls[i]) { out_nulls[i] = true; continue; }
            bytea *b = DatumGetByteaPP(elems[i]);
            if (VARSIZE_ANY_EXHDR(b) != sizeof(lp_id))
                ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                                errmsg("a blake3 ID is 16 bytes (got %zu)", (size_t)VARSIZE_ANY_EXHDR(b))));
            memcpy(ids[i].b, VARDATA_ANY(b), sizeof(lp_id));
            valid[i] = true;
            tnode(t, &ids[i]);
        }
        read_levels(t, keep, true, NULL);
        for (int i = 0; i < count; i++) {
            if (!valid[i]) continue;
            bool fresh = false;
            Datum *cached = lp_idmap_get(cache, &ids[i], &fresh);
            if (fresh) {
                StringInfoData out;
                initStringInfo(&out);
                render_label(walk, t, &ids[i], &out);
                bool suppress = false;
                if (null_if_hex && out.len == 32) {
                    char hex[33];
                    lp_id_hex(&ids[i], hex);
                    if (memcmp(out.data, hex, 32) == 0) suppress = true;
                }
                *cached = suppress ? (Datum)0 : PointerGetDatum(cstring_to_text_with_len(out.data, out.len));
                pfree(out.data);
            }
            if (*cached == (Datum)0) {
                out_nulls[i] = true;
            } else {
                out_elems[i] = *cached;
                out_nulls[i] = false;
            }
        }
    }
    PG_FINALLY();
    {
        lp_idmap_free(cache);
        lp_idmap_free(t);
        lp_realizer_free(walk);
    }
    PG_END_TRY();

    int dims[1] = { count }, lbs[1] = { 1 };
    ArrayType *res = construct_md_array(out_elems, out_nulls, 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT);
    pfree(ids); pfree(valid); pfree(out_elems); pfree(out_nulls);
    pfree(elems); pfree(nulls);
    PG_RETURN_ARRAYTYPE_P(res);
}

/* Reference preparation is one Native operation: preserve client ID/surface
 * positions, probe the complete identity set, then materialize only missing
 * labels for present identities. Text is never used to elect another entity. */
PG_FUNCTION_INFO_V1(laplace_resolve_refs);
Datum laplace_resolve_refs(PG_FUNCTION_ARGS){
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_resolve_refs",&descriptor);
    if(PG_ARGISNULL(0) && PG_ARGISNULL(1))PG_RETURN_NULL();
    if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("reference identities and surfaces must retain the same non-NULL ID positions")));
    ArrayType *identities=PG_GETARG_ARRAYTYPE_P(0),*labels=PG_GETARG_ARRAYTYPE_P(1);
    Datum *values,*surfaces;bool *missing,*unlabeled;int count,nlabels;
    deconstruct_array(identities,BYTEAOID,-1,false,TYPALIGN_INT,&values,&missing,&count);
    deconstruct_array(labels,TEXTOID,-1,false,TYPALIGN_INT,&surfaces,&unlabeled,&nlabels);
    if(count!=nlabels || ARR_NDIM(identities)>1 || ARR_NDIM(labels)>1)
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("reference identities and surfaces must retain the same non-NULL ID positions")));
    lp_id *ids=palloc(sizeof(*ids)*Max(count,1));bool *present=palloc0(sizeof(*present)*Max(count,1));
    for(int i=0;i<count;i++){
        if(missing[i])ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("reference identities and surfaces must retain the same non-NULL ID positions")));
        bytea *value=DatumGetByteaPP(values[i]);
        if(VARSIZE_ANY_EXHDR(value)!=sizeof(lp_id))ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("Native reference ID must contain exactly 16 bytes")));
        memcpy(ids[i].b,VARDATA_ANY(value),sizeof(lp_id));
        present[i]=lp_tier0_codepoint(lp_pg_tier0(),ids+i)>=0;
    }
    lp_pg_present(ids,(size_t)count,present);
    lp_idmap *tree=lp_idmap_sized(sizeof(TNode));MemoryContext keep=CurrentMemoryContext;lp_realizer *walk=lp_realizer_new();
    PG_TRY();{
        for(int i=0;i<count;i++){
            if(present[i] && unlabeled[i])tnode(tree,ids+i);
        }
        read_levels(tree,keep,true,NULL);
        for(int i=0;i<count;i++){
            Datum label=surfaces[i];StringInfoData text;bool rendered=false;
            if(unlabeled[i]){
                initStringInfo(&text);rendered=true;
                if(present[i])render_label(walk,tree,ids+i,&text);
                else{char hex[33];lp_id_hex(ids+i,hex);appendStringInfoString(&text,hex);}
                label=PointerGetDatum(cstring_to_text_with_len(text.data,text.len));
            }
            Datum output[]={Int64GetDatum((int64)i+1),values[i],label,BoolGetDatum(present[i])};bool nulls[]={false,false,false,false};
            set_put(fcinfo,rows,descriptor,output,nulls);
            if(rendered){pfree(DatumGetPointer(label));pfree(text.data);}
        }
    }PG_FINALLY();{lp_idmap_free(tree);lp_realizer_free(walk);}PG_END_TRY();
    pfree(ids);pfree(present);pfree(values);pfree(surfaces);pfree(missing);pfree(unlabeled);PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_resolve_ref);
Datum laplace_resolve_ref(PG_FUNCTION_ARGS){
    text *input=PG_GETARG_TEXT_PP(0);lp_id id;bool present=false;
    if(VARSIZE_ANY_EXHDR(input)==32){char hex[33];memcpy(hex,VARDATA_ANY(input),32);hex[32]=0;
        if(!lp_id_unhex(hex,&id))id=trunk_of(input,NULL,0,NULL).id;
    }else{if(!VARSIZE_ANY_EXHDR(input))PG_RETURN_NULL();id=trunk_of(input,NULL,0,NULL).id;}
    present=lp_tier0_codepoint(lp_pg_tier0(),&id)>=0;
    lp_pg_present(&id,1,&present);
    if(!present)PG_RETURN_NULL();
    PG_RETURN_BYTEA_P(bytes_datum(id.b,sizeof id));
}

/* Selected claims use the same path frontier and display reader as labels.
 * Only elected claim roots are opened; structural descendants retain their
 * existing label policy. Runs remain compressed throughout inspection. */
PG_FUNCTION_INFO_V1(laplace_claim_preview);
Datum laplace_claim_preview(PG_FUNCTION_ARGS){
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_claim_preview",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    lp_idmap *tree=lp_idmap_sized(sizeof(TNode)),*selected=lp_idmap_new();lp_realizer *walk=lp_realizer_new();
    PG_TRY();{
        for(int i=0;i<count;i++){tnode(tree,ids+i);lp_idmap_put(selected,ids+i,NULL);}
        read_levels(tree,CurrentMemoryContext,true,selected);
        for(int i=0;i<count;i++){
            CHECK_FOR_INTERRUPTS();TNode *node=lp_idmap_at(tree,(size_t)lp_idmap_find(tree,ids+i));
            if(!node->claim || !node->nkid)continue;
            uint64 arity=0;for(size_t k=0;k<node->nkid;k++)arity+=node->kid[k].run;
            const lp_id *last=&node->kid[node->nkid-1].id;
            const lp_id *predicate=arity==3?&node->kid[node->kid[0].run>=2?0:1].id:NULL;
            StringInfoData type,fact;initStringInfo(&type);initStringInfo(&fact);
            if(predicate)render_label(walk,tree,predicate,&type);
            if(arity<=3)render_label(walk,tree,last,&fact);
            else for(size_t k=0;k<node->nkid;k++){
                if(k)appendStringInfoString(&fact," · ");
                render_label(walk,tree,&node->kid[k].id,&fact);
                if(node->kid[k].run>1)appendStringInfo(&fact," × %u",node->kid[k].run);
            }
            text *type_text=cstring_to_text_with_len(type.data,type.len),*fact_text=cstring_to_text_with_len(fact.data,fact.len);
            Datum values[]={Int64GetDatum((int64)i+1),PointerGetDatum(ids+i),
                predicate?PointerGetDatum(predicate):(Datum)0,
                PointerGetDatum(arity<=3?last:ids+i),PointerGetDatum(type_text),PointerGetDatum(fact_text)};
            bool nulls[]={false,false,predicate==NULL,false,false,false};set_put(fcinfo,rows,descriptor,values,nulls);
            pfree(type_text);pfree(fact_text);pfree(type.data);pfree(fact.data);
        }
    }PG_FINALLY();{lp_idmap_free(tree);lp_idmap_free(selected);lp_realizer_free(walk);}PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}

/* laplace_paths(ids): the paths of a set of entities: one DAG level per call. */
typedef struct { FunctionCallInfo call; Tuplestorestate *rows; TupleDesc descriptor; } PathOutput;
static void path_output_row(const lp_id *id,Datum path,Datum mask,void *context){
    PathOutput *output=context;Datum values[]={id_datum(id),path};bool nulls[]={false,false};
    set_put(output->call,output->rows,output->descriptor,values,nulls);
}
PG_FUNCTION_INFO_V1(laplace_paths);
Datum laplace_paths(PG_FUNCTION_ARGS){
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_paths",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);lp_idmap *unique=lp_idmap_new();
    PG_TRY();{
        for(int i=0;i<count;i++)lp_idmap_put(unique,ids+i,NULL);
        PathOutput output={fcinfo,rows,descriptor};
        lp_pg_lookup_paths(lp_idmap_keys(unique),lp_idmap_count(unique),false,path_output_row,&output);
    }PG_FINALLY();{lp_idmap_free(unique);}PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}

/* Ordered packed constituents of a set, decoded by the owner of the Native
 * carrier. Generic PostGIS/legacy mantissa decoders do not share this layout. */
typedef struct { PathOutput output; int32 depth; lp_idmap *expanded; lp_vec(lp_id) next; } BelowOutput;
static void constituent_rows(PathOutput *output,const lp_id *id,Datum path,Datum mask,BelowOutput *below){
    Geo geometry=geo_of(path);lp_path p=geometry.p;
    bool is_tuple=(lp_pg_mask_native(mask)&lp_pg_kind_mask("tuple"))!=0;
    if(p.n>PG_INT32_MAX)ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("too many packed constituents")));
    for(size_t i=0;i<p.n;i++){
        CHECK_FOR_INTERRUPTS();lp_id child=lp_path_id(p,i);double m;
        memcpy(&m,p.v+i*4*sizeof(double)+3*sizeof(double),sizeof m);
        uint64 bits=lp_m_bits(m);
        if(is_tuple && !lp_m_said(m))bits|=(uint64)LP_SAID_TUPLE<<LP_M_RUN_BITS;
        Datum values[]={id_datum(id),Int32GetDatum((int32)i+1),id_datum(&child),
            Int32GetDatum((int32)lp_path_run(p,i)),Int64GetDatum((int64)bits),Int32GetDatum(below?below->depth:0)};
        bool nulls[]={false,false,false,false,false,false};
        set_put(output->call,output->rows,output->descriptor,values,nulls);
        if(below){bool fresh;lp_idmap_put(below->expanded,&child,&fresh);if(fresh)lp_push(&below->next,child);}
    }
}
static void constituent_output_row(const lp_id *id,Datum path,Datum mask,void *context){constituent_rows(context,id,path,mask,NULL);}
static void below_output_row(const lp_id *id,Datum path,Datum mask,void *context){
    BelowOutput *below=context;constituent_rows(&below->output,id,path,mask,below);
}
PG_FUNCTION_INFO_V1(laplace_constituents);
Datum laplace_constituents(PG_FUNCTION_ARGS){
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_constituents",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);lp_idmap *unique=lp_idmap_new();
    PG_TRY();{
        for(int i=0;i<count;i++)lp_idmap_put(unique,ids+i,NULL);
        PathOutput output={fcinfo,rows,descriptor};
        lp_pg_lookup_paths(lp_idmap_keys(unique),lp_idmap_count(unique),true,constituent_output_row,&output);
    }PG_FINALLY();{lp_idmap_free(unique);}PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}

/* laplace_below(ids, depth): the downward closure of laplace_constituents. Depth 1 is each input's constituents, each
 * later depth the constituents of the children not yet expanded; every parent expanded once, one path read a depth. */
PG_FUNCTION_INFO_V1(laplace_below);
Datum laplace_below(PG_FUNCTION_ARGS){
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_below",&descriptor);
    int32 depth=PG_GETARG_INT32(1);
    if(depth<0)ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),errmsg("laplace_below: depth must not be negative")));
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);
    BelowOutput below={{fcinfo,rows,descriptor},0,lp_idmap_new(),{0}};lp_vec(lp_id) frontier={0};
    PG_TRY();{
        for(int i=0;i<count;i++){bool fresh;lp_idmap_put(below.expanded,ids+i,&fresh);if(fresh)lp_push(&frontier,ids[i]);}
        for(int32 d=1;d<=depth && frontier.n;d++){
            CHECK_FOR_INTERRUPTS();below.depth=d;below.next.n=0;
            lp_pg_lookup_paths(frontier.v,frontier.n,true,below_output_row,&below);
            frontier.n=0;for(size_t i=0;i<below.next.n;i++)lp_push(&frontier,below.next.v[i]);
        }
    } PG_FINALLY();{lp_idmap_free(below.expanded);lp_vec_free(&below.next);lp_vec_free(&frontier);}PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- batched witnesses with realized source names */
typedef struct {
    lp_id witness, lineage, record_id;
    double trust;
    int64 parts;
    bool has_lin, has_path;
} WitnessNamedRow;

typedef struct {
    lp_vec(WitnessNamedRow) rows;
    lp_vec(lp_id) wids;
    lp_idmap *by_w;
    lp_idmap *rec_counts;
} WitnessNamedCollect;

static void witness_named_visit(const lp_id *id, const lp_id *lineage, double trust, void *context){
    WitnessNamedCollect *c = context;
    size_t idx = c->rows.n;
    WitnessNamedRow r = { .witness = *id, .trust = trust, .has_lin = (lineage != NULL) };
    if (lineage) r.lineage = *lineage;
    lp_push(&c->rows, r);
    lp_push(&c->wids, *id);
    *(size_t *)lp_idmap_get(c->by_w, id, NULL) = idx;
}

static void witness_trunk_path(const lp_id *id, Datum path, Datum mask, void *context){
    (void)mask;
    WitnessNamedCollect *c = context;
    size_t *pidx = lp_idmap_lookup(c->by_w, id);
    if (!pidx) return;
    Geo g = geo_of(path);
    if (g.p.n == 0) return;
    WitnessNamedRow *r = &c->rows.v[*pidx];
    r->record_id = lp_path_id(g.p, 0);
    size_t plen = lp_path_len(g.p);
    r->parts = plen > PG_INT64_MAX ? PG_INT64_MAX : (int64)plen;
    r->has_path = true;
    (*(int64 *)lp_idmap_get(c->rec_counts, &r->record_id, NULL))++;
}

PG_FUNCTION_INFO_V1(laplace_witnesses_named);
Datum laplace_witnesses_named(PG_FUNCTION_ARGS){
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_witnesses_named", &td);
    WitnessNamedCollect c = { .by_w = lp_idmap_sized(sizeof(size_t)), .rec_counts = lp_idmap_sized(sizeof(int64)) };
    lp_idmap *t = lp_idmap_sized(sizeof(TNode));
    lp_realizer *walk = lp_realizer_new();
    PG_TRY(); {
        lp_pg_scan_all_witnesses(witness_named_visit, &c);
        if (c.wids.n > 0) {
            lp_pg_lookup_paths(c.wids.v, c.wids.n, false, witness_trunk_path, &c);
            for (size_t i = 0; i < c.rows.n; i++)
                if (c.rows.v[i].has_path) tnode(t, &c.rows.v[i].record_id);
            read_levels(t, CurrentMemoryContext, true, NULL);
        }
        for (size_t i = 0; i < c.rows.n; i++) {
            const WitnessNamedRow *r = &c.rows.v[i];
            StringInfoData out; initStringInfo(&out);
            if (r->has_path) render_label(walk, t, &r->record_id, &out);
            text *name_t = cstring_to_text_with_len(out.data, out.len);
            int64 readings = 0;
            if (r->has_path) {
                int64 *cnt = lp_idmap_lookup(c.rec_counts, &r->record_id);
                if (cnt) readings = *cnt;
            }
            Datum v[7] = {
                id_datum(&r->witness),
                Float8GetDatum(r->trust),
                r->has_lin ? id_datum(&r->lineage) : (Datum)0,
                r->has_path ? id_datum(&r->record_id) : (Datum)0,
                PointerGetDatum(name_t),
                Int64GetDatum(r->parts),
                Int64GetDatum(readings)
            };
            bool nl[7] = { false, false, !r->has_lin, !r->has_path, false, false, false };
            set_put(fcinfo, ts, td, v, nl);
            pfree(DatumGetPointer(v[0]));
            if (r->has_lin) pfree(DatumGetPointer(v[2]));
            if (r->has_path) pfree(DatumGetPointer(v[3]));
            pfree(name_t); pfree(out.data);
        }
    } PG_FINALLY(); {
        lp_idmap_free(t); lp_realizer_free(walk);
        lp_idmap_free(c.by_w); lp_idmap_free(c.rec_counts);
        lp_vec_free(&c.rows); lp_vec_free(&c.wids);
    } PG_END_TRY();
    return (Datum)0;
}

/* ---------------------------------------------------------------- single-pass code AST outline, paths & source text */
typedef lp_vec(lp_id) RealizeIdVec;
typedef struct { lp_vertex *v; size_t n; } CodeAstNode;
typedef struct { lp_idmap *nodes; RealizeIdVec *meta_next; MemoryContext ctx; bool record_roles; } CodeAstCollect;

static void code_ast_visit(const lp_id *id, Datum path, Datum mask, void *context){
    (void)mask;
    CodeAstCollect *c = context;
    bool fresh;
    CodeAstNode *entry = lp_idmap_get(c->nodes, id, &fresh);
    if (!fresh) return;
    Geo g = geo_of(path);
    entry->n = g.p.n;
    entry->v = NULL;
    if (!g.p.n) return;
    entry->v = MemoryContextAlloc(c->ctx, sizeof(lp_vertex) * g.p.n);
    if (lp_path_decode(g.p, entry->v, g.p.n) != g.p.n)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("laplace_code_symbols: invalid stored path")));
    if (c->record_roles && c->meta_next) {
        for (size_t k = 0; k < entry->n; k++) {
            uint32 sd = entry->v[k].said;
            if ((sd == LP_SAID_RECORD || sd == LP_SAID_HOLDS) && lp_idmap_find(c->nodes, &entry->v[k].id) < 0)
                lp_push(c->meta_next, entry->v[k].id);
        }
    }
}

static void code_fetch_missing(CodeAstCollect *c, const lp_id *ids, size_t n){
    if (!n) return;
    lp_idmap *uniq = lp_idmap_new();
    for (size_t i = 0; i < n; i++)
        if (lp_idmap_find(c->nodes, &ids[i]) < 0) lp_idmap_put(uniq, &ids[i], NULL);
    if (lp_idmap_count(uniq) > 0)
        lp_pg_lookup_paths(lp_idmap_keys(uniq), lp_idmap_count(uniq), false, code_ast_visit, c);
    lp_idmap_free(uniq);
}

static void render_exact(lp_realizer *walk, lp_idmap *t, const lp_id *id, StringInfo out){
    if (lp_tier0_codepoint(lp_pg_tier0(), id) >= 0 || lp_idmap_find(t, id) >= 0) {
        render(walk, t, id, out);
        return;
    }
    char hex[33]; lp_id_hex(id, hex);
    appendStringInfoString(out, hex);
}

PG_FUNCTION_INFO_V1(laplace_code_symbols);
Datum laplace_code_symbols(PG_FUNCTION_ARGS){
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_code_symbols", &td);
    if (PG_ARGISNULL(0)) return (Datum)0;
    int nf = 0;
    lp_id *files = ids_of(PG_GETARG_ARRAYTYPE_P(0), &nf);
    if (nf <= 0) { pfree(files); return (Datum)0; }
    bool include_text = PG_NARGS() > 1 && !PG_ARGISNULL(1) ? PG_GETARG_BOOL(1) : true;

    lp_text *tx = lp_pg_text_context();
    lp_id id_pathname = {0}, id_filename = {0}, id_declared = {0};
    if (!lp_text_id(tx, "pathname", 8, &id_pathname) ||
        !lp_text_id(tx, "filename", 8, &id_filename) ||
        !lp_text_id(tx, "declared identifier", 19, &id_declared))
        ereport(ERROR, (errmsg("laplace_code_symbols: tier0 lacks ASCII lookup atoms")));

    MemoryContext caller = CurrentMemoryContext;
    MemoryContext symctx = AllocSetContextCreate(caller, "laplace_code_symbols", ALLOCSET_DEFAULT_SIZES);
    lp_idmap *nodes = lp_idmap_sized(sizeof(CodeAstNode));
    lp_idmap *text_tree = lp_idmap_sized(sizeof(TNode));
    lp_realizer *walk = lp_realizer_new();

    typedef struct { lp_id kind, name, source; } DeclIds;
    typedef struct { lp_id file; lp_vec(lp_id) path_segs; lp_id filename; bool has_path, has_name, is_path_multi, present;
                     lp_vec(lp_id) text_parts; lp_vec(uint32) text_runs; lp_vec(DeclIds) decls; } FileSym;

    PG_TRY(); {
        RealizeIdVec ast_front = {0}, ast_next = {0}, meta_heads = {0}, meta_kids = {0}, path_nodes = {0};
        CodeAstCollect collect = { .nodes = nodes, .meta_next = &ast_next, .ctx = symctx, .record_roles = true };

        for (int i = 0; i < nf; i++) lp_push(&ast_front, files[i]);
        while (ast_front.n > 0) {
            CHECK_FOR_INTERRUPTS();
            ast_next.n = 0;
            code_fetch_missing(&collect, ast_front.v, ast_front.n);
            ast_front.n = 0;
            for (size_t i = 0; i < ast_next.n; i++) lp_push(&ast_front, ast_next.v[i]);
        }

        /* Bounded metadata walk (max depth 8, matching api_cw_file_name) without widening content search. */
        collect.record_roles = false; collect.meta_next = NULL;
        for (int i = 0; i < nf; i++) lp_push(&meta_heads, files[i]);
        for (int depth = 0; depth < 8 && meta_heads.n > 0; depth++) {
            CHECK_FOR_INTERRUPTS();
            code_fetch_missing(&collect, meta_heads.v, meta_heads.n);
            meta_kids.n = 0;
            lp_vec(lp_id) next_heads = {0};
            for (size_t i = 0; i < meta_heads.n; i++) {
                const CodeAstNode *n = lp_idmap_lookup(nodes, &meta_heads.v[i]);
                if (!n || !n->n) continue;
                for (size_t k = 0; k < n->n && k < 64; k++) lp_push(&meta_kids, n->v[k].id);
                lp_push(&next_heads, n->v[0].id);
            }
            code_fetch_missing(&collect, meta_kids.v, meta_kids.n);
            path_nodes.n = 0;
            for (size_t i = 0; i < meta_kids.n; i++) {
                const CodeAstNode *pair = lp_idmap_lookup(nodes, &meta_kids.v[i]);
                if (pair && pair->n == 2 && lp_id_eq(&pair->v[0].id, &id_pathname))
                    lp_push(&path_nodes, pair->v[1].id);
            }
            if (path_nodes.n > 0) code_fetch_missing(&collect, path_nodes.v, path_nodes.n);
            meta_heads.n = 0;
            for (size_t i = 0; i < next_heads.n; i++) lp_push(&meta_heads, next_heads.v[i]);
            lp_vec_free(&next_heads);
        }

        FileSym *fsyms = MemoryContextAllocZero(symctx, sizeof(FileSym) * (size_t)nf);
        RealizeIdVec all_claims = {0}, all_subjects = {0};
        lp_vec(RealizeIdVec) raw_claims = {0};
        typedef struct { lp_id id; uint32 role; } Pend;
        lp_vec(Pend) pend = {0};

        for (int i = 0; i < nf; i++) {
            FileSym *fs = &fsyms[i];
            fs->file = files[i];
            const CodeAstNode *fn = lp_idmap_lookup(nodes, &files[i]);
            if (!fn) continue;
            fs->present = true;

            /* Resolve file_path leaves from metadata chain */
            lp_id at = files[i];
            for (int depth = 0; depth < 8; depth++) {
                const CodeAstNode *kn = lp_idmap_lookup(nodes, &at);
                if (!kn || !kn->n) break;
                lp_id pname = {0}, fname = {0}; bool hp = false, hn = false;
                for (size_t k = 0; k < kn->n && k < 64; k++) {
                    const CodeAstNode *pair = lp_idmap_lookup(nodes, &kn->v[k].id);
                    if (!pair || pair->n != 2) continue;
                    if (!hp && lp_id_eq(&pair->v[0].id, &id_pathname)) { pname = pair->v[1].id; hp = true; }
                    else if (!hn && lp_id_eq(&pair->v[0].id, &id_filename)) { fname = pair->v[1].id; hn = true; }
                }
                if (hp) {
                    const CodeAstNode *pn = (hn && lp_id_eq(&pname, &fname)) ? NULL : lp_idmap_lookup(nodes, &pname);
                    if (pn && pn->n > 0) {
                        for (size_t z = 0; z < pn->n && z < 128; z++) {
                            lp_push(&fs->path_segs, pn->v[z].id);
                            tnode(text_tree, &pn->v[z].id);
                        }
                    } else {
                        lp_push(&fs->path_segs, pname);
                        tnode(text_tree, &pname);
                    }
                    fs->has_path = true;
                    break;
                }
                if (hn) {
                    fs->filename = fname; fs->has_name = true;
                    tnode(text_tree, &fname);
                    break;
                }
                at = kn->v[0].id;
            }

            /* Collect claims in source order (matching fo_collect_claims) */
            pend.n = 0;
            for (size_t k = fn->n; k-- > 0;) {
                uint32 sd = fn->v[k].said;
                if (sd == LP_SAID_RECORD || sd == LP_SAID_HOLDS || sd == LP_SAID_CLAIM)
                    lp_push(&pend, ((Pend){fn->v[k].id, sd}));
            }
            RealizeIdVec fclaims = {0};
            while (pend.n > 0) {
                Pend cur = pend.v[--pend.n];
                if (cur.role == LP_SAID_CLAIM) {
                    lp_push(&fclaims, cur.id);
                    lp_push(&all_claims, cur.id);
                    continue;
                }
                const CodeAstNode *cn = lp_idmap_lookup(nodes, &cur.id);
                size_t marked = 0;
                if (cn) {
                    for (size_t k = cn->n; k-- > 0;) {
                        uint32 sd = cn->v[k].said;
                        if (sd != LP_SAID_RECORD && sd != LP_SAID_HOLDS && sd != LP_SAID_CLAIM) continue;
                        marked++;
                        uint32 run = cn->v[k].run ? cn->v[k].run : 1;
                        while (run--) lp_push(&pend, ((Pend){cn->v[k].id, sd}));
                    }
                }
                if (!marked && cur.role == LP_SAID_RECORD) {
                    lp_push(&fclaims, cur.id);
                    lp_push(&all_claims, cur.id);
                }
            }
            /* Collect file content text leaves (matching fo_tree_text) */
            if (include_text) {
                for (size_t k = 0; k < fn->n; k++) {
                    uint32 sd = fn->v[k].said;
                    if (sd == LP_SAID_METADATA || sd == LP_SAID_CLAIM) continue;
                    if (sd == LP_SAID_RECORD) {
                        const CodeAstNode *rn = lp_idmap_lookup(nodes, &fn->v[k].id);
                        bool has_claim = false;
                        if (rn) for (size_t z = 0; z < rn->n; z++) if (rn->v[z].said == LP_SAID_CLAIM) { has_claim = true; break; }
                        if (!has_claim) continue;
                    }
                    lp_push(&fs->text_parts, fn->v[k].id);
                    lp_push(&fs->text_runs, fn->v[k].run ? fn->v[k].run : 1);
                    tnode(text_tree, &fn->v[k].id);
                }
            }
            lp_push(&raw_claims, fclaims);
        }

        /* Batch-fetch all claims and declared-identifier subjects */
        code_fetch_missing(&collect, all_claims.v, all_claims.n);
        for (size_t i = 0; i < all_claims.n; i++) {
            const CodeAstNode *cn = lp_idmap_lookup(nodes, &all_claims.v[i]);
            if (cn && cn->n == 3 && lp_id_eq(&cn->v[1].id, &id_declared))
                lp_push(&all_subjects, cn->v[0].id);
        }
        code_fetch_missing(&collect, all_subjects.v, all_subjects.n);

        for (int i = 0, ri = 0; i < nf; i++) {
            FileSym *fs = &fsyms[i];
            if (!fs->present) continue;
            RealizeIdVec *fclaims = &raw_claims.v[ri++];
            for (size_t k = 0; k < fclaims->n; k++) {
                const CodeAstNode *cn = lp_idmap_lookup(nodes, &fclaims->v[k]);
                if (!cn || cn->n != 3 || !lp_id_eq(&cn->v[1].id, &id_declared)) continue;
                const CodeAstNode *sn = lp_idmap_lookup(nodes, &cn->v[0].id);
                if (!sn || sn->n != 2) continue;
                DeclIds d = { sn->v[0].id, cn->v[2].id, sn->v[1].id };
                lp_push(&fs->decls, d);
                tnode(text_tree, &d.kind);
                tnode(text_tree, &d.name);
                tnode(text_tree, &d.source);
            }
            lp_vec_free(fclaims);
        }

        /* Realize all needed leaves in one shared text frontier */
        read_levels(text_tree, symctx, false, NULL);

        for (int i = 0; i < nf; i++) {
            FileSym *fs = &fsyms[i];
            if (!fs->present) continue;
            StringInfoData fp; initStringInfo(&fp);
            if (fs->has_path) {
                for (size_t z = 0; z < fs->path_segs.n; z++) {
                    appendStringInfoChar(&fp, '/');
                    render_exact(walk, text_tree, &fs->path_segs.v[z], &fp);
                }
            } else if (fs->has_name) {
                render_exact(walk, text_tree, &fs->filename, &fp);
            }
            StringInfoData stxt; initStringInfo(&stxt);
            if (include_text) {
                for (size_t v = 0; v < fs->text_parts.n; v++) {
                    StringInfoData part; initStringInfo(&part);
                    render_exact(walk, text_tree, &fs->text_parts.v[v], &part);
                    for (uint32 r = 0; r < fs->text_runs.v[v]; r++)
                        appendBinaryStringInfo(&stxt, part.data, part.len);
                    pfree(part.data);
                }
            }
            text *fp_t = cstring_to_text_with_len(fp.data, fp.len);
            text *st_t = include_text ? cstring_to_text_with_len(stxt.data, stxt.len) : NULL;

            if (fs->decls.n == 0) {
                Datum v[7] = {
                    id_datum(&fs->file),
                    PointerGetDatum(fp_t),
                    include_text ? PointerGetDatum(st_t) : (Datum)0,
                    Int32GetDatum(0),
                    (Datum)0, (Datum)0, (Datum)0
                };
                bool nl[7] = { false, false, !include_text, false, true, true, true };
                set_put(fcinfo, ts, td, v, nl);
                pfree(DatumGetPointer(v[0]));
            } else {
                for (size_t d = 0; d < fs->decls.n; d++) {
                    CHECK_FOR_INTERRUPTS();
                    StringInfoData dk, dn, ds;
                    initStringInfo(&dk); initStringInfo(&dn); initStringInfo(&ds);
                    render_exact(walk, text_tree, &fs->decls.v[d].kind, &dk);
                    render_exact(walk, text_tree, &fs->decls.v[d].name, &dn);
                    render_exact(walk, text_tree, &fs->decls.v[d].source, &ds);
                    text *dk_t = cstring_to_text_with_len(dk.data, dk.len);
                    text *dn_t = cstring_to_text_with_len(dn.data, dn.len);
                    text *ds_t = cstring_to_text_with_len(ds.data, ds.len);
                    bool emit_src = (include_text && d == 0);
                    Datum v[7] = {
                        id_datum(&fs->file),
                        PointerGetDatum(fp_t),
                        emit_src ? PointerGetDatum(st_t) : (Datum)0,
                        Int32GetDatum((int32)(d + 1)),
                        PointerGetDatum(dk_t),
                        PointerGetDatum(dn_t),
                        PointerGetDatum(ds_t)
                    };
                    bool nl[7] = { false, false, !emit_src, false, false, false, false };
                    set_put(fcinfo, ts, td, v, nl);
                    pfree(DatumGetPointer(v[0]));
                    pfree(dk_t); pfree(dn_t); pfree(ds_t);
                    pfree(dk.data); pfree(dn.data); pfree(ds.data);
                }
            }
            pfree(fp_t); if (st_t) pfree(st_t);
            pfree(fp.data); pfree(stxt.data);
            lp_vec_free(&fs->path_segs); lp_vec_free(&fs->text_parts);
            lp_vec_free(&fs->text_runs); lp_vec_free(&fs->decls);
        }

        lp_vec_free(&ast_front); lp_vec_free(&ast_next); lp_vec_free(&meta_heads);
        lp_vec_free(&meta_kids); lp_vec_free(&path_nodes);
        lp_vec_free(&all_claims); lp_vec_free(&all_subjects);
        lp_vec_free(&raw_claims); lp_vec_free(&pend);
    } PG_FINALLY(); {
        lp_realizer_free(walk);
        lp_idmap_free(text_tree);
        lp_idmap_free(nodes);
        MemoryContextDelete(symctx);
        pfree(files);
    } PG_END_TRY();
    return (Datum)0;
}
