/* GIN over the 64-bit carrier mask and highway bank bit/slot bindings.
 * SQL bit position i corresponds to Native's uint64 bit i; the SQL binary
 * representation writes the most significant bit of each byte first. */
#include "pg_core.h"
#include "access/gin.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/varbit.h"

#define LP_STRAT_HASBIT  1        /* mask ? int2 */
#define LP_STRAT_HASALL  2        /* mask ?& int2[] */
#define LP_STRAT_HASANY  3        /* mask ?| int2[] */

static GinTernaryValue tri_all(const GinTernaryValue *check, int32 n){ GinTernaryValue r = GIN_TRUE; for (int i = 0; i < n; i++) { if (check[i] == GIN_FALSE) return GIN_FALSE; if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } return r; }
static GinTernaryValue tri_any(const GinTernaryValue *check, int32 n){ GinTernaryValue r = GIN_FALSE; for (int i = 0; i < n; i++) { if (check[i] == GIN_TRUE) return GIN_TRUE; if (check[i] == GIN_MAYBE) r = GIN_MAYBE; } return r; }
static bool bool_all(const bool *check, int32 n){ for (int i = 0; i < n; i++) if (!check[i]) return false; return true; }
static bool bool_any(const bool *check, int32 n){ for (int i = 0; i < n; i++) if (check[i]) return true; return false; }

static VarBit *carrier64(VarBit *mask){
    if (VARBITLEN(mask) != LP_MASK_BITS)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: a carrier mask must have exactly 64 bits")));
    return mask;
}
static uint8 reverse_byte(uint8 b){
    b = (uint8)(((b & 0x55u) << 1) | ((b >> 1) & 0x55u));
    b = (uint8)(((b & 0x33u) << 2) | ((b >> 2) & 0x33u));
    return (uint8)((b << 4) | (b >> 4));
}
static lp_mask mask_native(VarBit *mask){
    carrier64(mask); lp_mask value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64)reverse_byte(VARBITS(mask)[i]) << (8 * i);
    return value;
}
static VarBit *mask_sql(lp_mask value){
    VarBit *mask = palloc0(VARBITTOTALLEN(LP_MASK_BITS));
    SET_VARSIZE(mask, VARBITTOTALLEN(LP_MASK_BITS)); VARBITLEN(mask) = LP_MASK_BITS;
    for (unsigned i = 0; i < 8; i++) VARBITS(mask)[i] = reverse_byte((uint8)(value >> (8 * i)));
    return mask;
}
static const lp_bank *mask_bank(text *name){
    char *bank = text_to_cstring(name); const lp_bank *descriptor = lp_highway_bank(lp_pg_highway(), bank);
    if (!descriptor) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: no bank \"%s\" (manifest/banks.tsv)", bank)));
    return descriptor;
}
static int64 mask_slot(const lp_bank *bank, text *value){
    if (!VARSIZE_ANY_EXHDR(value)) return -1;
    lp_ref ref = trunk_of(value, NULL, 0, NULL);
    return lp_highway_slot(lp_pg_highway(), bank->list, &ref.id);
}

lp_mask lp_pg_mask_native(Datum value){return mask_native(DatumGetVarBitP(value));}
lp_mask lp_pg_kind_mask(const char *role){
    const lp_bank *kind=mask_bank(cstring_to_text("kind"));
    int64 slot=mask_slot(kind,cstring_to_text(role));lp_mask mask=0;
    if(slot>=0)lp_mask_put(&mask,kind,(uint32)slot);return mask;
}

/* Frozen names are execution bindings, not per-row text to decompose. Keep
 * one binding per expression and compare the actual arguments so prepared
 * executions and row-varying names cannot reuse another argument's slot. */
typedef struct {
    const lp_bank *bank;
    char *bank_name, *value_name;
    size_t bank_bytes, value_bytes;
    int64 slot;
    bool has_value;
} MaskBinding;
static MaskBinding *mask_binding(FunctionCallInfo fcinfo, int bank_arg, int value_arg){
    MaskBinding *binding=fcinfo->flinfo->fn_extra;
    if(!binding){
        binding=MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,sizeof *binding);
        fcinfo->flinfo->fn_extra=binding;
    }
    text *name=PG_GETARG_TEXT_PP(bank_arg); size_t bytes=VARSIZE_ANY_EXHDR(name);
    if(!binding->bank || binding->bank_bytes!=bytes || memcmp(binding->bank_name,VARDATA_ANY(name),bytes)){
        const lp_bank *bank=mask_bank(name);
        char *saved=MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,Max(bytes,(size_t)1));
        memcpy(saved,VARDATA_ANY(name),bytes);
        if(binding->bank_name)pfree(binding->bank_name);
        binding->bank_name=saved;binding->bank_bytes=bytes;
        binding->bank=bank;binding->has_value=false;
    }
    if(value_arg>=0){
        text *value=PG_GETARG_TEXT_PP(value_arg); bytes=VARSIZE_ANY_EXHDR(value);
        if(!binding->has_value || binding->value_bytes!=bytes || memcmp(binding->value_name,VARDATA_ANY(value),bytes)){
            int64 slot=mask_slot(binding->bank,value);
            char *saved=MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,Max(bytes,(size_t)1));
            memcpy(saved,VARDATA_ANY(value),bytes);
            if(binding->value_name)pfree(binding->value_name);
            binding->value_name=saved;binding->value_bytes=bytes;
            binding->slot=slot;binding->has_value=true;
        }
    }
    return binding;
}

PG_FUNCTION_INFO_V1(laplace_mask_slot);
Datum laplace_mask_slot(PG_FUNCTION_ARGS){
    MaskBinding *binding=mask_binding(fcinfo,0,1); lp_mask value = 0;
    if (binding->slot < 0 || !lp_mask_put(&value, binding->bank, (uint32)binding->slot)) PG_RETURN_NULL();
    PG_RETURN_VARBIT_P(mask_sql(value));
}
PG_FUNCTION_INFO_V1(laplace_mask_field);
Datum laplace_mask_field(PG_FUNCTION_ARGS){ PG_RETURN_VARBIT_P(mask_sql(lp_mask_field(mask_binding(fcinfo,0,-1)->bank))); }
PG_FUNCTION_INFO_V1(laplace_mask_value);
Datum laplace_mask_value(PG_FUNCTION_ARGS){
    PG_RETURN_INT16((int16)lp_mask_get(mask_native(PG_GETARG_VARBIT_P(0)), mask_binding(fcinfo,1,-1)->bank));
}
PG_FUNCTION_INFO_V1(laplace_mask_has_value);
Datum laplace_mask_has_value(PG_FUNCTION_ARGS){
    lp_mask value = mask_native(PG_GETARG_VARBIT_P(0));
    MaskBinding *binding=mask_binding(fcinfo,1,2);
    PG_RETURN_BOOL(binding->slot >= 0 && lp_mask_has(value, binding->bank, (uint32)binding->slot));
}
PG_FUNCTION_INFO_V1(laplace_mask_valid);
Datum laplace_mask_valid(PG_FUNCTION_ARGS){
    lp_mask value = mask_native(PG_GETARG_VARBIT_P(0)), merged;
    PG_RETURN_BOOL(lp_mask_merge(lp_pg_highway(), value, 0, &merged));
}
PG_FUNCTION_INFO_V1(laplace_mask_merge);
Datum laplace_mask_merge(PG_FUNCTION_ARGS){
    lp_mask a = mask_native(PG_GETARG_VARBIT_P(0)), b = mask_native(PG_GETARG_VARBIT_P(1)), merged;
    if (!lp_mask_merge(lp_pg_highway(), a, b, &merged))
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("laplace: conflicting enum values or reserved bits in carrier mask merge")));
    PG_RETURN_VARBIT_P(mask_sql(merged));
}
PG_FUNCTION_INFO_V1(laplace_mask_extract_value);
Datum laplace_mask_extract_value(PG_FUNCTION_ARGS){
    VarBit *m = carrier64(PG_GETARG_VARBIT_P(0)); int32 *n = (int32 *)PG_GETARG_POINTER(1);
    int len = VARBITLEN(m); const bits8 *b = VARBITS(m); Datum *k = palloc(sizeof(Datum) * (len ? len : 1)); int c = 0;
    for (int i = 0; i < len; i++) if (b[i >> 3] & (0x80 >> (i & 7))) k[c++] = Int16GetDatum((int16)i);
    *n = c; PG_RETURN_POINTER(k);
}
PG_FUNCTION_INFO_V1(laplace_mask_extract_query);
Datum laplace_mask_extract_query(PG_FUNCTION_ARGS){
    int32 *n = (int32 *)PG_GETARG_POINTER(1); StrategyNumber st = PG_GETARG_UINT16(2); int32 *mode = (int32 *)PG_GETARG_POINTER(6);
    if (st == LP_STRAT_HASBIT) { Datum *k = palloc(sizeof(Datum)); k[0] = Int16GetDatum(PG_GETARG_INT16(0)); *n = 1; PG_RETURN_POINTER(k); }
    ArrayType *a = PG_GETARG_ARRAYTYPE_P(0); Datum *v; bool *nulls; int c; deconstruct_array(a, INT2OID, 2, true, TYPALIGN_SHORT, &v, &nulls, &c);
    for (int i = 0; i < c; i++) if (nulls[i]) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace: mask bit arrays cannot contain nulls")));
    *n = c; if (c == 0) *mode = st == LP_STRAT_HASALL ? GIN_SEARCH_MODE_ALL : GIN_SEARCH_MODE_DEFAULT;
    PG_RETURN_POINTER(v);
}
PG_FUNCTION_INFO_V1(laplace_mask_consistent);
Datum laplace_mask_consistent(PG_FUNCTION_ARGS){
    bool *check = (bool *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    *(bool *)PG_GETARG_POINTER(5) = false;
    PG_RETURN_BOOL(st == LP_STRAT_HASANY ? bool_any(check, n) : bool_all(check, n));
}
PG_FUNCTION_INFO_V1(laplace_mask_triconsistent);
Datum laplace_mask_triconsistent(PG_FUNCTION_ARGS){
    const GinTernaryValue *check = (const GinTernaryValue *)PG_GETARG_POINTER(0); StrategyNumber st = PG_GETARG_UINT16(1); int32 n = PG_GETARG_INT32(3);
    PG_RETURN_GIN_TERNARY_VALUE(st == LP_STRAT_HASANY ? tri_any(check, n) : tri_all(check, n));
}
static bool mask_bit(VarBit *m, int16 i){ carrier64(m); return i >= 0 && i < LP_MASK_BITS && (VARBITS(m)[i >> 3] & (0x80 >> (i & 7))); }
PG_FUNCTION_INFO_V1(laplace_mask_has);
Datum laplace_mask_has(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(mask_bit(PG_GETARG_VARBIT_P(0), PG_GETARG_INT16(1))); }
static bool mask_has_array(VarBit *m, ArrayType *a, bool all){
    carrier64(m);
    Datum *v; bool *nulls; int c; deconstruct_array(a, INT2OID, 2, true, TYPALIGN_SHORT, &v, &nulls, &c);
    for (int i = 0; i < c; i++) if (nulls[i]) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("laplace: mask bit arrays cannot contain nulls")));
    for (int i = 0; i < c; i++) {
        bool has = mask_bit(m, DatumGetInt16(v[i])); if (all && !has) return false; if (!all && has) return true;
    }
    return all;
}
PG_FUNCTION_INFO_V1(laplace_mask_has_all);
Datum laplace_mask_has_all(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(mask_has_array(PG_GETARG_VARBIT_P(0), PG_GETARG_ARRAYTYPE_P(1), true)); }
PG_FUNCTION_INFO_V1(laplace_mask_has_any);
Datum laplace_mask_has_any(PG_FUNCTION_ARGS){ PG_RETURN_BOOL(mask_has_array(PG_GETARG_VARBIT_P(0), PG_GETARG_ARRAYTYPE_P(1), false)); }
/* Only flag banks have single bits. Enum callers use laplace_mask_slot and compare the entire field. */
PG_FUNCTION_INFO_V1(laplace_bank_bit);
Datum laplace_bank_bit(PG_FUNCTION_ARGS){
    const lp_bank *bank = mask_bank(PG_GETARG_TEXT_PP(0));
    if (!bank->flags) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("laplace: enum banks use laplace_mask_slot, not a single bit")));
    int64 slot = mask_slot(bank, PG_GETARG_TEXT_PP(1));
    PG_RETURN_INT16(slot >= 0 && slot < bank->width ? (int16)(bank->offset + slot) : -1);
}
/* The bank a type is a value of, and its bit there, by the type's ID; no row when it is a value of no bank. */
PG_FUNCTION_INFO_V1(laplace_bank_of);
Datum laplace_bank_of(PG_FUNCTION_ARGS){
    int32_t bit; const lp_bank *b = lp_highway_bank_of(lp_pg_highway(), (const lp_id *)PG_GETARG_POINTER(0), &bit);
    TupleDesc td; if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE) ereport(ERROR, (errmsg("laplace_bank_of: a record is returned")));
    if (!b) PG_RETURN_NULL();
    Datum v[4] = { CStringGetTextDatum(b->name), CStringGetTextDatum(b->group), CStringGetTextDatum(b->carrier), Int16GetDatum((int16)bit) }; bool nl[4] = { 0 };
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(td), v, nl)));
}
