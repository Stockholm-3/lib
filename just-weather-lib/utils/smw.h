#ifndef SMW_H
#define SMW_H

#include "linked_list.h"

#include <stdint.h>

#ifndef SMW_MAX_TASKS
#    define SMW_MAX_TASKS 16
#endif

typedef struct {
    void* context;
    void (*callback)(void* context, uint64_t mon_time);

} SmwTask;

typedef struct {
    LinkedList* tasks;
} Smw;

extern Smw g_smw;

/**
 * @brief Initialize the global state machine worker (SMW).
 *
 * Sets up the internal task list and prepares the SMW for scheduling tasks.
 *
 * @return
 *   - 0 on success
 *   - -1 if task list allocation fails
 */
int smw_init();

/**
 * @brief Create and register a new task with the state machine worker.
 *
 * Allocates a SmwTask structure, sets its context and callback, and
 * appends it to the global task list.
 *
 * @param context  Pointer to user-defined context passed to the callback.
 * @param callback Function to be called periodically for this task.
 *
 * @return
 *   - Pointer to the newly created SmwTask on success
 *   - NULL on failure (allocation failure or task list not initialized)
 */
SmwTask* smw_create_task(void* context,
                         void (*callback)(void* context, uint64_t mon_time));

/**
 * @brief Remove and free a task from the state machine worker.
 *
 * Searches the global task list for the specified task, removes it,
 * and frees its memory.
 *
 * @param task Pointer to the SmwTask to remove.
 */
void smw_destroy_task(SmwTask* task);

/**
 * @brief Execute all registered tasks.
 *
 * Iterates over the SMW task list and calls each task's callback,
 * passing in the associated context and the current monotonic time.
 *
 * This function is intended to be called periodically, e.g., from
 * a main loop or timer tick.
 *
 * @param mon_time Current monotonic time in ticks or milliseconds.
 */
void smw_work(uint64_t mon_time);

/**
 * @brief Get the number of currently registered tasks.
 *
 * @return Number of tasks in the SMW task list, or 0 if uninitialized.
 */
int smw_get_task_count();

/**
 * @brief Dispose the state machine worker and free all tasks.
 *
 * Frees all memory associated with the task list and tasks.
 * After calling this, the SMW must be re-initialized with smw_init()
 * to register new tasks.
 */
void smw_dispose();

#endif // SMW_H
