/* Recomposition and path realization: entities back to their text, display
 * labels, claim previews, reference resolution, and constituent outputs. */
#include "pg_core.h"
#include "geo.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

typedef struct { size_t node; uint32 run; } TChild;
typedef struct { TChild *kid; size_t nkid; bool have, rendering, tuple, opaque, claim, scalar; uint8 text_state; } TNode;
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
    TChild *kid=MemoryContextAlloc(front->keep,sizeof(TChild)*(p.n?p.n:1));
    for(size_t i=0;i<p.n;i++){lp_id child=lp_path_id(p,i);kid[i]=(TChild){tnode(front->nodes,&child),lp_path_run(p,i)};}
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

typedef struct {size_t node,child;} SurfaceFrame;
static bool text_surface(lp_idmap *t,size_t i){
    TNode *root=lp_idmap_at(t,i);
    if(root->text_state==1 || root->text_state==2)return root->text_state==1;
    if(root->opaque || (root->tuple && !root->scalar)){root->text_state=2;return false;}
    if(root->tuple)return false;
    if(root->text_state==3)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_labels: cyclic composition")));
    if(!root->nkid){root->text_state=lp_tier0_codepoint(lp_pg_tier0(),lp_idmap_key(t,i))>=0?1:2;return root->text_state==1;}
    lp_vec(SurfaceFrame) stack={0};lp_push(&stack,((SurfaceFrame){i,0}));
    while(stack.n){
        CHECK_FOR_INTERRUPTS();SurfaceFrame *frame=stack.v+stack.n-1;
        TNode *node=lp_idmap_at(t,frame->node);
        if(node->text_state==1 || node->text_state==2){stack.n--;continue;}
        if(node->opaque || (node->tuple && !node->scalar)){node->text_state=2;continue;}
        if(!node->nkid){
            node->text_state=lp_tier0_codepoint(lp_pg_tier0(),lp_idmap_key(t,frame->node))>=0?1:2;continue;
        }
        node->text_state=3;
        if(frame->child==node->nkid){node->text_state=1;continue;}
        size_t child=node->kid[frame->child].node;TNode *next=lp_idmap_at(t,child);
        if(next->text_state==3)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_labels: cyclic composition")));
        if(next->text_state==2){node->text_state=2;continue;}
        if(next->text_state==1){frame->child++;continue;}
        lp_push(&stack,((SurfaceFrame){child,0}));
    }
    lp_vec_free(&stack);return ((TNode *)lp_idmap_at(t,i))->text_state==1;
}

typedef struct {size_t node,child;uint32 repeat,suffix;bool labels,entered;} RenderFrame;
/* Exact text and display labels share the occurrence stack. Structural labels
 * print a run once with its count; exact text emits every ordered occurrence.
 * Neither DAG altitude nor output length consumes the backend's C stack. */
static void render_tree(lp_idmap *t,size_t root,StringInfo out,bool labels){
    lp_vec(RenderFrame) stack={0};lp_push(&stack,((RenderFrame){.node=root,.labels=labels}));
    while(stack.n){
        CHECK_FOR_INTERRUPTS();RenderFrame *frame=stack.v+stack.n-1;
        TNode *node=lp_idmap_at(t,frame->node);
        if(!frame->entered){
            bool structured=frame->labels && (node->tuple || (node->opaque && node->nkid>0)) && !text_surface(t,frame->node);
            if(frame->labels && !structured){
                if(!text_surface(t,frame->node)){
                    char hex[33];lp_id_hex(lp_idmap_key(t,frame->node),hex);appendStringInfoString(out,hex);stack.n--;continue;
                }
                frame->labels=false;
            }
            if(!frame->labels && !node->nkid){
                int64 cp=lp_tier0_codepoint(lp_pg_tier0(),lp_idmap_key(t,frame->node));
                if(cp>=0){uint8 u[4];appendBinaryStringInfo(out,(const char *)u,(int)lp_utf8_put((uint32)cp,u));}
                stack.n--;continue;
            }
            if(node->rendering)ereport(ERROR,(errcode(ERRCODE_DATA_EXCEPTION),errmsg("laplace_text: cyclic composition")));
            node->rendering=true;frame->entered=true;
            if(frame->labels)appendStringInfoChar(out,'[');
        }
        if(frame->labels){
            if(frame->suffix>1)appendStringInfo(out," × %u",frame->suffix);
            frame->suffix=0;
            if(frame->child<node->nkid){
                if(frame->child)appendStringInfoString(out," · ");
                TChild child=node->kid[frame->child++];frame->suffix=child.run;
                lp_push(&stack,((RenderFrame){.node=child.node,.labels=true}));continue;
            }
            appendStringInfoChar(out,']');
        }else{
            if(!frame->repeat && frame->child<node->nkid)frame->repeat=node->kid[frame->child++].run;
            if(frame->repeat){
                size_t child=node->kid[frame->child-1].node;frame->repeat--;
                lp_push(&stack,((RenderFrame){.node=child}));continue;
            }
            if(frame->child<node->nkid)continue;
        }
        node->rendering=false;stack.n--;
    }
    lp_vec_free(&stack);
}

static void render(lp_idmap *t,size_t i,StringInfo out){render_tree(t,i,out,false);}
static void render_label(lp_idmap *t,size_t i,StringInfo out){render_tree(t,i,out,true);}

PG_FUNCTION_INFO_V1(laplace_text);
Datum laplace_text(PG_FUNCTION_ARGS){
    StringInfoData out; initStringInfo(&out); MemoryContext keep = CurrentMemoryContext;
    lp_idmap *t = lp_idmap_sized(sizeof(TNode)); size_t root = tnode(t, (const lp_id *)PG_GETARG_POINTER(0));
    PG_TRY();{
      read_levels(t,keep,false,NULL);
      render(t,root,&out);
    }PG_FINALLY();{lp_idmap_free(t);}PG_END_TRY();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}

static Datum realize_batch(FunctionCallInfo fcinfo,bool labels){
    TupleDesc td;Tuplestorestate *ts=set_begin(fcinfo,labels ? "laplace_labels" : "laplace_text_batch",&td);
    MemoryContext keep=CurrentMemoryContext;
    int n;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&n);
    lp_idmap *t=lp_idmap_sized(sizeof(TNode));size_t *roots=palloc(Max(n,1)*sizeof(size_t));
    PG_TRY();{
      for(int i=0;i<n;i++)roots[i]=tnode(t,ids+i);
      read_levels(t,keep,labels,NULL);
      for(int i=0;i<n;i++){
        StringInfoData out;initStringInfo(&out);
        if(labels)render_label(t,roots[i],&out);else render(t,roots[i],&out);
        text *content=cstring_to_text_with_len(out.data,out.len);
        Datum values[]={Int64GetDatum((int64)i+1),PointerGetDatum(ids+i),PointerGetDatum(content)};
        bool nulls[]={false,false,false};set_put(fcinfo,ts,td,values,nulls);
        pfree(content);pfree(out.data);
      }
    }PG_FINALLY();{lp_idmap_free(t);}PG_END_TRY();
    pfree(roots);pfree(ids);PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_text_batch);
Datum laplace_text_batch(PG_FUNCTION_ARGS){return realize_batch(fcinfo,false);}
PG_FUNCTION_INFO_V1(laplace_labels);
Datum laplace_labels(PG_FUNCTION_ARGS){return realize_batch(fcinfo,true);}

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
    lp_idmap *tree=lp_idmap_sized(sizeof(TNode));MemoryContext keep=CurrentMemoryContext;
    PG_TRY();{
        for(int i=0;i<count;i++){
            if(present[i] && unlabeled[i])tnode(tree,ids+i);
        }
        read_levels(tree,keep,true,NULL);
        for(int i=0;i<count;i++){
            Datum label=surfaces[i];StringInfoData text;bool rendered=false;
            if(unlabeled[i]){
                initStringInfo(&text);rendered=true;
                if(present[i])render_label(tree,(size_t)lp_idmap_find(tree,ids+i),&text);
                else{char hex[33];lp_id_hex(ids+i,hex);appendStringInfoString(&text,hex);}
                label=PointerGetDatum(cstring_to_text_with_len(text.data,text.len));
            }
            Datum output[]={Int64GetDatum((int64)i+1),values[i],label,BoolGetDatum(present[i])};bool nulls[]={false,false,false,false};
            set_put(fcinfo,rows,descriptor,output,nulls);
            if(rendered){pfree(DatumGetPointer(label));pfree(text.data);}
        }
    }PG_FINALLY();{lp_idmap_free(tree);}PG_END_TRY();
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
    lp_idmap *tree=lp_idmap_sized(sizeof(TNode)),*selected=lp_idmap_new();
    PG_TRY();{
        for(int i=0;i<count;i++){tnode(tree,ids+i);lp_idmap_put(selected,ids+i,NULL);}
        read_levels(tree,CurrentMemoryContext,true,selected);
        for(int i=0;i<count;i++){
            CHECK_FOR_INTERRUPTS();TNode *node=lp_idmap_at(tree,(size_t)lp_idmap_find(tree,ids+i));
            if(!node->claim || !node->nkid)continue;
            uint64 arity=0;for(size_t k=0;k<node->nkid;k++)arity+=node->kid[k].run;
            size_t last=node->kid[node->nkid-1].node;
            size_t predicate=arity==3?node->kid[node->kid[0].run>=2?0:1].node:SIZE_MAX;
            StringInfoData type,fact;initStringInfo(&type);initStringInfo(&fact);
            if(predicate!=SIZE_MAX)render_label(tree,predicate,&type);
            if(arity<=3)render_label(tree,last,&fact);
            else for(size_t k=0;k<node->nkid;k++){
                if(k)appendStringInfoString(&fact," · ");
                render_label(tree,node->kid[k].node,&fact);
                if(node->kid[k].run>1)appendStringInfo(&fact," × %u",node->kid[k].run);
            }
            text *type_text=cstring_to_text_with_len(type.data,type.len),*fact_text=cstring_to_text_with_len(fact.data,fact.len);
            Datum values[]={Int64GetDatum((int64)i+1),PointerGetDatum(ids+i),
                predicate==SIZE_MAX?(Datum)0:PointerGetDatum(lp_idmap_key(tree,predicate)),
                PointerGetDatum(arity<=3?lp_idmap_key(tree,last):ids+i),PointerGetDatum(type_text),PointerGetDatum(fact_text)};
            bool nulls[]={false,false,predicate==SIZE_MAX,false,false,false};set_put(fcinfo,rows,descriptor,values,nulls);
            pfree(type_text);pfree(fact_text);pfree(type.data);pfree(fact.data);
        }
    }PG_FINALLY();{lp_idmap_free(tree);lp_idmap_free(selected);}PG_END_TRY();
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
static void constituent_output_row(const lp_id *id,Datum path,Datum mask,void *context){
    PathOutput *output=context;Geo geometry=geo_of(path);lp_path p=geometry.p;
    if(p.n>PG_INT32_MAX)ereport(ERROR,(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),errmsg("too many packed constituents")));
    for(size_t i=0;i<p.n;i++){
        CHECK_FOR_INTERRUPTS();lp_id child=lp_path_id(p,i);double m;
        memcpy(&m,p.v+i*4*sizeof(double)+3*sizeof(double),sizeof m);
        Datum values[]={id_datum(id),Int32GetDatum((int32)i+1),id_datum(&child),
            Int32GetDatum((int32)lp_path_run(p,i)),Int64GetDatum((int64)lp_m_bits(m))};
        bool nulls[]={false,false,false,false,false};
        set_put(output->call,output->rows,output->descriptor,values,nulls);
    }
}
PG_FUNCTION_INFO_V1(laplace_constituents);
Datum laplace_constituents(PG_FUNCTION_ARGS){
    TupleDesc descriptor;Tuplestorestate *rows=set_begin(fcinfo,"laplace_constituents",&descriptor);
    int count;lp_id *ids=ids_of(PG_GETARG_ARRAYTYPE_P(0),&count);lp_idmap *unique=lp_idmap_new();
    PG_TRY();{
        for(int i=0;i<count;i++)lp_idmap_put(unique,ids+i,NULL);
        PathOutput output={fcinfo,rows,descriptor};
        lp_pg_lookup_paths(lp_idmap_keys(unique),lp_idmap_count(unique),false,constituent_output_row,&output);
    }PG_FINALLY();{lp_idmap_free(unique);}PG_END_TRY();
    pfree(ids);PG_RETURN_NULL();
}
