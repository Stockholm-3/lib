/**
 * @file scheduler.h
 * @brief Simple timer scheduler for intervals and daily events.
 *
 * This module provides a lightweight scheduler for running repeated
 * or daily tasks in a single-threaded, blocking event loop.
 *
 * Features:
 *   - Interval timers: callbacks run every N milliseconds.
 *   - Daily timers: callbacks run once per day at a specified hour, minute, and
 * second.
 *   - Graceful shutdown via a volatile sig_atomic_t shutdown flag.
 *
 * Design:
 *   - Uses a single timerfd and monotonic timestamps to calculate the next
 * timer expiration.
 *   - Efficient for multiple timers without requiring additional threads.
 *   - Blocking loop; does not integrate with other file descriptors (sockets,
 * pipes, etc.). Users may extend the scheduler with poll() or epoll() if
 * needed.
 *
 * Usage:
 * @code
 * #include "scheduler.h"
 * #include <signal.h>
 * #include <stdio.h>
 *
 * volatile sig_atomic_t shutdown_flag = 0;
 *
 * void my_callback(void) {
 *     printf("Timer fired!\n");
 * }
 *
 * int main(void) {
 *     SchedulerTimer* t1 = create_interval_timer(1000, my_callback); // every
 * 1s SchedulerTimer* t2 = create_daily_timer(12, 30, 0, my_callback); //
 * 12:30:00 daily
 *
 *     SchedulerTimer* timers[] = {t1, t2};
 *     run_scheduler(timers, 2, &shutdown_flag);
 *
 *     destroy_timer(t1);
 *     destroy_timer(t2);
 *     return 0;
 * }
 * @endcode
 *
 * @note The scheduler is not thread-safe. All timers should be created
 *       and used in the same thread as run_scheduler.
 * @note Timers should not be destroyed while run_scheduler is active.
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Callback function type for timers.
 *
 * This function is called when a timer fires.
 * It takes no parameters and returns nothing.
 */
typedef void (*TimerCallback)(void);

/**
 * @brief Opaque handle for a scheduler timer.
 *
 * Use the factory functions to create timers.
 * Use destroy_timer() to free them.
 */
typedef struct SchedulerTimer SchedulerTimer;

/**
 * @brief Run the scheduler loop.
 *
 * This function blocks the calling thread and repeatedly waits for the next
 * timer to fire. When a timer expires, its callback is invoked.
 *
 * @param timers Pointer to an array of SchedulerTimer pointers.
 * @param count Number of timers in the array.
 * @param shutdown_flag Pointer to a volatile sig_atomic_t flag. If the value
 *                      becomes non-zero, the scheduler will exit the loop
 *                      and return. This is intended for handling shutdown
 *                      signals safely (e.g., SIGINT, SIGTERM).
 *
 * @note The scheduler uses a single timerfd internally and calculates the
 *       next timer expiration. It is accurate and efficient for multiple
 * timers, but currently does not integrate other file descriptors. If you need
 *       to wait on sockets or pipes simultaneously, you would need to
 *       extend it with poll()/epoll() yourself.
 *
 * @note This function blocks indefinitely until shutdown_flag is non-zero.
 */
void run_scheduler(SchedulerTimer** timers, size_t count,
                   const volatile sig_atomic_t* shutdown_flag);

/**
 * @brief Create a repeating interval timer.
 *
 * The callback will be invoked repeatedly every interval_ms milliseconds.
 *
 * @param interval_ms Interval in milliseconds between timer callbacks.
 * @param cb Function pointer to the callback to execute.
 * @return Pointer to a SchedulerTimer, or NULL if allocation fails.
 *
 * @note The interval starts counting from when run_scheduler is called.
 */
SchedulerTimer* create_interval_timer(uint64_t interval_ms, TimerCallback cb);

/**
 * @brief Create a daily timer that fires at a specific time of day.
 *
 * The callback will fire once per day at the given hour, minute, and second.
 *
 * @param hour Hour of the day [0-23].
 * @param minute Minute of the hour [0-59].
 * @param second Second of the minute [0-59].
 * @param cb Function pointer to the callback to execute.
 * @return Pointer to a SchedulerTimer, or NULL if allocation fails or
 *         invalid time values are provided.
 *
 * @note The scheduler uses the local time zone of the system. If the
 *       specified time has already passed today, the callback will be
 *       scheduled for the next day.
 */
SchedulerTimer* create_daily_timer(int hour, int minute, int second,
                                   TimerCallback cb);

/**
 * @brief Destroy a timer and free its memory.
 *
 * @param t Pointer to the SchedulerTimer to destroy.
 * @note Do not call this on a timer while it is actively firing in
 *       run_scheduler. Destroy only after run_scheduler has returned
 *       or before starting it.
 */
void destroy_timer(SchedulerTimer* t);

#endif // SCHEDULER_H
