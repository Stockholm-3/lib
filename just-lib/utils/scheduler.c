#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include "scheduler.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define SHUTDOWN_POLL_MS 100

typedef enum { TIMER_INTERVAL, TIMER_DAILY, TIMER_ALIGNED_UTC } TimerType;

typedef struct {
    int hour;
    int minute;
    int second;
} DailyArg;

struct SchedulerTimer {
    TimerType     type;
    TimerCallback callback;
    uint64_t      interval_ms; // for interval timers

    // aligned timer
    uint64_t aligned_ms; // grid size
    uint64_t offset_ms;  // phase offset

    DailyArg daily; // for daily timers

    uint64_t next_mono_ms; // absolute MONOTONIC deadline
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

static uint64_t realtime_ms_to_monotonic_ms(uint64_t rt_ms) {
    uint64_t rt_now_ms = realtime_now_ms();
    uint64_t mono_now  = mono_now_ms();
    int64_t  diff_ms   = (int64_t)rt_ms - (int64_t)rt_now_ms;
    return mono_now + diff_ms;
}

static uint64_t next_aligned_utc_ms(uint64_t interval_ms, uint64_t offset_ms) {
    uint64_t now = realtime_now_ms();

    if (offset_ms >= interval_ms) {
        offset_ms %= interval_ms;
    }

    uint64_t base;

    if (now >= offset_ms) {
        base = now - offset_ms;
    } else {
        base = 0;
    }

    uint64_t rem = base % interval_ms;

    uint64_t delta;

    if (rem == 0) {
        delta = interval_ms;
    } else {
        delta = interval_ms - rem;
    }

    return now + delta;
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

SchedulerTimer* create_aligned_timer_utc(uint64_t interval_ms,
                                         uint64_t offset_ms, TimerCallback cb) {
    if (!cb || interval_ms == 0) {
        return NULL;
    }

    SchedulerTimer* t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }

    t->type       = TIMER_ALIGNED_UTC;
    t->aligned_ms = interval_ms;
    t->offset_ms  = offset_ms;
    t->callback   = cb;

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

    // Initialise deadliunes
    uint64_t now_mono = mono_now_ms();
    for (size_t i = 0; i < count; i++) {
        switch (timers[i]->type) {
        case TIMER_INTERVAL:
            timers[i]->next_mono_ms = now_mono + timers[i]->interval_ms;
            break;
        case TIMER_DAILY: {
            time_t rt               = next_daily_realtime(&timers[i]->daily);
            timers[i]->next_mono_ms = realtime_sec_to_monotonic_ms(rt);
            break;
        }
        case TIMER_ALIGNED_UTC: {
            uint64_t rt             = next_aligned_utc_ms(timers[i]->aligned_ms,
                                                          timers[i]->offset_ms);
            timers[i]->next_mono_ms = realtime_sec_to_monotonic_ms(rt / 1000);
            break;
        }
        }
    }

    struct pollfd pfd = {.fd = tfd, .events = POLLIN};

    while (!*shutdown_flag) {
        // Find the earliest deadline
        uint64_t earliest = UINT64_MAX;
        for (size_t i = 0; i < count; i++) {
            if (timers[i]->next_mono_ms < earliest) {
                earliest = timers[i]->next_mono_ms;
            }
        }

        now_mono = mono_now_ms();

        uint64_t wait_ms = (earliest > now_mono) ? (earliest - now_mono) : 0;

        // Cap the timerfd wait to SHUTDOWN_POLL_MS so we never block too long
        uint64_t arm_ms =
            (wait_ms < SHUTDOWN_POLL_MS) ? wait_ms : SHUTDOWN_POLL_MS;

        struct itimerspec it = {0};
        it.it_value.tv_sec   = arm_ms / 1000;
        it.it_value.tv_nsec  = (arm_ms % 1000) * 1000000;

        if (arm_ms == 0) {
            it.it_value.tv_nsec = 1;
        }

        if (timerfd_settime(tfd, 0, &it, NULL) < 0) {
            perror("timerfd_settime");
            break;
        }

        int ready = poll(&pfd, 1, (int)arm_ms + 1);

        if (*shutdown_flag) {
            break;
        }

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (ready == 0) {
            continue;
        }

        uint64_t expirations;
        ssize_t  n = read(tfd, &expirations, sizeof(expirations));
        if (n != sizeof(expirations)) {
            if (errno != EAGAIN) {
                perror("timerfd read");
                break;
            }
            continue;
        }

        now_mono = mono_now_ms();

        // Fire any timers whose deadline has passed
        for (size_t i = 0; i < count; i++) {
            if (timers[i]->next_mono_ms > now_mono) {
                continue;
            }

            timers[i]->callback();

            // Reschedule
            switch (timers[i]->type) {
            case TIMER_INTERVAL:
                timers[i]->next_mono_ms += timers[i]->interval_ms;
                break;
            case TIMER_DAILY: {
                time_t rt = next_daily_realtime(&timers[i]->daily);
                timers[i]->next_mono_ms = realtime_sec_to_monotonic_ms(rt);
                break;
            }
            case TIMER_ALIGNED_UTC: {
                uint64_t last_scheduled_rt_ms =
                    timers[i]->next_mono_ms - mono_now_ms() + realtime_now_ms();
                uint64_t next_rt_ms =
                    last_scheduled_rt_ms + timers[i]->aligned_ms;
                timers[i]->next_mono_ms =
                    realtime_ms_to_monotonic_ms(next_rt_ms);
                break;
            }
            }
        }
    }

    close(tfd);
}
