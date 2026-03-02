/**
 * @file thread_pool.cpp
 * @brief Thread pool implementation using C++11 standard library.
 *
 * Internals use std::thread, std::mutex, std::condition_variable,
 * std::atomic, and std::queue.  The public API is unchanged and
 * remains C-compatible (extern "C" declared in thread_pool.h).
 */

#include "thread_pool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <pthread.h> /* pthread_setname_np — Linux extension */
#include <queue>
#include <thread>
#include <time.h> /* clock_gettime, CLOCK_MONOTONIC */
#include <vector>

/* ============= Internal Helpers ============= */

static uint64_t now_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}

/* ============= Internal Types ============= */

struct ThreadPoolTask {
    ThreadPoolWorkFunc work_fn;
    void*              work_arg;
    ThreadPoolDoneFunc done_fn;
    void*              done_arg;
    std::atomic<int>   cancelled;
    uint64_t           deadline_ms;
    int                status;
    int                priority;

    ThreadPoolTask()
        : work_fn(NULL), work_arg(NULL), done_fn(NULL), done_arg(NULL),
          cancelled(0), deadline_ms(0), status(0), priority(0) {}
};

struct WorkQueue {
    std::queue<ThreadPoolTask*> high; /* priority >= 0 */
    std::queue<ThreadPoolTask*> low;  /* priority <  0 */
    std::mutex                  mutex;
    std::condition_variable     cond;
    std::condition_variable     idle_cond; /* shares mutex with cond */
};

struct CompletionQueue {
    std::queue<ThreadPoolTask*> tasks;
    std::mutex                  mutex;
};

struct ThreadPool {
    std::vector<std::thread> threads;
    int                      num_workers;
    int                      max_pending;
    std::atomic<bool>        shutdown;

    WorkQueue       work;
    CompletionQueue done;

    /* Statistics — atomics, no separate mutex required */
    std::atomic<int> active_workers;
    std::atomic<int> completed_total;

    ThreadPool()
        : num_workers(0), max_pending(0), shutdown(false), active_workers(0),
          completed_total(0) {}
};

/* ============= Worker Thread ============= */

static void worker_thread(ThreadPool* pool) {
    for (;;) {
        ThreadPoolTask* task = NULL;

        {
            std::unique_lock<std::mutex> lock(pool->work.mutex);
            pool->work.cond.wait(lock, [pool] {
                return !pool->work.high.empty() || !pool->work.low.empty()
                       || pool->shutdown.load();
            });

            if (pool->shutdown.load() && pool->work.high.empty() &&
                pool->work.low.empty()) {
                return;
            }

            if (!pool->work.high.empty()) {
                task = pool->work.high.front();
                pool->work.high.pop();
            } else {
                task = pool->work.low.front();
                pool->work.low.pop();
            }
            /* Increment under lock so wait_idle cannot observe
             * active_workers==0 in the window between dequeue and execution
             * start. */
            pool->active_workers++;
        }

        /* Check cancellation before executing */
        if (task->cancelled.load()) {
            task->status = TP_STATUS_CANCELLED;

            /* Check timeout before executing */
        } else if (task->deadline_ms > 0 &&
                   now_monotonic_ms() > task->deadline_ms) {
            task->status = TP_STATUS_TIMEOUT;

        } else {
            /* Execute work in this worker thread.
             * try-catch prevents an unexpected C++ exception from propagating
             * out of worker_thread, which would trigger std::terminate. */
            try {
                task->status = task->work_fn
                                   ? task->work_fn(task->work_arg, task)
                                   : TP_STATUS_OK;

                /* Check timeout after executing */
                if (task->deadline_ms > 0 &&
                    now_monotonic_ms() > task->deadline_ms) {
                    task->status = TP_STATUS_TIMEOUT;

                    /* Check cancellation after executing */
                } else if (task->cancelled.load() &&
                           task->status == TP_STATUS_OK) {
                    task->status = TP_STATUS_CANCELLED;
                }
            } catch (...) {
                task->status = TP_STATUS_ERROR;
            }
        }

        /* Count every processed task: executed, cancelled, timeout. */
        pool->completed_total++;

        /* Push to done queue BEFORE decrementing active_workers.
         * wait_idle evaluates its predicate under work.mutex.  By decrementing
         * only after the push (and inside work.mutex), we guarantee that any
         * wakeup of wait_idle — including spurious ones — can only observe
         * active==0 after the task is already in the done queue.
         * done.mutex is always released before work.mutex is acquired → no
         * cycle. */
        if (task->done_fn) {
            std::lock_guard<std::mutex> dlock(pool->done.mutex);
            pool->done.tasks.push(task);
        } else {
            delete task;
        }

        {
            std::lock_guard<std::mutex> wlock(pool->work.mutex);
            pool->active_workers.fetch_sub(1);
            if (pool->active_workers.load() == 0 && pool->work.high.empty() &&
                pool->work.low.empty()) {
                pool->work.idle_cond.notify_all();
            }
        }
    }
}

/* ============= Public API ============= */

ThreadPool* thread_pool_create(int num_workers, int max_pending) {
    if (num_workers <= 0) {
        return NULL;
    }

    ThreadPool* pool = new (std::nothrow) ThreadPool();
    if (!pool) {
        return NULL;
    }

    pool->num_workers = num_workers;
    pool->max_pending = max_pending;
    pool->threads.reserve(num_workers);

    for (int i = 0; i < num_workers; i++) {
        try {
            pool->threads.emplace_back(worker_thread, pool);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(pool->work.mutex);
                pool->shutdown = true;
            }
            pool->work.cond.notify_all();
            for (std::vector<std::thread>::iterator it = pool->threads.begin();
                 it != pool->threads.end(); ++it) {
                if (it->joinable()) {
                    it->join();
                }
            }
            delete pool;
            return NULL;
        }

        /* Name the thread for debugging (best-effort, Linux only) */
        char name[16];
        snprintf(name, sizeof(name), "tp-worker-%d", i);
        pthread_setname_np(pool->threads.back().native_handle(), name);
    }

    return pool;
}

void thread_pool_destroy(ThreadPool* pool) {
    if (!pool) {
        return;
    }

    /* Signal shutdown */
    {
        std::lock_guard<std::mutex> lock(pool->work.mutex);
        pool->shutdown = true;
    }
    pool->work.cond.notify_all();

    /* Wait for all workers */
    for (std::vector<std::thread>::iterator it = pool->threads.begin();
         it != pool->threads.end(); ++it) {
        if (it->joinable()) {
            it->join();
        }
    }

    /* Free remaining work queue tasks */
    while (!pool->work.high.empty()) {
        delete pool->work.high.front();
        pool->work.high.pop();
    }
    while (!pool->work.low.empty()) {
        delete pool->work.low.front();
        pool->work.low.pop();
    }

    /* Free remaining completion queue tasks */
    while (!pool->done.tasks.empty()) {
        delete pool->done.tasks.front();
        pool->done.tasks.pop();
    }

    delete pool;
}

ThreadPoolTask* thread_pool_submit(ThreadPool* pool, ThreadPoolWorkFunc work_fn,
                                   void* work_arg, ThreadPoolDoneFunc done_fn,
                                   void* done_arg, int timeout_ms,
                                   int priority) {
    if (!pool) {
        return NULL;
    }

    ThreadPoolTask* task = new (std::nothrow) ThreadPoolTask();
    if (!task) {
        return NULL;
    }

    task->work_fn  = work_fn;
    task->work_arg = work_arg;
    task->done_fn  = done_fn;
    task->done_arg = done_arg;

    if (timeout_ms > 0) {
        task->deadline_ms =
            now_monotonic_ms() + static_cast<uint64_t>(timeout_ms);
    }

    std::lock_guard<std::mutex> lock(pool->work.mutex);

    /* Reject if shut down (checked under lock to avoid race with destroy) */
    if (pool->shutdown.load()) {
        delete task;
        return NULL;
    }

    /* Backpressure: reject if queue is full */
    if (pool->max_pending > 0 &&
        static_cast<int>(pool->work.high.size() + pool->work.low.size()) >=
            pool->max_pending) {
        delete task;
        return NULL;
    }

    task->priority = priority;
    if (priority >= 0) {
        pool->work.high.push(task);
    } else {
        pool->work.low.push(task);
    }
    pool->work.cond.notify_one();
    return task;
}

void thread_pool_cancel(ThreadPoolTask* task) {
    if (task) {
        task->cancelled.store(1);
    }
}

int thread_pool_task_is_cancelled(ThreadPoolTask* task) {
    return task ? task->cancelled.load() : 0;
}

int thread_pool_task_remaining_ms(ThreadPoolTask* task) {
    if (!task || task->deadline_ms == 0) {
        return 0; /* no timeout */
    }
    uint64_t now = now_monotonic_ms();
    if (now >= task->deadline_ms) {
        return -1; /* expired */
    }
    return static_cast<int>(task->deadline_ms - now);
}

int thread_pool_process_completions(ThreadPool* pool) {
    if (!pool) {
        return 0;
    }

    /* Swap out all completed tasks at once (minimises lock time) */
    std::queue<ThreadPoolTask*> local;
    {
        std::lock_guard<std::mutex> lock(pool->done.mutex);
        std::swap(local, pool->done.tasks);
    }

    /* Dispatch callbacks on the main thread */
    int count = 0;
    while (!local.empty()) {
        ThreadPoolTask* task = local.front();
        local.pop();
        if (task->done_fn) {
            task->done_fn(task->done_arg, task->status);
        }
        delete task;
        count++;
    }

    return count;
}

void thread_pool_get_stats(ThreadPool* pool, ThreadPoolStats* stats) {
    if (!pool || !stats) {
        return;
    }

    stats->num_workers = pool->num_workers;
    /* completed_total is monotonically increasing — consistent without lock */
    stats->completed_tasks = pool->completed_total.load();

    /* Read active_workers and pending_tasks under the same lock so the
     * snapshot is consistent: active_workers++ happens under work.mutex
     * during dequeue, preventing active=0,pending=0 while a task is running */
    std::lock_guard<std::mutex> lock(pool->work.mutex);
    stats->active_workers = pool->active_workers.load();
    stats->pending_tasks  = static_cast<int>(pool->work.high.size() +
                                              pool->work.low.size());
}

void thread_pool_wait_idle(ThreadPool* pool) {
    if (!pool) {
        return;
    }

    /* idle_cond shares work.mutex — no nested lock required */
    std::unique_lock<std::mutex> lock(pool->work.mutex);
    pool->work.idle_cond.wait(lock, [pool] {
        return pool->active_workers.load() == 0 && pool->work.high.empty() &&
               pool->work.low.empty();
    });
}

void thread_pool_smw_callback(void* context, uint64_t mon_time) {
    (void)mon_time;
    thread_pool_process_completions(static_cast<ThreadPool*>(context));
}
