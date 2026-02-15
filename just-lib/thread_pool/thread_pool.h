/**
 * @file thread_pool.h
 * @brief Minimal thread pool for offloading work from the main event loop.
 *
 * Workers execute submitted tasks in background threads.  Completion
 * callbacks are collected in a thread-safe queue and dispatched on
 * the main thread via thread_pool_process_completions(), which is
 * intended to be called from an SMW task every event-loop cycle.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdint.h>

typedef struct ThreadPool ThreadPool;

/** Function executed in a worker thread. */
typedef void (*ThreadPoolWorkFunc)(void* arg);

/** Callback executed on the main thread after work completes. */
typedef void (*ThreadPoolDoneFunc)(void* arg);

/**
 * Create a thread pool with @p num_workers worker threads.
 *
 * @param num_workers  Number of worker threads (must be > 0).
 * @return Pointer to the pool, or NULL on failure.
 */
ThreadPool* thread_pool_create(int num_workers);

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
 * main thread (safe to call send_response, etc.).
 *
 * Either callback may be NULL:
 *  - work_fn == NULL → done_fn is queued immediately.
 *  - done_fn == NULL → no completion callback.
 *
 * @return 0 on success, -1 on failure.
 */
int thread_pool_submit(ThreadPool* pool, ThreadPoolWorkFunc work_fn,
                       void* work_arg, ThreadPoolDoneFunc done_fn,
                       void* done_arg);

/**
 * Drain the completion queue and invoke done callbacks.
 *
 * Must be called from the main thread (e.g. inside an SMW task).
 *
 * @return Number of callbacks dispatched.
 */
int thread_pool_process_completions(ThreadPool* pool);

/**
 * SMW-compatible callback that calls thread_pool_process_completions().
 *
 * Register with: smw_create_task(pool, thread_pool_smw_callback);
 *
 * @param context  Pointer to ThreadPool (passed by SMW).
 * @param mon_time Current monotonic time (unused).
 */
void thread_pool_smw_callback(void* context, uint64_t mon_time);

#endif /* THREAD_POOL_H */
