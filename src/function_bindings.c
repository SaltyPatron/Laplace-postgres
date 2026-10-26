/* The Native module owns its SQL namespace boundaries. This registry is
 * installation metadata; primitive and index calls never execute it. */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "pg_core.h"

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
