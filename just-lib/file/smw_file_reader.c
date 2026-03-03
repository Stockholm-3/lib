#include "smw_file_reader.h"

#include "file_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <smw.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 4096

typedef enum {
    S_INIT,
    S_LOCK,
    S_OPEN,
    S_READ,
    S_DONE,
    S_ERROR,
    S_DISPOSE,
} State;

typedef struct {
    void* task;

    char lock_path[512];
    char file_path[512];

    int lock_fd;
    int file_fd;

    char*  buf;
    size_t buf_len;
    size_t buf_cap;

    SfrStatus result;
    State     state;

    void*     cb_ctx;
    SfrOnDone on_done;
} SfrTask;

static void do_lock(SfrTask* t) {
    if (t->lock_fd < 0) {
        t->lock_fd = file_lock_try_shared(t->lock_path);
        if (t->lock_fd < 0) {
            if (errno == EWOULDBLOCK) {
                return;
            }
            t->result = SFR_LOCK_ERROR;
            t->state  = S_ERROR;
            return;
        }
    }
    t->state = S_OPEN;
}

static void do_open(SfrTask* t) {
    t->file_fd = open(t->file_path, O_RDONLY | O_NONBLOCK);
    if (t->file_fd < 0) {
        t->result = (errno == ENOENT) ? SFR_NOT_FOUND : SFR_READ_ERROR;
        t->state  = S_ERROR;
        return;
    }
    t->state = S_READ;
}

static void do_read(SfrTask* t) {
    char    chunk[CHUNK];
    ssize_t n = read(t->file_fd, chunk, sizeof(chunk));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        t->result = SFR_READ_ERROR;
        t->state  = S_ERROR;
        return;
    }
    if (n > 0) {
        size_t need = t->buf_len + (size_t)n;
        if (need > t->buf_cap) {
            size_t nc = need + CHUNK;
            char*  nb = realloc(t->buf, nc + 1);
            if (!nb) {
                t->result = SFR_READ_ERROR;
                t->state  = S_ERROR;
                return;
            }
            t->buf     = nb;
            t->buf_cap = nc;
        }
        memcpy(t->buf + t->buf_len, chunk, (size_t)n);
        t->buf_len += (size_t)n;
        return;
    }
    // EOF
    if (t->buf) {
        t->buf[t->buf_len] = '\0';
    }
    file_lock_release(t->lock_fd);
    t->lock_fd = -1;
    close(t->file_fd);
    t->file_fd = -1;
    t->result  = SFR_OK;
    t->state   = S_DONE;
}

static void do_dispose(SfrTask* t) {
    if (t->task) {
        smw_destroy_task(t->task);
        t->task = NULL;
    }
    if (t->lock_fd >= 0) {
        file_lock_release(t->lock_fd);
        t->lock_fd = -1;
    }
    if (t->file_fd >= 0) {
        close(t->file_fd);
        t->file_fd = -1;
    }
    free(t->buf);
    memset(t, 0, sizeof(*t));
    free(t);
}

void smw_file_reader_work(void* context, uint64_t mon_time) {
    (void)mon_time;
    SfrTask* t = (SfrTask*)context;
    switch (t->state) {
    case S_INIT:
        t->state = S_LOCK;
        break;
    case S_LOCK:
        do_lock(t);
        break;
    case S_OPEN:
        do_open(t);
        break;
    case S_READ:
        do_read(t);
        break;
    case S_DONE: {
        char*  buf = t->buf;
        size_t len = t->buf_len;
        t->buf     = NULL;
        t->buf_len = 0;
        t->on_done(t->cb_ctx, SFR_OK, buf, len);
        t->state = S_DISPOSE;
        break;
    }
    case S_ERROR:
        if (t->lock_fd >= 0) {
            file_lock_release(t->lock_fd);
            t->lock_fd = -1;
        }
        if (t->file_fd >= 0) {
            close(t->file_fd);
            t->file_fd = -1;
        }
        t->on_done(t->cb_ctx, t->result, NULL, 0);
        t->state = S_DISPOSE;
        break;
    case S_DISPOSE:
        do_dispose(t);
        break;
    }
}

int smw_file_reader_start(const char* lock_path, const char* file_path,
                          void* context, SfrOnDone on_done) {
    if (!lock_path || !file_path || !on_done) {
        return -1;
    }

    SfrTask* t = calloc(1, sizeof(*t));
    if (!t) {
        return -1;
    }

    strncpy(t->lock_path, lock_path, sizeof(t->lock_path) - 1);
    strncpy(t->file_path, file_path, sizeof(t->file_path) - 1);
    t->lock_fd = -1;
    t->file_fd = -1;
    t->cb_ctx  = context;
    t->on_done = on_done;
    t->state   = S_INIT;

    t->task = smw_create_task(t, smw_file_reader_work);
    if (!t->task) {
        free(t);
        return -1;
    }
    return 0;
}
