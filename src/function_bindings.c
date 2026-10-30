/* The Native module owns its SQL namespace boundaries. This registry is
 * installation metadata; primitive and index calls never execute it. */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "pg_core.h"
#include "commands/extension.h"
#include "lib/stringinfo.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/regproc.h"

typedef struct { const char *symbol; bool sql_scope; } FunctionBinding;
static const FunctionBinding bindings[]={
#include "laplace_function_bindings.h"
};

static const char current_api[]=
#include "laplace_current_api.h"
;

/* Native declarations are shared by cold install, version transition and
 * missing-object repair. PostgreSQL parses the signature; no name guessing,
 * body scraping, caller search_path or foreign-object adoption is involved. */
static void restore_declared_api(const char *module,Oid extension)
{
    char *bound=lp_pg_sql(current_api),*literal=quote_literal_cstr(module);
    const char *at=bound,*tag="'MODULE_PATHNAME'",*next;
    StringInfoData sql;initStringInfo(&sql);
    while((next=strstr(at,tag))!=NULL){
        appendBinaryStringInfo(&sql,at,(int)(next-at));appendStringInfoString(&sql,literal);at=next+strlen(tag);
    }
    appendStringInfoString(&sql,at);
    List *declarations=raw_parser(sql.data,RAW_PARSE_DEFAULT);
    foreach_ptr(RawStmt,raw,declarations){
        if(!IsA(raw->stmt,CreateFunctionStmt))elog(ERROR,"Native API declaration is not a function");
        CreateFunctionStmt *function=(CreateFunctionStmt *)raw->stmt;
        if(list_length(function->funcname)!=2 ||
           get_namespace_oid(strVal(linitial(function->funcname)),false)!=lp_pg_schema())
            elog(ERROR,"Native API declaration is outside its installed namespace");
        Oid arguments[FUNC_MAX_ARGS];int count=0;
        foreach_ptr(FunctionParameter,argument,function->parameters){
            if(argument->mode==FUNC_PARAM_OUT || argument->mode==FUNC_PARAM_TABLE)continue;
            if(count==FUNC_MAX_ARGS)elog(ERROR,"Native API declaration has too many arguments");
            arguments[count++]=LookupTypeNameOid(NULL,argument->argType,false);
        }
        Oid procedure=LookupFuncName(function->funcname,count,arguments,true);
        if(OidIsValid(procedure)){
            if(getExtensionOfObject(ProcedureRelationId,procedure)!=extension)
                ereport(ERROR,(errcode(ERRCODE_DUPLICATE_OBJECT),errmsg("Native API conflicts with a foreign function: %s",format_procedure_qualified(procedure))));
            continue;
        }
        const char *start=sql.data+raw->stmt_location;
        char *definition=raw->stmt_len?pnstrdup(start,raw->stmt_len):pstrdup(start);
        if(SPI_execute(definition,false,0)<0)elog(ERROR,"Native API creation failed");
        pfree(definition);
        procedure=LookupFuncName(function->funcname,count,arguments,false);
        if(getExtensionOfObject(ProcedureRelationId,procedure)!=extension){
            char *membership=psprintf("ALTER EXTENSION laplace ADD FUNCTION %s",format_procedure_qualified(procedure));
            if(SPI_execute(membership,false,0)<0)elog(ERROR,"Native API membership restoration failed");
            pfree(membership);
        }
    }
    pfree(bound);pfree(literal);pfree(sql.data);
}

PG_FUNCTION_INFO_V1(laplace_function_bindings);
Datum laplace_function_bindings(PG_FUNCTION_ARGS)
{
    TupleDesc descriptor;
    Tuplestorestate *rows=set_begin(fcinfo,"laplace_function_bindings",&descriptor);
    bool nulls[2]={false,false};
    for(size_t i=0;i<lengthof(bindings);i++){
        Datum values[2]={CStringGetTextDatum(bindings[i].symbol),BoolGetDatum(bindings[i].sql_scope)};
        set_put(fcinfo,rows,descriptor,values,nulls);
    }
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(laplace_restore_function_bindings);
Datum laplace_restore_function_bindings(PG_FUNCTION_ARGS)
{
    HeapTuple self=SearchSysCache1(PROCOID,ObjectIdGetDatum(fcinfo->flinfo->fn_oid));
    if(!HeapTupleIsValid(self))ereport(ERROR,(errmsg("Laplace function binding: function disappeared")));
    bool missing;
    Datum module_value=SysCacheGetAttr(PROCOID,self,Anum_pg_proc_probin,&missing);
    if(missing)ereport(ERROR,(errmsg("Laplace function binding: module is not declared")));
    char *module=TextDatumGetCString(module_value);
    ReleaseSysCache(self);
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("Laplace function binding: SPI_connect")));
    Oid type=OIDOID;Datum extension=ObjectIdGetDatum(get_extension_oid("laplace",false));
    restore_declared_api(module,DatumGetObjectId(extension));
    if(SPI_execute_with_args(
        "SELECT pg_catalog.format('%I.%I(%s)',n.nspname,p.proname,pg_catalog.pg_get_function_identity_arguments(p.oid)),"
        "p.prosrc,l.lanname,p.probin,pg_catalog.pg_get_functiondef(p.oid),"
        "EXISTS(SELECT FROM pg_catalog.unnest(p.proconfig) s WHERE s LIKE 'search_path=%') "
        "FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace "
        "JOIN pg_catalog.pg_language l ON l.oid=p.prolang "
        "JOIN pg_catalog.pg_depend d ON d.classid='pg_catalog.pg_proc'::pg_catalog.regclass AND d.objid=p.oid "
        "WHERE d.refclassid='pg_catalog.pg_extension'::pg_catalog.regclass AND d.refobjid=$1 AND d.deptype='e' "
        "AND (l.lanname='c' OR (l.lanname='sql' AND NOT p.prosecdef AND p.proconfig IS NOT NULL))",1,&type,&extension,NULL,false,0)!=SPI_OK_SELECT)
        ereport(ERROR,(errmsg("Laplace function binding: catalog read failed")));
    StringInfoData ddl;initStringInfo(&ddl);
    bool present[lengthof(bindings)]={false};
    for(uint64 row=0;row<SPI_processed;row++){
        char *signature=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,1);
        char *symbol=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,2);
        char *language=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,3);
        bool known=strcmp(language,"sql")==0;
        for(size_t i=0;i<lengthof(bindings);i++)if(strcmp(language,"c")==0 && strcmp(bindings[i].symbol,symbol)==0){known=true;present[i]=true;break;}
        if(!known)ereport(ERROR,(errmsg("Native function has no binding contract: %s",signature)));
        if(strcmp(language,"c")==0){
            char *old_module=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,4);
            if(!old_module || strcmp(old_module,module)!=0){
                char *definition=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,5);
                char *body=strstr(definition,"\nAS ");
                if(!body)ereport(ERROR,(errmsg("Laplace function binding: missing C body for %s",signature)));
                /* Preserve the complete declaration, attributes, defaults and OID.
                 * Only the module/symbol body changes; dependent objects survive. */
                appendBinaryStringInfo(&ddl,definition,(int)(body-definition));
                appendStringInfo(&ddl,"\nAS %s, %s;\n",quote_literal_cstr(module),quote_literal_cstr(symbol));
            }
        }
        char *has_scope=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,6);
        if(strcmp(has_scope,"t")==0)appendStringInfo(&ddl,"ALTER FUNCTION %s RESET search_path;\n",signature);
    }
    for(size_t i=0;i<lengthof(bindings);i++)if(!present[i])
        ereport(ERROR,(errcode(ERRCODE_UNDEFINED_FUNCTION),errmsg("Native API is missing its declared function: %s",bindings[i].symbol)));
    SPI_freetuptable(SPI_tuptable);
    if(ddl.len && SPI_execute(ddl.data,false,0)<0)ereport(ERROR,(errmsg("Laplace function binding: metadata update failed")));
    pfree(ddl.data);SPI_finish();PG_RETURN_VOID();
}
