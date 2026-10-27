/* The Native module owns its SQL namespace boundaries. This registry is
 * installation metadata; primitive and index calls never execute it. */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "pg_core.h"
#include "commands/extension.h"
#include "lib/stringinfo.h"

typedef struct { const char *symbol; bool sql_scope; } FunctionBinding;
static const FunctionBinding bindings[]={
#include "laplace_function_bindings.h"
};

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
    if(SPI_connect()!=SPI_OK_CONNECT)ereport(ERROR,(errmsg("Laplace function binding: SPI_connect")));
    Oid type=OIDOID;Datum extension=ObjectIdGetDatum(get_extension_oid("laplace",false));
    if(SPI_execute_with_args(
        "SELECT pg_catalog.format('%I.%I(%s)',n.nspname,p.proname,pg_catalog.pg_get_function_identity_arguments(p.oid)),"
        "p.prosrc,l.lanname FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace "
        "JOIN pg_catalog.pg_language l ON l.oid=p.prolang "
        "JOIN pg_catalog.pg_depend d ON d.classid='pg_catalog.pg_proc'::pg_catalog.regclass AND d.objid=p.oid "
        "WHERE d.refclassid='pg_catalog.pg_extension'::pg_catalog.regclass AND d.refobjid=$1 AND d.deptype='e' "
        "AND l.lanname IN ('c','sql') AND NOT p.prosecdef AND p.proconfig IS NOT NULL",1,&type,&extension,NULL,true,0)!=SPI_OK_SELECT)
        ereport(ERROR,(errmsg("Laplace function binding: catalog read failed")));
    StringInfoData ddl;initStringInfo(&ddl);
    for(uint64 row=0;row<SPI_processed;row++){
        char *signature=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,1);
        char *symbol=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,2);
        char *language=SPI_getvalue(SPI_tuptable->vals[row],SPI_tuptable->tupdesc,3);
        bool known=strcmp(language,"sql")==0;
        for(size_t i=0;i<lengthof(bindings);i++)if(strcmp(bindings[i].symbol,symbol)==0){known=true;break;}
        if(!known)ereport(ERROR,(errmsg("Native function has no binding contract: %s",signature)));
        appendStringInfo(&ddl,"ALTER FUNCTION %s RESET search_path;\n",signature);
    }
    SPI_freetuptable(SPI_tuptable);
    if(ddl.len && SPI_execute(ddl.data,false,0)<0)ereport(ERROR,(errmsg("Laplace function binding: metadata update failed")));
    pfree(ddl.data);SPI_finish();PG_RETURN_VOID();
}
