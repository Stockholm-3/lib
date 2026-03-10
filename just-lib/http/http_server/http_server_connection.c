#include "http_server_connection.h"

#include "logger.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE (10 * 1024 * 1024)

#define MODULE "http_conn"

static void task_work(void* context, uint64_t mon_time);

static int state_receive_headers(HTTPServerConnection* c, uint64_t mon_time);
static int state_receive_body(HTTPServerConnection* c, uint64_t mon_time);
static int state_send(HTTPServerConnection* c, uint64_t mon_time);

static int  read_chunk(HTTPServerConnection* c, uint64_t mon_time);
static int  parse_headers(HTTPServerConnection* c);
static int  extract_body(HTTPServerConnection* c);
static void fire_request(HTTPServerConnection* c);
static void dispose_resources(HTTPServerConnection* c);

int http_server_connection_initiate(HTTPServerConnection* connection, int fd) {
    memset(connection, 0, sizeof(*connection));
    tcp_client_initiate(&connection->tcpClient, fd);
    connection->state = HTTP_SERVER_CONNECTION_STATE_INIT;
    connection->task  = smw_create_task(connection, task_work);
    if (!connection->task) {
        tcp_client_dispose(&connection->tcpClient);
        LOG_ERROR(MODULE, "fd=%d failed to create smw task", fd);
        return -1;
    }
    LOG_DEBUG(MODULE, "fd=%d connection initiated", fd);
    return 0;
}

int http_server_connection_initiate_ptr(int                    fd,
                                        HTTPServerConnection** connection_ptr) {
    if (!connection_ptr) {
        return -1;
    }
    HTTPServerConnection* c = malloc(sizeof(HTTPServerConnection));
    if (!c) {
        return -2;
    }
    int result = http_server_connection_initiate(c, fd);
    if (result != 0) {
        free(c);
        return result;
    }
    c->heap_allocated = 1;
    *connection_ptr   = c;
    return 0;
}

void http_server_connection_set_callback(
    HTTPServerConnection* connection, void* context,
    HttpServerConnectionOnRequest on_request) {
    connection->context   = context;
    connection->onRequest = on_request;
}

int http_server_connection_respond(HTTPServerConnection* connection,
                                   const void* data, size_t len) {
    if (!connection || !data || len == 0) {
        return -1;
    }
    if (connection->state != HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE) {
        return -2;
    }
    if (connection->write_buffer) {
        return -3;
    }

    uint8_t* buf = malloc(len);
    if (!buf) {
        return -4;
    }
    memcpy(buf, data, len);

    connection->write_buffer = buf;
    connection->write_size   = len;
    connection->write_offset = 0;
    connection->state        = HTTP_SERVER_CONNECTION_STATE_SEND;

    LOG_DEBUG(MODULE, "%s %s response queued (%zu bytes)", connection->method,
              connection->request_path, len);

    return 0;
}

void http_server_connection_dispose(HTTPServerConnection* connection) {
    if (!connection || !connection->task) {
        // task == NULL means already disposed or never fully initialised
        return;
    }

    LOG_DEBUG(MODULE, "%s %s connection disposed",
              connection->method ? connection->method : "-",
              connection->request_path ? connection->request_path : "-");

    // smw_work saves the next node pointer before invoking each callback, so
    // destroying the task from within the task callback is safe — the list
    // traversal in smw_work is not disturbed.
    smw_destroy_task(connection->task);

    tcp_client_dispose(&connection->tcpClient);
    dispose_resources(connection);

    memset(connection, 0, sizeof(*connection));
}

void http_server_connection_dispose_ptr(HTTPServerConnection** connection_ptr) {
    if (!connection_ptr || !*connection_ptr) {
        return;
    }
    http_server_connection_dispose(*connection_ptr);
    free(*connection_ptr);
    *connection_ptr = NULL;
}

static int read_chunk(HTTPServerConnection* c, uint64_t mon_time) {
    uint8_t tmp[CHUNK_SIZE];
    int     n = tcp_client_read(&c->tcpClient, tmp, sizeof(tmp));

    if (n < 0) {
        LOG_WARN(MODULE, "tcp read error: %s", strerror(errno));
        return -1;
    }
    if (n == 0) {
        // Peer closed the connection (EOF)
        LOG_DEBUG(MODULE, "peer closed connection (EOF)");
        return -1;
    }

    c->last_activity_time = mon_time;

    size_t max_size =
        (c->body_start > 0) ? (c->body_start + MAX_BODY_SIZE) : MAX_HEADER_SIZE;

    size_t new_size = c->read_buffer_size + (size_t)n;
    if (new_size > max_size) {
        LOG_WARN(MODULE, "read buffer limit exceeded (%zu > %zu)", new_size,
                 max_size);
        return -2;
    }

    uint8_t* buf = realloc(c->read_buffer, new_size);
    if (!buf) {
        return -1;
    }

    memcpy(buf + c->read_buffer_size, tmp, n);
    c->read_buffer      = buf;
    c->read_buffer_size = new_size;

    return n;
}

static int validate_method(const char* method) {
    static const char* valid[] = {"GET",    "POST",    "PUT",
                                  "DELETE", "HEAD",    "OPTIONS",
                                  "PATCH",  "CONNECT", "TRACE"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(method, valid[i]) == 0)
            return 1;
    }
    return 0;
}

static int validate_path(const char* path) {
    return path && (path[0] == '/' || strcmp(path, "*") == 0);
}

static int parse_headers(HTTPServerConnection* c) {
    // Scan for the end-of-headers marker \r\n\r\n
    for (size_t i = 0; i + 3 < c->read_buffer_size; i++) {
        if (c->read_buffer[i] != '\r' || c->read_buffer[i + 1] != '\n' ||
            c->read_buffer[i + 2] != '\r' || c->read_buffer[i + 3] != '\n') {
            continue;
        }

        size_t header_end = i + 4;

        char* raw = malloc(header_end + 1);
        if (!raw) {
            return -1;
        }
        memcpy(raw, c->read_buffer, header_end);
        raw[header_end] = '\0';

        char   method[METHOD_MAX_LEN]     = {0};
        char   path[REQUEST_PATH_MAX_LEN] = {0};
        char   version[16]                = {0};
        char   host[HOST_MAX_LEN]         = {0};
        size_t content_len                = 0;

        int parsed = sscanf(raw, "%7s %255s %15s", method, path, version);

        if (parsed < 2 || !validate_method(method) || !validate_path(path)) {
            LOG_WARN(MODULE,
                     "malformed request line (parsed=%d method='%s' path='%s')",
                     parsed, method, path);
            free(raw);
            return -1;
        }

        if (parsed >= 3 && strncmp(version, "HTTP/", 5) != 0) {
            LOG_WARN(MODULE, "unrecognised HTTP version '%s'", version);
            free(raw);
            return -1;
        }

        char* host_hdr = strstr(raw, "Host:");
        if (host_hdr) {
            sscanf(host_hdr, "Host: %255s", host);
        }

        char* cl_hdr = strstr(raw, "Content-Length:");
        if (cl_hdr) {
            if (sscanf(cl_hdr, "Content-Length: %zu", &content_len) != 1 ||
                content_len > MAX_BODY_SIZE) {
                LOG_WARN(MODULE, "invalid or oversized Content-Length");
                free(raw);
                return -1;
            }
        }

        free(raw);

        // All three must succeed — duplicate all before committing any,
        // to avoid a partial-failure leak
        char* m = strdup(method);
        char* p = strdup(path);
        char* h = strdup(host);

        if (!m || !p || !h) {
            free(m);
            free(p);
            free(h);
            return -1;
        }

        c->method       = m;
        c->request_path = p;
        c->host         = h;
        c->content_len  = content_len;
        c->body_start   = header_end;

        LOG_DEBUG(MODULE, "%s %s host='%s' content-length=%zu", method, path,
                  host, content_len);

        return 1;
    }

    return 0; // headers not yet complete
}

static int extract_body(HTTPServerConnection* c) {
    if (c->content_len == 0) {
        return 0;
    }
    uint8_t* buf = malloc(c->content_len);
    if (!buf) {
        return -1;
    }
    memcpy(buf, c->read_buffer + c->body_start, c->content_len);
    c->body = buf;
    return 0;
}

static void fire_request(HTTPServerConnection* c) {
    LOG_INFO(MODULE, "%s %s", c->method, c->request_path);
    c->state = HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE;
    c->onRequest(c->context);
}

static int state_receive_headers(HTTPServerConnection* c, uint64_t mon_time) {
    int n = read_chunk(c, mon_time);
    if (n < 0) {
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    }
    if (n == 0) {
        return 0;
    }

    int result = parse_headers(c);
    if (result < 0) {
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    }
    if (result == 0) {
        return 0; // still accumulating
    }

    if (c->content_len == 0) {
        fire_request(c);
    } else {
        c->state = HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY;
    }
    return 0;
}

static int state_receive_body(HTTPServerConnection* c, uint64_t mon_time) {
    while (c->read_buffer_size < c->body_start + c->content_len) {
        int n = read_chunk(c, mon_time);
        if (n < 0) {
            c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        }
        if (n == 0) {
            return 0; // no data available this tick; wait
        }
    }

    if (extract_body(c) < 0) {
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    }

    fire_request(c);
    return 0;
}

static int state_send(HTTPServerConnection* c, uint64_t mon_time) {
    if (!c->write_buffer || c->write_offset >= c->write_size) {
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return 0;
    }

    ssize_t sent =
        tcp_client_write(&c->tcpClient, c->write_buffer + c->write_offset,
                         c->write_size - c->write_offset);

    if (sent > 0) {
        c->write_offset += (size_t)sent;
        c->last_activity_time = mon_time;
    } else if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_WARN(MODULE, "%s %s send error: %s", c->method, c->request_path,
                     strerror(errno));
            c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        }
    }

    if (c->write_offset >= c->write_size) {
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
    }

    return 0;
}

static void task_work(void* context, uint64_t mon_time) {
    HTTPServerConnection* c = (HTTPServerConnection*)context;

    if (c->state == HTTP_SERVER_CONNECTION_STATE_INIT) {
        c->start_time         = mon_time;
        c->last_activity_time = mon_time;
        c->state              = HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS;
        return;
    }

    if (mon_time - c->start_time >= TIMEOUT_MS) {
        LOG_WARN(MODULE, "%s %s hard timeout exceeded (%llu ms)",
                 c->method ? c->method : "-",
                 c->request_path ? c->request_path : "-",
                 (unsigned long long)(mon_time - c->start_time));
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
    }

    if (c->state != HTTP_SERVER_CONNECTION_STATE_DISPOSE &&
        mon_time - c->last_activity_time >= IDLE_TIMEOUT_MS) {
        LOG_WARN(MODULE, "%s %s idle timeout exceeded (%llu ms)",
                 c->method ? c->method : "-",
                 c->request_path ? c->request_path : "-",
                 (unsigned long long)(mon_time - c->last_activity_time));
        c->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
    }

    switch (c->state) {
    case HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS:
        state_receive_headers(c, mon_time);
        break;
    case HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY:
        state_receive_body(c, mon_time);
        break;
    case HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE:
        break;
    case HTTP_SERVER_CONNECTION_STATE_SEND:
        state_send(c, mon_time);
        break;
    case HTTP_SERVER_CONNECTION_STATE_DISPOSE:
        if (c->heap_allocated) {
            http_server_connection_dispose_ptr(&c);
        } else {
            http_server_connection_dispose(c);
        }
        break;
    case HTTP_SERVER_CONNECTION_STATE_INIT:
        break;
    }
}

static void dispose_resources(HTTPServerConnection* c) {
    free(c->read_buffer);
    free(c->body);
    free(c->method);
    free(c->request_path);
    free(c->host);
    free(c->write_buffer);
}
