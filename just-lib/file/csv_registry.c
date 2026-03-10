#include "csv_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <smw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 4096

typedef enum {
    RS_INIT,
    RS_OPEN,
    RS_LOCK,
    RS_READ,
    RS_PROCESS,
    RS_SEEK,
    RS_WRITE,
    RS_DONE,
    RS_ERROR,
    RS_DISPOSE,
} RState;

typedef struct {
    void* task;
    int   fd;

    char*  rbuf;
    size_t rbuf_len;
    size_t rbuf_cap;

    char*  wbuf;
    size_t wbuf_len;
    size_t wbuf_off;

    CsvRow* rows;
    int     row_count;
    int     max_rows;

    char   csv_path[512];
    char   key[256];
    char   tag[32];
    double f1, f2;

    CsvRegStatus result;
    RState       state;

    void*        cb_ctx;
    CsvRegOnDone on_done;
} RegTask;

static void parse_rows(RegTask* t) {
    t->row_count = 0;
    time_t now   = time(NULL);
    int    found = 0;

    char* cur = t->rbuf;
    char* end = t->rbuf + t->rbuf_len;

    while (cur < end) {
        char*  nl   = memchr(cur, '\n', (size_t)(end - cur));
        size_t llen = nl ? (size_t)(nl - cur) : (size_t)(end - cur);
        char*  next = nl ? nl + 1 : end;

        if (llen == 0) {
            cur = next;
            continue;
        }

        char line[512];
        if (llen >= sizeof(line)) {
            llen = sizeof(line) - 1;
        }
        memcpy(line, cur, llen);
        line[llen] = '\0';
        cur        = next;

        if (t->row_count >= t->max_rows * 2) {
            break;
        }

        CsvRow r  = {0};
        char*  sp = NULL;

        char* tok = strtok_r(line, ",", &sp);
        if (!tok || tok[0] == '\0') {
            continue;
        }
        strncpy(r.key, tok, sizeof(r.key) - 1);

        tok = strtok_r(NULL, ",", &sp);
        strncpy(r.tag, tok ? tok : "", sizeof(r.tag) - 1);

        tok  = strtok_r(NULL, ",", &sp);
        r.f1 = tok ? atof(tok) : 0.0;

        tok  = strtok_r(NULL, ",", &sp);
        r.f2 = tok ? atof(tok) : 0.0;

        tok             = strtok_r(NULL, ",", &sp);
        r.last_accessed = tok ? (time_t)atoll(tok) : now;

        if (strcmp(r.key, t->key) == 0 && strcmp(r.tag, t->tag) == 0) {
            found           = 1;
            r.last_accessed = now;
        }

        t->rows[t->row_count++] = r;
    }

    if (found) {
        t->result = CSV_REG_EXISTS;
        return;
    }

    int live = t->row_count;

    if (live >= t->max_rows) {
        int oldest = 0;
        for (int i = 1; i < t->row_count; i++) {
            if (t->rows[i].last_accessed < t->rows[oldest].last_accessed) {
                oldest = i;
            }
        }
        // Remove by shifting
        memmove(&t->rows[oldest], &t->rows[oldest + 1],
                (size_t)(t->row_count - oldest - 1) * sizeof(CsvRow));
        t->row_count--;
    }

    CsvRow* nr = &t->rows[t->row_count];
    strncpy(nr->key, t->key, sizeof(nr->key) - 1);
    strncpy(nr->tag, t->tag, sizeof(nr->tag) - 1);
    nr->f1            = t->f1;
    nr->f2            = t->f2;
    nr->last_accessed = now;
    t->row_count++;
    t->result = CSV_REG_ADDED;
}

static int build_write_buf(RegTask* t) {
    size_t cap = (size_t)(t->row_count + 1) * 512;
    char*  buf = malloc(cap);
    if (!buf) {
        return -1;
    }
    size_t off = 0;
    for (int i = 0; i < t->row_count; i++) {
        CsvRow* r = &t->rows[i];
        int w = snprintf(buf + off, cap - off, "%s,%s,%.6f,%.6f,%ld\n", r->key,
                         r->tag, r->f1, r->f2, (long)r->last_accessed);
        if (w < 0 || (size_t)w >= cap - off) {
            free(buf);
            return -1;
        }
        off += (size_t)w;
    }
    t->wbuf     = buf;
    t->wbuf_len = off;
    t->wbuf_off = 0;
    return 0;
}

static void rs_open(RegTask* t) {
    int fd = open(t->csv_path, O_RDWR | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        fd = open(t->csv_path, O_RDWR | O_CREAT | O_NONBLOCK, 0644);
    }
    if (fd < 0) {
        t->state = RS_ERROR;
        return;
    }
    t->fd    = fd;
    t->state = RS_LOCK;
}

static void rs_lock(RegTask* t) {
    if (flock(t->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            return;
        }
        t->state = RS_ERROR;
        { return; }
    }
    t->state = RS_READ;
}

static void rs_read(RegTask* t) {
    char    chunk[CHUNK];
    ssize_t n = read(t->fd, chunk, sizeof(chunk));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        t->state = RS_ERROR;
        return;
    }
    if (n > 0) {
        size_t need = t->rbuf_len + (size_t)n;
        if (need > t->rbuf_cap) {
            size_t nc = need + CHUNK;
            char*  nb = realloc(t->rbuf, nc);
            if (!nb) {
                t->state = RS_ERROR;
                return;
            }
            t->rbuf     = nb;
            t->rbuf_cap = nc;
        }
        memcpy(t->rbuf + t->rbuf_len, chunk, (size_t)n);
        t->rbuf_len += (size_t)n;
        return;
    }
    // EOF
    t->state = RS_PROCESS;
}

static void rs_process(RegTask* t) {
    parse_rows(t);
    if (build_write_buf(t) != 0) {
        t->state = RS_ERROR;
        return;
    }
    t->state = RS_SEEK;
}

static void rs_seek(RegTask* t) {
    if (lseek(t->fd, 0, SEEK_SET) != 0 || ftruncate(t->fd, 0) != 0) {
        t->state = RS_ERROR;
        return;
    }
    t->state = RS_WRITE;
}

static void rs_write(RegTask* t) {
    while (t->wbuf_off < t->wbuf_len) {
        ssize_t n =
            write(t->fd, t->wbuf + t->wbuf_off, t->wbuf_len - t->wbuf_off);
        if (n > 0) {
            t->wbuf_off += (size_t)n;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        t->state = RS_ERROR;
        return;
    }
    t->state = RS_DONE;
}

static void rs_dispose(RegTask* t) {
    if (t->task) {
        smw_destroy_task(t->task);
        t->task = NULL;
    }
    if (t->fd >= 0) {
        flock(t->fd, LOCK_UN);
        close(t->fd);
        t->fd = -1;
    }
    free(t->rbuf);
    free(t->wbuf);
    free(t->rows);
    memset(t, 0, sizeof(*t));
    free(t);
}

void csv_registry_task_work(void* context, uint64_t mon_time) {
    (void)mon_time;
    RegTask* t = (RegTask*)context;
    switch (t->state) {
    case RS_INIT:
        t->state = RS_OPEN;
        break;
    case RS_OPEN:
        rs_open(t);
        break;
    case RS_LOCK:
        rs_lock(t);
        break;
    case RS_READ:
        rs_read(t);
        break;
    case RS_PROCESS:
        rs_process(t);
        break;
    case RS_SEEK:
        rs_seek(t);
        break;
    case RS_WRITE:
        rs_write(t);
        break;
    case RS_DONE:
        t->on_done(t->cb_ctx, t->result);
        t->state = RS_DISPOSE;
        break;
    case RS_ERROR:
        t->on_done(t->cb_ctx, CSV_REG_LIMIT_REACHED);
        t->state = RS_DISPOSE;
        break;
    case RS_DISPOSE:
        rs_dispose(t);
        break;
    }
}

int csv_registry_upsert(const char* csv_path, int max_rows, const char* key,
                        const char* tag, double f1, double f2, void* context,
                        CsvRegOnDone on_done) {
    if (!csv_path || max_rows <= 0 || !key || !tag || !on_done) {
        return -1;
    }

    RegTask* t = calloc(1, sizeof(*t));
    if (!t) {
        return -1;
    }

    t->rows = calloc((size_t)(max_rows + 1), sizeof(CsvRow));
    if (!t->rows) {
        free(t);
        return -1;
    }

    strncpy(t->csv_path, csv_path, sizeof(t->csv_path) - 1);
    strncpy(t->key, key, sizeof(t->key) - 1);
    strncpy(t->tag, tag, sizeof(t->tag) - 1);
    t->f1       = f1;
    t->f2       = f2;
    t->max_rows = max_rows;
    t->fd       = -1;
    t->cb_ctx   = context;
    t->on_done  = on_done;
    t->state    = RS_INIT;

    t->task = smw_create_task(t, csv_registry_task_work);
    if (!t->task) {
        free(t->rows);
        free(t);
        return -1;
    }
    return 0;
}

int csv_registry_load(const char* csv_path, int max_rows, CsvRow** out,
                      int* out_count) {
    *out       = NULL;
    *out_count = 0;
    if (!csv_path || max_rows <= 0) {
        return -1;
    }

    FILE* fp = fopen(csv_path, "r");
    if (!fp) {
        return (errno == ENOENT) ? 0 : -1;
    }

    if (flock(fileno(fp), LOCK_SH) != 0) {
        fclose(fp);
        return -1;
    }

    CsvRow* rows = calloc((size_t)max_rows, sizeof(CsvRow));
    if (!rows) {
        flock(fileno(fp), LOCK_UN);
        fclose(fp);
        return -1;
    }

    time_t now   = time(NULL);
    int    count = 0;
    char   line[512];

    while (fgets(line, sizeof(line), fp) && count < max_rows) {
        char* sp  = NULL;
        char* tok = strtok_r(line, ",", &sp);
        if (!tok || tok[0] == '\0') {
            continue;
        }

        CsvRow r = {0};
        strncpy(r.key, tok, sizeof(r.key) - 1);

        tok = strtok_r(NULL, ",", &sp);
        strncpy(r.tag, tok ? tok : "", sizeof(r.tag) - 1);

        tok             = strtok_r(NULL, ",", &sp);
        r.f1            = tok ? atof(tok) : 0.0;
        tok             = strtok_r(NULL, ",", &sp);
        r.f2            = tok ? atof(tok) : 0.0;
        tok             = strtok_r(NULL, ",", &sp);
        r.last_accessed = tok ? (time_t)atoll(tok) : now;

        rows[count++] = r;
    }

    flock(fileno(fp), LOCK_UN);
    fclose(fp);
    *out       = rows;
    *out_count = count;
    return 0;
}
