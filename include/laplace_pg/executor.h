#ifndef LAPLACE_PG_EXECUTOR_H
#define LAPLACE_PG_EXECUTOR_H

/* PostgreSQL adapter shared by the canonical extension and application
 * extension. The executor owns selection and order; Native consumes borrowed
 * slots. Retain values in the caller's context, outside the reset row arena. */
#include "postgres.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "tcop/dest.h"
#include "utils/memutils.h"

typedef void (*lp_pg_executor_visit)(TupleTableSlot *row, void *context);
typedef struct {
    DestReceiver receiver;
    lp_pg_executor_visit visit;
    void *context;
    MemoryContext memory;
    uint64 rows;
    bool selected;
} lp_pg_executor_receiver;

static bool lp_pg_executor_receive(TupleTableSlot *row, DestReceiver *destination)
{
    lp_pg_executor_receiver *receiver = (lp_pg_executor_receiver *)destination;
    CHECK_FOR_INTERRUPTS();
    MemoryContext before = MemoryContextSwitchTo(receiver->memory);
    receiver->visit(row, receiver->context);
    receiver->rows++;
    MemoryContextSwitchTo(before);
    MemoryContextReset(receiver->memory);
    return true;
}
static void lp_pg_executor_start(DestReceiver *destination, int operation, TupleDesc descriptor)
{
    (void)descriptor;
    if (operation != CMD_SELECT) elog(ERROR, "Native set receiver requires SELECT");
    ((lp_pg_executor_receiver *)destination)->selected = true;
}
static void lp_pg_executor_end(DestReceiver *destination) { (void)destination; }

/* read_only follows SPI's snapshot/command-counter contract. A reader after
 * writes in the same function must explicitly select writable SPI execution.
 * This function neither starts nor commits a transaction. */
static inline uint64 lp_pg_execute_stream(SPIPlanPtr plan, ParamListInfo parameters,
    bool read_only, lp_pg_executor_visit visit, void *context)
{
    lp_pg_executor_receiver receiver = {
        .receiver = {lp_pg_executor_receive, lp_pg_executor_start,
                     lp_pg_executor_end, lp_pg_executor_end, DestNone},
        .visit = visit, .context = context,
        .memory = AllocSetContextCreate(CurrentMemoryContext, "Native executor row", ALLOCSET_SMALL_SIZES)
    };
    SPIExecuteOptions options = {.params = parameters, .read_only = read_only,
        .must_return_tuples = true, .dest = &receiver.receiver};
    int result = SPI_execute_plan_extended(plan, &options);
    /* SPI labels SELECT with this non-SPI receiver as UTILITY. Verify the
     * executor command and independently count every delivered row. */
    if ((result != SPI_OK_SELECT && result != SPI_OK_UTILITY) || !receiver.selected ||
        SPI_processed != receiver.rows)
        elog(ERROR, "Native set executor failed: %s", SPI_result_code_string(result));
    MemoryContextDelete(receiver.memory);
    return receiver.rows;
}
#endif
