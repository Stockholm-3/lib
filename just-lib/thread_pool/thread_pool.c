/**
 * @file thread_pool.c
 * @brief Thread pool implementation using POSIX threads.
 */

#include "thread_pool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============= Internal Helpers ============= */

static uint64_t now_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ============= Internal Types ============= */

struct ThreadPoolTask {
    ThreadPoolWorkFunc     work_fn;
    void*                  work_arg;
    ThreadPoolDoneFunc     done_fn;
    void*                  done_arg;
    struct ThreadPoolTask* next;
    volatile int           cancelled;
    uint64_t               deadline_ms; /* 0 = no timeout */
    int                    status;
};

typedef struct {
    ThreadPoolTask* head;
    ThreadPoolTask* tail;
    int             count;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} WorkQueue;

typedef struct {
    ThreadPoolTask* head;
    ThreadPoolTask* tail;
    pthread_mutex_t mutex;
} CompletionQueue;

struct ThreadPool {
    pthread_t*   threads;
    int          num_workers;
    int          max_pending;
    volatile int shutdown;

    WorkQueue       work;
    CompletionQueue done;

    /* Statistics */
    volatile int    active_workers;
    volatile int    completed_total;
    pthread_mutex_t stats_mutex;
    pthread_cond_t  idle_cond;
};

/* ============= Queue Helpers ============= */

static void queue_push(ThreadPoolTask** head, ThreadPoolTask** tail,
                       ThreadPoolTask* task) {
    task->next = NULL;
    if (*tail) {
        (*tail)->next = task;
    } else {
        *head = task;
    }
    *tail = task;
}

static ThreadPoolTask* queue_pop(ThreadPoolTask** head, ThreadPoolTask** tail) {
    ThreadPoolTask* task = *head;
    if (!task) {
        return NULL;
    }
    *head = task->next;
    if (!*head) {
        *tail = NULL;
    }
    task->next = NULL;
    return task;
}

static ThreadPoolTask* queue_take_all(ThreadPoolTask** head,
                                      ThreadPoolTask** tail) {
    ThreadPoolTask* list = *head;
    *head                = NULL;
    *tail                = NULL;
    return list;
}

/* ============= Worker Thread ============= */

static void* worker_thread(void* arg) {
    ThreadPool* pool = (ThreadPool*)arg;

    for (;;) {
        pthread_mutex_lock(&pool->work.mutex);

        while (!pool->work.head && !pool->shutdown) {
            pthread_cond_wait(&pool->work.cond, &pool->work.mutex);
        }

        if (pool->shutdown && !pool->work.head) {
            pthread_mutex_unlock(&pool->work.mutex);
            break;
        }

        ThreadPoolTask* task = queue_pop(&pool->work.head, &pool->work.tail);
        if (task) {
            pool->work.count--;
        }
        pthread_mutex_unlock(&pool->work.mutex);

        if (!task) {
            continue;
        }

        /* Check cancellation before executing */
        if (task->cancelled) {
            task->status = TP_STATUS_CANCELLED;
            goto completion;
        }

        /* Check timeout before executing */
        if (task->deadline_ms > 0 && now_monotonic_ms() > task->deadline_ms) {
            task->status = TP_STATUS_TIMEOUT;
            goto completion;
        }

        /* Track active workers */
        pthread_mutex_lock(&pool->stats_mutex);
        pool->active_workers++;
        pthread_mutex_unlock(&pool->stats_mutex);

        /* Execute work in this worker thread */
        if (task->work_fn) {
            task->status = task->work_fn(task->work_arg, task);
        } else {
            task->status = TP_STATUS_OK;
        }

        /* Check timeout after executing */
        if (task->deadline_ms > 0 && now_monotonic_ms() > task->deadline_ms) {
            task->status = TP_STATUS_TIMEOUT;
        }

        /* Check cancellation after executing */
        if (task->cancelled && task->status == TP_STATUS_OK) {
            task->status = TP_STATUS_CANCELLED;
        }

        /* Update stats */
        pthread_mutex_lock(&pool->stats_mutex);
        pool->active_workers--;
        pool->completed_total++;
        int idle = (pool->active_workers == 0);
        pthread_mutex_unlock(&pool->stats_mutex);

        /* Signal idle if work queue is empty and no active workers */
        if (idle) {
            pthread_mutex_lock(&pool->work.mutex);
            int empty = (pool->work.count == 0);
            pthread_mutex_unlock(&pool->work.mutex);
            if (empty) {
                pthread_cond_broadcast(&pool->idle_cond);
            }
        }

    completion:
        /* Move to completion queue if there is a done callback */
        if (task->done_fn) {
            pthread_mutex_lock(&pool->done.mutex);
            queue_push(&pool->done.head, &pool->done.tail, task);
            pthread_mutex_unlock(&pool->done.mutex);
        } else {
            free(task);
        }
    }

    return NULL;
}

/* ============= Public API ============= */

ThreadPool* thread_pool_create(int num_workers, int max_pending) {
    if (num_workers <= 0) {
        return NULL;
    }

    ThreadPool* pool = calloc(1, sizeof(ThreadPool));
    if (!pool) {
        return NULL;
    }

    pool->num_workers = num_workers;
    pool->max_pending = max_pending;
    pool->shutdown    = 0;

    if (pthread_mutex_init(&pool->work.mutex, NULL) != 0) {
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->work.cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->work.mutex);
        free(pool);
        return NULL;
    }
    if (pthread_mutex_init(&pool->done.mutex, NULL) != 0) {
        pthread_cond_destroy(&pool->work.cond);
        pthread_mutex_destroy(&pool->work.mutex);
        free(pool);
        return NULL;
    }
    if (pthread_mutex_init(&pool->stats_mutex, NULL) != 0) {
        pthread_mutex_destroy(&pool->done.mutex);
        pthread_cond_destroy(&pool->work.cond);
        pthread_mutex_destroy(&pool->work.mutex);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->idle_cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->stats_mutex);
        pthread_mutex_destroy(&pool->done.mutex);
        pthread_cond_destroy(&pool->work.cond);
        pthread_mutex_destroy(&pool->work.mutex);
        free(pool);
        return NULL;
    }

    pool->threads = calloc(num_workers, sizeof(pthread_t));
    if (!pool->threads) {
        pthread_cond_destroy(&pool->idle_cond);
        pthread_mutex_destroy(&pool->stats_mutex);
        pthread_mutex_destroy(&pool->done.mutex);
        pthread_cond_destroy(&pool->work.cond);
        pthread_mutex_destroy(&pool->work.mutex);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work.cond);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_cond_destroy(&pool->idle_cond);
            pthread_mutex_destroy(&pool->stats_mutex);
            pthread_mutex_destroy(&pool->done.mutex);
            pthread_cond_destroy(&pool->work.cond);
            pthread_mutex_destroy(&pool->work.mutex);
            free(pool->threads);
            free(pool);
            return NULL;
        }

        /* Name the thread for debugging (best-effort) */
        char name[16];
        snprintf(name, sizeof(name), "tp-worker-%d", i);
        pthread_setname_np(pool->threads[i], name);
    }

    return pool;
}

void thread_pool_destroy(ThreadPool* pool) {
    if (!pool) {
        return;
    }

    /* Signal shutdown */
    pthread_mutex_lock(&pool->work.mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work.cond);
    pthread_mutex_unlock(&pool->work.mutex);

    /* Wait for all workers */
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* Free remaining work queue tasks */
    ThreadPoolTask* task = pool->work.head;
    while (task) {
        ThreadPoolTask* next = task->next;
        free(task);
        task = next;
    }

    /* Free remaining completion queue tasks */
    task = pool->done.head;
    while (task) {
        ThreadPoolTask* next = task->next;
        free(task);
        task = next;
    }

    pthread_cond_destroy(&pool->idle_cond);
    pthread_mutex_destroy(&pool->stats_mutex);
    pthread_mutex_destroy(&pool->done.mutex);
    pthread_cond_destroy(&pool->work.cond);
    pthread_mutex_destroy(&pool->work.mutex);
    free(pool->threads);
    free(pool);
}

ThreadPoolTask* thread_pool_submit(ThreadPool* pool, ThreadPoolWorkFunc work_fn,
                                   void* work_arg, ThreadPoolDoneFunc done_fn,
                                   void* done_arg, int timeout_ms) {
    if (!pool || pool->shutdown) {
        return NULL;
    }

    ThreadPoolTask* task = calloc(1, sizeof(ThreadPoolTask));
    if (!task) {
        return NULL;
    }

    task->work_fn  = work_fn;
    task->work_arg = work_arg;
    task->done_fn  = done_fn;
    task->done_arg = done_arg;

    if (timeout_ms > 0) {
        task->deadline_ms = now_monotonic_ms() + (uint64_t)timeout_ms;
    }

    pthread_mutex_lock(&pool->work.mutex);

    /* Backpressure: reject if queue is full */
    if (pool->max_pending > 0 && pool->work.count >= pool->max_pending) {
        pthread_mutex_unlock(&pool->work.mutex);
        free(task);
        return NULL;
    }

    queue_push(&pool->work.head, &pool->work.tail, task);
    pool->work.count++;
    pthread_cond_signal(&pool->work.cond);
    pthread_mutex_unlock(&pool->work.mutex);

    return task;
}

void thread_pool_cancel(ThreadPoolTask* task) {
    if (task) {
        task->cancelled = 1;
    }
}

int thread_pool_task_is_cancelled(ThreadPoolTask* task) {
    return task ? task->cancelled : 0;
}

int thread_pool_task_remaining_ms(ThreadPoolTask* task) {
    if (!task || task->deadline_ms == 0) {
        return 0; /* no timeout */
    }
    uint64_t now = now_monotonic_ms();
    if (now >= task->deadline_ms) {
        return -1; /* expired */
    }
    return (int)(task->deadline_ms - now);
}

int thread_pool_process_completions(ThreadPool* pool) {
    if (!pool) {
        return 0;
    }

    /* Take all completed tasks at once (minimizes lock time) */
    pthread_mutex_lock(&pool->done.mutex);
    ThreadPoolTask* list = queue_take_all(&pool->done.head, &pool->done.tail);
    pthread_mutex_unlock(&pool->done.mutex);

    /* Dispatch callbacks on the main thread */
    int count = 0;
    while (list) {
        ThreadPoolTask* next = list->next;
        if (list->done_fn) {
            list->done_fn(list->done_arg, list->status);
        }
        free(list);
        list = next;
        count++;
    }

    return count;
}

void thread_pool_get_stats(ThreadPool* pool, ThreadPoolStats* stats) {
    if (!pool || !stats) {
        return;
    }

    stats->num_workers = pool->num_workers;

    pthread_mutex_lock(&pool->stats_mutex);
    stats->active_workers  = pool->active_workers;
    stats->completed_tasks = pool->completed_total;
    pthread_mutex_unlock(&pool->stats_mutex);

    pthread_mutex_lock(&pool->work.mutex);
    stats->pending_tasks = pool->work.count;
    pthread_mutex_unlock(&pool->work.mutex);
}

void thread_pool_wait_idle(ThreadPool* pool) {
    if (!pool) {
        return;
    }

    pthread_mutex_lock(&pool->stats_mutex);
    for (;;) {
        pthread_mutex_lock(&pool->work.mutex);
        int pending = pool->work.count;
        pthread_mutex_unlock(&pool->work.mutex);

        if (pending == 0 && pool->active_workers == 0) {
            break;
        }

        pthread_cond_wait(&pool->idle_cond, &pool->stats_mutex);
    }
    pthread_mutex_unlock(&pool->stats_mutex);
}

void thread_pool_smw_callback(void* context, uint64_t mon_time) {
    (void)mon_time;
    thread_pool_process_completions((ThreadPool*)context);
}
