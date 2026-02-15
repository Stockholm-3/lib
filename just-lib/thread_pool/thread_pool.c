/**
 * @file thread_pool.c
 * @brief Thread pool implementation using POSIX threads.
 */

#include "thread_pool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============= Internal Types ============= */

typedef struct Task {
    ThreadPoolWorkFunc work_fn;
    void*              work_arg;
    ThreadPoolDoneFunc done_fn;
    void*              done_arg;
    struct Task*       next;
} Task;

typedef struct {
    Task*           head;
    Task*           tail;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} WorkQueue;

typedef struct {
    Task*           head;
    Task*           tail;
    pthread_mutex_t mutex;
} CompletionQueue;

struct ThreadPool {
    pthread_t*      threads;
    int             num_workers;
    volatile int    shutdown;
    WorkQueue       work;
    CompletionQueue done;
};

/* ============= Queue Helpers ============= */

static void queue_push(Task** head, Task** tail, Task* task) {
    task->next = NULL;
    if (*tail) {
        (*tail)->next = task;
    } else {
        *head = task;
    }
    *tail = task;
}

static Task* queue_pop(Task** head, Task** tail) {
    Task* task = *head;
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

static Task* queue_take_all(Task** head, Task** tail) {
    Task* list = *head;
    *head      = NULL;
    *tail      = NULL;
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

        Task* task = queue_pop(&pool->work.head, &pool->work.tail);
        pthread_mutex_unlock(&pool->work.mutex);

        if (!task) {
            continue;
        }

        /* Execute work in this worker thread */
        if (task->work_fn) {
            task->work_fn(task->work_arg);
        }

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

ThreadPool* thread_pool_create(int num_workers) {
    if (num_workers <= 0) {
        return NULL;
    }

    ThreadPool* pool = calloc(1, sizeof(ThreadPool));
    if (!pool) {
        return NULL;
    }

    pool->num_workers = num_workers;
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

    pool->threads = calloc(num_workers, sizeof(pthread_t));
    if (!pool->threads) {
        pthread_mutex_destroy(&pool->done.mutex);
        pthread_cond_destroy(&pool->work.cond);
        pthread_mutex_destroy(&pool->work.mutex);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            /* Shut down already-started threads */
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work.cond);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_mutex_destroy(&pool->done.mutex);
            pthread_cond_destroy(&pool->work.cond);
            pthread_mutex_destroy(&pool->work.mutex);
            free(pool->threads);
            free(pool);
            return NULL;
        }
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
    Task* task = pool->work.head;
    while (task) {
        Task* next = task->next;
        free(task);
        task = next;
    }

    /* Free remaining completion queue tasks */
    task = pool->done.head;
    while (task) {
        Task* next = task->next;
        free(task);
        task = next;
    }

    pthread_mutex_destroy(&pool->done.mutex);
    pthread_cond_destroy(&pool->work.cond);
    pthread_mutex_destroy(&pool->work.mutex);
    free(pool->threads);
    free(pool);
}

int thread_pool_submit(ThreadPool* pool, ThreadPoolWorkFunc work_fn,
                       void* work_arg, ThreadPoolDoneFunc done_fn,
                       void* done_arg) {
    if (!pool || pool->shutdown) {
        return -1;
    }

    Task* task = malloc(sizeof(Task));
    if (!task) {
        return -1;
    }

    task->work_fn  = work_fn;
    task->work_arg = work_arg;
    task->done_fn  = done_fn;
    task->done_arg = done_arg;
    task->next     = NULL;

    pthread_mutex_lock(&pool->work.mutex);
    queue_push(&pool->work.head, &pool->work.tail, task);
    pthread_cond_signal(&pool->work.cond);
    pthread_mutex_unlock(&pool->work.mutex);

    return 0;
}

int thread_pool_process_completions(ThreadPool* pool) {
    if (!pool) {
        return 0;
    }

    /* Take all completed tasks at once (minimizes lock time) */
    pthread_mutex_lock(&pool->done.mutex);
    Task* list = queue_take_all(&pool->done.head, &pool->done.tail);
    pthread_mutex_unlock(&pool->done.mutex);

    /* Dispatch callbacks on the main thread */
    int count = 0;
    while (list) {
        Task* next = list->next;
        if (list->done_fn) {
            list->done_fn(list->done_arg);
        }
        free(list);
        list = next;
        count++;
    }

    return count;
}

void thread_pool_smw_callback(void* context, uint64_t mon_time) {
    (void)mon_time;
    thread_pool_process_completions((ThreadPool*)context);
}
