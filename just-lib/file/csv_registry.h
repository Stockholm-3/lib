/**
 * @file csv_registry.h
 * @brief Generic async CSV registry: read → upsert/evict → write.
 *
 * Rows have the shape:
 *   key, tag, f1, f2, last_accessed_unix
 *
 * The registry keeps at most @p max_rows non-evicted rows.
 * When a new row would exceed the limit the oldest entry (by
 * last_accessed) is replaced — no TTL, eviction is solely count-based
 * so the caller controls the maximum size.
 *
 * This module has no knowledge of cities, energy plans, or any
 * domain-specific data.
 */
#ifndef CSV_REGISTRY_H
#define CSV_REGISTRY_H

#include <stdint.h>
#include <time.h>

typedef struct {
    char   key[256]; /* primary identifier, e.g. city name */
    char   tag[32];  /* secondary label,   e.g. price zone */
    double f1;       /* first numeric field, e.g. latitude  */
    double f2;       /* second numeric field, e.g. longitude */
    time_t last_accessed;
} CsvRow;

typedef enum {
    CSV_REG_ADDED,         /* new row inserted              */
    CSV_REG_EXISTS,        /* row refreshed (last_accessed) */
    CSV_REG_LIMIT_REACHED, /* max_rows hit and no old row evicted */
} CsvRegStatus;

typedef void (*CsvRegOnDone)(void* context, CsvRegStatus status);

/**
 * Async: upsert a row identified by (@p key, @p tag).
 *
 * If the row already exists its last_accessed is refreshed.
 * If the registry is full the least-recently-accessed row is evicted
 * to make room (count-based, no TTL).
 *
 * Non-blocking SMW task — safe to call from any context.
 * Fires @p on_done exactly once. On internal error the callback
 * receives CSV_REG_LIMIT_REACHED.
 *
 * @param csv_path   Path to the CSV file (created if absent).
 * @param max_rows   Maximum rows allowed in the file.
 * @return 0 on success (task created), -1 on error.
 */
int csv_registry_upsert(const char* csv_path, int max_rows, const char* key,
                        const char* tag, double f1, double f2, void* context,
                        CsvRegOnDone on_done);

/**
 * Sync (blocking): load all rows from @p csv_path.
 *
 * Caller must free(*out) when done.
 *
 * @param[out] out      Pointer to array of CsvRow (heap-allocated).
 * @param[out] out_count Number of rows loaded.
 * @return 0 on success, -1 on failure.
 */
int csv_registry_load(const char* csv_path, int max_rows, CsvRow** out,
                      int* out_count);

/* SMW work function — registered internally by csv_registry_upsert(). */
void csv_registry_task_work(void* context, uint64_t mon_time);

#endif // CSV_REGISTRY_H
