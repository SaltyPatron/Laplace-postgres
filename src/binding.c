/* One namespace binding owner for every Native database operation. */
#include "pg_core.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "tcop/dest.h"

static bool registered, dirty = true;
static Oid native_schema;
static char *native_name, *postgis_name;
static MemoryContext names;
static uint64 generation;

static void invalidate_binding(Datum argument, int cache, uint32 hash)
{
    dirty = true;
}

static void bind_names(void)
{
    if (!registered) {
        CacheRegisterSyscacheCallback(EXTENSIONOID, invalidate_binding, (Datum)0);
        CacheRegisterSyscacheCallback(NAMESPACEOID, invalidate_binding, (Datum)0);
        CacheRegisterSyscacheCallback(PROCOID, invalidate_binding, (Datum)0);
        registered = true;
    }
    if (!dirty) return;
    Oid own = get_extension_schema(get_extension_oid("laplace", false));
    Oid gis = get_extension_schema(get_extension_oid("postgis", false));
    char *own_name = get_namespace_name(own), *gis_name = get_namespace_name(gis);
    if (!own_name || !gis_name)
        ereport(ERROR, (errmsg("Laplace extension namespace is missing")));
    if (!names) names = AllocSetContextCreate(TopMemoryContext, "Laplace installed namespaces", ALLOCSET_SMALL_SIZES);
    MemoryContextReset(names);
    MemoryContext previous = MemoryContextSwitchTo(names);
    native_schema = own;
    native_name = pstrdup(quote_identifier(own_name));
    postgis_name = pstrdup(quote_identifier(gis_name));
    MemoryContextSwitchTo(previous);
    pfree(own_name); pfree(gis_name);
    generation++;
    dirty = false;
}

Oid lp_pg_schema(void) { bind_names(); return native_schema; }
uint64 lp_pg_binding_generation(void) { bind_names(); return generation; }

Oid lp_pg_relation(const char *name)
{
    Oid relation = get_relname_relid(name, lp_pg_schema());
    if (!OidIsValid(relation))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("Laplace relation %s.%s is missing", native_name, name)));
    return relation;
}

char *lp_pg_sql(const char *sql)
{
    bind_names();
    StringInfoData out; initStringInfo(&out);
    while (*sql) {
        if (strncmp(sql, "@extschema@", 11) == 0) {
            appendStringInfoString(&out, native_name); sql += 11;
        } else if (strncmp(sql, "@postgis@", 9) == 0) {
            appendStringInfoString(&out, postgis_name); sql += 9;
        } else appendStringInfoChar(&out, *sql++);
    }
    return out.data;
}

int lp_pg_execute(const char *sql,bool read_only,long count)
{
    char *bound=lp_pg_sql(sql);
    int result=SPI_execute(bound,read_only,count);pfree(bound);return result;
}
int lp_pg_execute_args(const char *sql,int nargs,Oid *types,Datum *values,const char *nulls,bool read_only,long count)
{
    char *bound=lp_pg_sql(sql);
    int result=SPI_execute_with_args(bound,nargs,types,values,nulls,read_only,count);pfree(bound);return result;
}
SPIPlanPtr lp_pg_prepare_cursor(const char *sql,int nargs,Oid *types,int options)
{
    char *bound=lp_pg_sql(sql);
    SPIPlanPtr result=SPI_prepare_cursor(bound,nargs,types,options);pfree(bound);return result;
}

void lp_pg_stream(SPIPlanPtr plan,ParamListInfo parameters,lp_pg_row_visit visit,void *context)
{
    lp_pg_execute_stream(plan,parameters,true,visit,context);
}
