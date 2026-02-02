#include <errno.h>
#include <signal.h>
#define _GNU_SOURCE
#include "scheduler.h"

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* ---------- internal ---------- */

typedef enum { TIMER_INTERVAL, TIMER_DAILY } TimerType;

typedef struct {
    int hour;
    int minute;
    int second;
} DailyArg;

struct SchedulerTimer {
    TimerType     type;
    TimerCallback callback;
    uint64_t      interval_ms;  // for interval timers
    DailyArg      daily;        // for daily timers
    uint64_t      next_mono_ms; // absolute MONOTONIC deadline
};

// Return current monotonic time in milliseconds
static uint64_t mono_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Return current real-time in milliseconds
static uint64_t realtime_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Convert absolute real-time seconds to monotonic ms
// Useful for aligning daily timers with the timerfd
static uint64_t realtime_sec_to_monotonic_ms(time_t rt_sec) {
    uint64_t rt_now_ms = realtime_now_ms();
    uint64_t mono_now  = mono_now_ms();
    int64_t  diff_ms   = (int64_t)(rt_sec * 1000) - (int64_t)rt_now_ms;
    return mono_now + diff_ms;
}

// Compute next real-time fire for daily timer
// Returns a time_t representing the next occurrence
static time_t next_daily_realtime(const DailyArg* d) {
    time_t    now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    tm_now.tm_hour  = d->hour;
    tm_now.tm_min   = d->minute;
    tm_now.tm_sec   = d->second;
    tm_now.tm_isdst = -1;

    time_t t = mktime(&tm_now);
    if (t <= now) {
        t += (time_t)(24 * 3600);
    }
    return t;
}

SchedulerTimer* create_interval_timer(uint64_t interval_ms, TimerCallback cb) {
    SchedulerTimer* t = calloc(1, sizeof(*t));
    t->type           = TIMER_INTERVAL;
    t->interval_ms    = interval_ms;
    t->callback       = cb;
    return t;
}

SchedulerTimer* create_daily_timer(int hour, int minute, int second,
                                   TimerCallback cb) {
    if (!cb) {
        return NULL;
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59) {
        return NULL;
    }

    SchedulerTimer* t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }

    t->type         = TIMER_DAILY;
    t->daily.hour   = hour;
    t->daily.minute = minute;
    t->daily.second = second;
    t->callback     = cb;
    return t;
}

void destroy_timer(SchedulerTimer* t) { free(t); }

void run_scheduler(SchedulerTimer** timers, size_t count,
                   const volatile sig_atomic_t* shutdown_flag) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) {
        perror("timerfd_create");
        return;
    }

    /* initialize next fire times */
    uint64_t now_ms = mono_now_ms();
    for (size_t i = 0; i < count; i++) {
        if (timers[i]->type == TIMER_INTERVAL) {
            timers[i]->next_mono_ms = now_ms + timers[i]->interval_ms;
        } else {
            time_t rt               = next_daily_realtime(&timers[i]->daily);
            timers[i]->next_mono_ms = realtime_sec_to_monotonic_ms(rt);
        }
    }

    while (!*shutdown_flag) {
        // find earliest deadline
        uint64_t earliest = UINT64_MAX;
        for (size_t i = 0; i < count; i++) {
            if (timers[i]->next_mono_ms < earliest) {
                earliest = timers[i]->next_mono_ms;
            }
        }

        // calculate next time spec
        now_ms           = mono_now_ms();
        uint64_t wait_ms = (earliest > now_ms) ? (earliest - now_ms) : 0;

        struct itimerspec it = {0};

        it.it_value.tv_sec  = (time_t)(wait_ms / 1000);
        it.it_value.tv_nsec = (long)((wait_ms % 1000) * 1000000);

        timerfd_settime(tfd, 0, &it, NULL);

        uint64_t expirations;

        ssize_t n;

        do {
            n = read(tfd, &expirations, sizeof(expirations));
        } while (n < 0 && errno == EINTR && !*shutdown_flag);

        if (*shutdown_flag) {
            break;
        }

        if (n != sizeof(expirations)) {
            perror("timerfd read");
            break;
        }

        now_ms = mono_now_ms();

        for (size_t i = 0; i < count; i++) {
            if (timers[i]->next_mono_ms <= now_ms) {
                timers[i]->callback();

                if (timers[i]->type == TIMER_INTERVAL) {
                    timers[i]->next_mono_ms = now_ms + timers[i]->interval_ms;
                } else {
                    time_t rt = next_daily_realtime(&timers[i]->daily);
                    timers[i]->next_mono_ms = realtime_sec_to_monotonic_ms(rt);
                }
            }
        }
    }
}
