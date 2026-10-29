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
/* Returning false consumes the current row and ends this selection. This is
 * useful for exact-match readers; it does not impose an arbitrary fetch cap. */
typedef bool (*lp_pg_executor_visit_until)(TupleTableSlot *row, void *context);
typedef struct {
    DestReceiver receiver;
    lp_pg_executor_visit visit;
    lp_pg_executor_visit_until visit_until;
    void *context;
    MemoryContext memory;
    uint64 rows;
    bool selected;
    bool stopped;
} lp_pg_executor_receiver;

static bool lp_pg_executor_receive(TupleTableSlot *row, DestReceiver *destination)
{
    lp_pg_executor_receiver *receiver = (lp_pg_executor_receiver *)destination;
    CHECK_FOR_INTERRUPTS();
    MemoryContext before = MemoryContextSwitchTo(receiver->memory);
    if (receiver->visit_until)
        receiver->stopped = !receiver->visit_until(row, receiver->context);
    else
        receiver->visit(row, receiver->context);
    receiver->rows++;
    MemoryContextSwitchTo(before);
    MemoryContextReset(receiver->memory);
    return !receiver->stopped;
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
static inline uint64 lp_pg_execute_stream_callbacks(SPIPlanPtr plan, ParamListInfo parameters,
    bool read_only, lp_pg_executor_visit visit, lp_pg_executor_visit_until visit_until,
    void *context)
{
    if (!plan || (!visit && !visit_until))
        elog(ERROR, "Native set executor requires a plan and row consumer");
    lp_pg_executor_receiver receiver = {
        .receiver = {lp_pg_executor_receive, lp_pg_executor_start,
                     lp_pg_executor_end, lp_pg_executor_end, DestNone},
        .visit = visit, .visit_until = visit_until, .context = context,
        .memory = AllocSetContextCreate(CurrentMemoryContext, "Native executor row", ALLOCSET_SMALL_SIZES)
    };
    SPIExecuteOptions options = {.params = parameters, .read_only = read_only,
        .must_return_tuples = true, .dest = &receiver.receiver};
    int result = SPI_execute_plan_extended(plan, &options);
    /* SPI labels SELECT with this non-SPI receiver as UTILITY. Verify the
     * executor command and independently count every delivered row. */
    if ((result != SPI_OK_SELECT && result != SPI_OK_UTILITY) || !receiver.selected ||
        (!receiver.stopped && SPI_processed != receiver.rows))
        elog(ERROR, "Native set executor failed: %s", SPI_result_code_string(result));
    MemoryContextDelete(receiver.memory);
    return receiver.rows;
}

static inline uint64 lp_pg_execute_stream(SPIPlanPtr plan, ParamListInfo parameters,
    bool read_only, lp_pg_executor_visit visit, void *context)
{
    return lp_pg_execute_stream_callbacks(plan, parameters, read_only, visit, NULL, context);
}

static inline uint64 lp_pg_execute_stream_until(SPIPlanPtr plan, ParamListInfo parameters,
    bool read_only, lp_pg_executor_visit_until visit, void *context)
{
    return lp_pg_execute_stream_callbacks(plan, parameters, read_only, NULL, visit, context);
}
#endif
