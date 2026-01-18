/**
 * @file smw.c
 * @brief Simple scheduler / middleware worker implementation.
 *
 * This module implements a very lightweight task system where callbacks
 * can be registered and executed during a work cycle.
 *
 * Tasks are stored internally in a linked list and executed sequentially.
 * Each task consists of a user-provided context pointer and a callback
 * function.
 *
 * @note This module is NOT thread-safe.
 * @note Callbacks may safely remove their own task, but must not assume
 *       iteration order stability beyond the current callback.
 */

#include "smw.h"
#include "linked_list.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Global SMW state.
 *
 * Holds the internal task list. This state is initialized by smw_init()
 * and released by smw_dispose().
 */
Smw g_smw;

/**
 * @brief Initialize the SMW system.
 *
 * This function must be called before any other SMW functions.
 * It initializes internal state and allocates the task list.
 *
 * @return
 *   - 0 on success
 *   - -1 on allocation failure
 */
int smw_init() {
    memset(&g_smw, 0, sizeof(g_smw));

    g_smw.tasks = linked_list_create();
    if (!g_smw.tasks) {
        return -1;
    }

    return 0;
}

/**
 * @brief Create and register a new SMW task.
 *
 * Allocates a new task structure and appends it to the internal task list.
 *
 * @param context  User-defined pointer passed to the callback.
 * @param callback Function invoked during smw_work().
 *
 * @return
 *   - Pointer to the created task on success
 *   - NULL on failure
 *
 * @note The returned task is owned by the SMW system and will be freed
 *       automatically when removed or when smw_dispose() is called.
 */
SmwTask* smw_create_task(void* context,
                         void (*callback)(void* context, uint64_t mon_time)) {
    if (!g_smw.tasks) {
        return NULL;
    }

    SmwTask* task = malloc(sizeof(SmwTask));
    if (!task) {
        return NULL;
    }

    task->context  = context;
    task->callback = callback;

    if (linked_list_append(g_smw.tasks, task) != 0) {
        free(task);
        return NULL;
    }

    return task;
}

/**
 * @brief Destroy and unregister a task.
 *
 * Searches the internal task list for the specified task and removes it.
 * The task memory is freed automatically.
 *
 * @param task Pointer to the task to destroy.
 *
 * @note It is safe to call this function from within a task callback.
 */
void smw_destroy_task(SmwTask* task) {
    if (!g_smw.tasks || !task) {
        return;
    }

    Node* node = g_smw.tasks->head;
    while (node) {
        Node* next = node->front;
        if (node->item == task) {
            linked_list_remove(g_smw.tasks, node, free);
            break;
        }
        node = next;
    }
}

/**
 * @brief Execute all registered tasks.
 *
 * Iterates through the task list and invokes each task's callback.
 *
 * @param mon_time Monotonic timestamp passed to callbacks.
 *
 * @note The next node is cached before callback execution to allow
 *       safe removal of the current task during iteration.
 */
void smw_work(uint64_t mon_time) {
    if (!g_smw.tasks) {
        return;
    }

    Node* node = g_smw.tasks->head;
    while (node) {
        Node*    next = node->front; /* Save next node before callback */
        SmwTask* task = (SmwTask*)node->item;

        if (task && task->callback) {
            task->callback(task->context, mon_time);
        }

        node = next;
    }
}

/**
 * @brief Get the number of registered tasks.
 *
 * @return Number of active tasks, or 0 if uninitialized.
 */
int smw_get_task_count() {
    if (!g_smw.tasks) {
        return 0;
    }

    return (int)g_smw.tasks->size;
}

/**
 * @brief Dispose of the SMW system.
 *
 * Frees all registered tasks and releases internal resources.
 * After calling this function, the SMW system must be reinitialized
 * before use.
 */
void smw_dispose() {
    if (!g_smw.tasks) {
        return;
    }

    linked_list_dispose(&g_smw.tasks, free);
}
