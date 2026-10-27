/* Ordered structure is read from the canonical DAG under registered witnesses.
 * Native owns run arithmetic; the adapter owns source scope and SQL row shape. */
#include "pg_core.h"
#include "catalog/pg_type.h"

PG_FUNCTION_INFO_V1(laplace_separator_ids);
Datum laplace_separator_ids(PG_FUNCTION_ARGS)
{
    ArrayType *cached = fcinfo->flinfo->fn_extra;
    if (!cached) {
        const lp_layout *layout = lp_pg_flags();
        const lp_field *category = lp_flags_field(layout, "gc");
        const char *names[] = {"Zs", "Zl", "Zp", "Cc"}; int32 values[4];
        if (!category) ereport(ERROR, (errmsg("the Unicode floor has no General_Category field")));
        for (size_t i = 0; i < 4; i++) {
            values[i] = lp_flags_value(layout, category, names[i]);
            if (values[i] < 0) ereport(ERROR, (errmsg("the Unicode floor has no %s category", names[i])));
        }
        const lp_tier0_record *floor = lp_pg_tier0();
        lp_vec(lp_id) ids = {0};
        for (uint32 cp = 0; cp < LP_NCP; cp++) {
            uint32 value = lp_flags_get(layout, cp, category);
            if ((cp & 4095u) == 0) CHECK_FOR_INTERRUPTS();
            if (value == (uint32)values[0] || value == (uint32)values[1] ||
                value == (uint32)values[2] || value == (uint32)values[3]) {
                const lp_tier0_record *record = lp_tier0_at(floor, cp);
                if (!record) ereport(ERROR, (errmsg("separator category contains a non-scalar codepoint")));
                lp_push(&ids, record->id);
            }
        }
        /* UAX #29 GB3: CRLF is the composed separator grapheme. */
        lp_id children[] = {lp_tier0_at(floor, 13)->id, lp_tier0_at(floor, 10)->id}, crlf;
        lp_id_compose(children, 2, &crlf); lp_push(&ids, crlf);
        MemoryContext previous = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
        Oid element = get_element_type(get_fn_expr_rettype(fcinfo->flinfo));
        cached = id_array_of(element, ids.v, (int)ids.n);
        fcinfo->flinfo->fn_extra = cached;
        MemoryContextSwitchTo(previous); lp_vec_free(&ids);
    }
    PG_RETURN_ARRAYTYPE_P(DatumGetArrayTypePCopy(PointerGetDatum(cached)));
}

typedef struct {
    lp_idmap *vocab, *separators;
    uint64 lo, hi;
    lp_id container;
    Tuplestorestate *rows;
    TupleDesc descriptor;
} OrderedPairs;
static lp_idmap *sequence_ids(ArrayType *array)
{
    if (ARR_NDIM(array) > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("ordered structure requires one-dimensional identity arrays")));
    int count; lp_id *ids = ids_of(array, &count); lp_idmap *set = lp_idmap_new();
    for (int i = 0; i < count; i++) lp_idmap_put(set, &ids[i], NULL);
    pfree(ids); return set;
}
static bool ordered_emit(void *context, const lp_id *subject, const lp_id *object, uint64 gap, uint64 count)
{
    OrderedPairs *pairs = context;
    CHECK_FOR_INTERRUPTS();
    if (pairs->vocab && lp_idmap_find(pairs->vocab, subject) < 0) return true;
    if (count > PG_INT64_MAX)
        ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("ordered occurrence count exceeds int8")));
    /* The tuplestore copies fixed-width values. Borrow the immutable run IDs
     * instead of allocating three tiny objects for every emitted pair. */
    Datum values[] = {PointerGetDatum(&pairs->container), Int32GetDatum((int32)gap),
        PointerGetDatum(subject), PointerGetDatum(object), Int64GetDatum((int64)count)};
    bool nulls[5] = {0};
    tuplestore_putvalues(pairs->rows, pairs->descriptor, values, nulls);
    return true;
}
static void ordered_path(const lp_id *id, lp_path path, Datum mask, void *context)
{
    OrderedPairs *pairs = context; (void)mask;
    if (path.n > MaxAllocSize / sizeof(lp_id_run))
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("ordered path exceeds allocation capacity")));
    lp_id_run *runs = palloc(Max(path.n, 1) * sizeof *runs); size_t n = 0;
    for (size_t i = 0; i < path.n; i++) {
        lp_id child = lp_path_id(path, i);
        if (lp_idmap_find(pairs->separators, &child) >= 0) continue;
        runs[n++] = (lp_id_run){child, lp_path_run(path, i)};
    }
    pairs->container = *id;
    if (!lp_id_run_pairs(runs, n, pairs->lo, pairs->hi, ordered_emit, pairs))
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid ordered run sequence")));
    pfree(runs);
}
PG_FUNCTION_INFO_V1(laplace_ordered_pair_counts);
Datum laplace_ordered_pair_counts(PG_FUNCTION_ARGS)
{
    OrderedPairs pairs = {0};
    pairs.rows = set_begin(fcinfo, "laplace_ordered_pair_counts", &pairs.descriptor);
    if (PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_GETARG_INT32(1) < 1 || PG_GETARG_INT32(2) < PG_GETARG_INT32(1))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("ordered structure requires positive ordered offset bounds")));
    pairs.lo = (uint64)PG_GETARG_INT32(1); pairs.hi = (uint64)PG_GETARG_INT32(2);
    pairs.vocab = PG_ARGISNULL(0) ? NULL : sequence_ids(PG_GETARG_ARRAYTYPE_P(0));
    pairs.separators = PG_ARGISNULL(4) ? lp_idmap_new() : sequence_ids(PG_GETARG_ARRAYTYPE_P(4));
    ArrayType *roots = PG_ARGISNULL(3) ? NULL : PG_GETARG_ARRAYTYPE_P(3);
    if ((!pairs.vocab || lp_idmap_count(pairs.vocab)) &&
        (!roots || ArrayGetNItems(ARR_NDIM(roots), ARR_DIMS(roots))))
        lp_pg_walk_witnesses(roots, ordered_path, &pairs);
    lp_idmap_free(pairs.vocab); lp_idmap_free(pairs.separators);
    PG_RETURN_NULL();
}
