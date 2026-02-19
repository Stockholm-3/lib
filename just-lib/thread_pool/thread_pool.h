/**
 * @file thread_pool.h
 * @brief Thread pool for offloading work from the main event loop.
 *
 * Workers execute submitted tasks in background threads.  Completion
 * callbacks are collected in a thread-safe queue and dispatched on
 * the main thread via thread_pool_process_completions(), which is
 * intended to be called from an SMW task every event-loop cycle.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThreadPool     ThreadPool;
typedef struct ThreadPoolTask ThreadPoolTask;

/** Status codes passed to done callbacks. */
#define TP_STATUS_OK        0
#define TP_STATUS_CANCELLED -1
#define TP_STATUS_TIMEOUT   -2
#define TP_STATUS_ERROR     -3  /**< Unexpected exception thrown by work_fn. */

/**
 * Function executed in a worker thread.
 *
 * @param arg   User-provided work argument.
 * @param task  Task handle — use for cooperative cancel/timeout checks.
 * @return 0 on success, positive value for user-defined errors.
 */
typedef int (*ThreadPoolWorkFunc)(void* arg, ThreadPoolTask* task);

/**
 * Callback executed on the main thread after work completes.
 *
 * @param arg    User-provided done argument.
 * @param status Result: TP_STATUS_OK (0), >0 = work_fn error,
 *               TP_STATUS_CANCELLED, TP_STATUS_TIMEOUT, or TP_STATUS_ERROR.
 */
typedef void (*ThreadPoolDoneFunc)(void* arg, int status);

/** Runtime statistics snapshot. */
typedef struct {
    int num_workers;     /**< Total worker threads. */
    int active_workers;  /**< Workers currently executing a task. */
    int pending_tasks;   /**< Tasks waiting in the work queue. */
    int completed_tasks; /**< Total tasks processed since creation (executed + cancelled + timeout). */
} ThreadPoolStats;

/**
 * Create a thread pool.
 *
 * @param num_workers  Number of worker threads (must be > 0).
 * @param max_pending  Maximum queued tasks (0 = unlimited).
 * @return Pointer to the pool, or NULL on failure.
 */
ThreadPool* thread_pool_create(int num_workers, int max_pending);

/**
 * Shut down the pool and join all worker threads.
 *
 * Blocks until every in-flight task finishes.  Pending completion
 * callbacks are discarded — call thread_pool_process_completions()
 * before destroy if you need them.
 *
 * @param pool  Pool to destroy (NULL is a safe no-op).
 */
void thread_pool_destroy(ThreadPool* pool);

/**
 * Submit a task to the pool.
 *
 * @p work_fn runs in a worker thread.  When it returns, the task is
 * moved to the completion queue.  On the next call to
 * thread_pool_process_completions(), @p done_fn is invoked on the
 * main thread with the result status.
 *
 * Either callback may be NULL:
 *  - work_fn == NULL → done_fn is queued immediately (status = 0).
 *  - done_fn == NULL → no completion callback.
 *
 * @param pool       Thread pool.
 * @param work_fn    Work function (runs in worker thread).
 * @param work_arg   Argument for work_fn.
 * @param done_fn    Completion callback (runs on main thread).
 * @param done_arg   Argument for done_fn.
 * @param timeout_ms Timeout in milliseconds (0 = no timeout).
 * @return Task handle, or NULL on failure (shutdown, queue full, alloc).
 */
ThreadPoolTask* thread_pool_submit(ThreadPool* pool, ThreadPoolWorkFunc work_fn,
                                   void* work_arg, ThreadPoolDoneFunc done_fn,
                                   void* done_arg, int timeout_ms);

/**
 * Cancel a task.
 *
 * If the task is still queued, it will be skipped with TP_STATUS_CANCELLED.
 * If the task is currently running, work_fn should check cancellation
 * cooperatively via thread_pool_task_is_cancelled().
 *
 * @param task  Task handle returned by thread_pool_submit().
 */
void thread_pool_cancel(ThreadPoolTask* task);

/**
 * Check if a task has been cancelled (cooperative).
 *
 * Intended to be called from within work_fn to allow early exit.
 *
 * @return Non-zero if cancelled.
 */
int thread_pool_task_is_cancelled(ThreadPoolTask* task);

/**
 * Get remaining time before timeout (cooperative).
 *
 * Intended to be called from within work_fn.
 *
 * @return Milliseconds remaining, 0 if no timeout set, -1 if expired.
 */
int thread_pool_task_remaining_ms(ThreadPoolTask* task);

/**
 * Drain the completion queue and invoke done callbacks.
 *
 * Must be called from the main thread (e.g. inside an SMW task).
 *
 * @return Number of callbacks dispatched.
 */
int thread_pool_process_completions(ThreadPool* pool);

/**
 * Get a snapshot of pool statistics.
 *
 * Thread-safe — may be called from any thread.
 */
void thread_pool_get_stats(ThreadPool* pool, ThreadPoolStats* stats);

/**
 * Block until all submitted tasks have completed.
 *
 * Does not prevent new submissions — only waits for the current
 * work queue to drain and active workers to finish.
 */
void thread_pool_wait_idle(ThreadPool* pool);

/**
 * SMW-compatible callback that calls thread_pool_process_completions().
 *
 * Register with: smw_create_task(pool, thread_pool_smw_callback);
 *
 * @param context  Pointer to ThreadPool (passed by SMW).
 * @param mon_time Current monotonic time (unused).
 */
void thread_pool_smw_callback(void* context, uint64_t mon_time);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_POOL_H */
