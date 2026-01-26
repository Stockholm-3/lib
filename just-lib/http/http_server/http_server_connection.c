#include "http_server_connection.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HEADER_SIZE 8192
#define MAX_BODY_SIZE (10 * 1024 * 1024) // 10MB

// Drop if no read/write activity for this long
#define IDLE_TIMEOUT_MS 5000

//-----------------Internal Functions-----------------

void http_server_connection_task_work(void* context, uint64_t mon_time);

static int  http_server_connection_send(HTTPServerConnection* connection,
                                        uint64_t              mon_time);
static int  read_chunk_into_buffer(HTTPServerConnection* connection,
                                   uint64_t              mon_time);
static int  parse_http_headers(HTTPServerConnection* connection);
static int  validate_http_method(const char* method);
static int  validate_request_path(const char* path);
static int  extract_request_body(HTTPServerConnection* connection);
static void transition_to_wait_response_state(HTTPServerConnection* connection);

//----------------------------------------------------

int http_server_connection_initiate(HTTPServerConnection* connection, int fd) {
    tcp_client_initiate(&connection->tcpClient, fd);

    connection->read_buffer        = NULL;
    connection->method             = NULL;
    connection->request_path       = NULL;
    connection->host               = NULL;
    connection->write_buffer       = NULL;
    connection->body               = NULL;
    connection->read_buffer_size   = 0;
    connection->content_len        = 0;
    connection->write_size         = 0;
    connection->write_offset       = 0;
    connection->body_start         = 0;
    connection->start_time         = 0;
    connection->last_activity_time = 0;
    connection->state              = HTTP_SERVER_CONNECTION_STATE_INIT;

    connection->task =
        smw_create_task(connection, http_server_connection_task_work);

    return 0;
}

int http_server_connection_initiate_ptr(int                    fd,
                                        HTTPServerConnection** connection_ptr) {
    if (!connection_ptr) {
        return -1;
    }

    HTTPServerConnection* connection =
        (HTTPServerConnection*)malloc(sizeof(HTTPServerConnection));
    if (!connection) {
        return -2;
    }

    int result = http_server_connection_initiate(connection, fd);
    if (result != 0) {
        free(connection);
        return result;
    }

    *connection_ptr = connection;
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
    if (connection->write_buffer != NULL) {
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

    return 0;
}

//----------------- I/O Helpers -----------------

static int read_chunk_into_buffer(HTTPServerConnection* connection,
                                  uint64_t              mon_time) {
    uint8_t chunk_buffer[CHUNK_SIZE];

    int bytes_read = tcp_client_read(&connection->tcpClient, chunk_buffer,
                                     sizeof(chunk_buffer));

    if (bytes_read < 0) {
        return -1;
    } else if (bytes_read == 0) {
        return 0;
    }

    // Activity!
    connection->last_activity_time = mon_time;

    size_t new_size = connection->read_buffer_size + bytes_read;

    size_t max_size = (connection->body_start > 0)
                          ? (connection->body_start + MAX_BODY_SIZE)
                          : MAX_HEADER_SIZE;

    if (new_size > max_size) {
        return -2;
    }

    uint8_t* new_buffer = realloc(connection->read_buffer, new_size);
    if (!new_buffer) {
        return -1;
    }

    connection->read_buffer = new_buffer;
    memcpy(connection->read_buffer + connection->read_buffer_size, chunk_buffer,
           bytes_read);
    connection->read_buffer_size += bytes_read;

    return bytes_read;
}

static int http_server_connection_send(HTTPServerConnection* connection,
                                       uint64_t              mon_time) {
    if (!connection || !connection->write_buffer ||
        connection->write_offset >= connection->write_size) {
        return 0;
    }

    ssize_t sent =
        tcp_client_write(&connection->tcpClient,
                         connection->write_buffer + connection->write_offset,
                         connection->write_size - connection->write_offset);

    if (sent > 0) {
        connection->write_offset += sent;
        connection->last_activity_time = mon_time; // Activity!
    } else if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        }
    }

    if (connection->write_offset >= connection->write_size) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
    }

    return 0;
}

//----------------- Parsing -----------------

static int validate_http_method(const char* method) {
    if (!method || method[0] == '\0') {
        return 0;
    }

    const char* valid_methods[] = {"GET",    "POST",    "PUT",
                                   "DELETE", "HEAD",    "OPTIONS",
                                   "PATCH",  "CONNECT", "TRACE"};

    for (size_t i = 0; i < sizeof(valid_methods) / sizeof(valid_methods[0]);
         i++) {
        if (strcmp(method, valid_methods[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static int validate_request_path(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    return (path[0] == '/' || strcmp(path, "*") == 0);
}

static int parse_http_headers(HTTPServerConnection* connection) {
    for (size_t i = 0; i + 3 < connection->read_buffer_size; i++) {
        if (connection->read_buffer[i] == '\r' &&
            connection->read_buffer[i + 1] == '\n' &&
            connection->read_buffer[i + 2] == '\r' &&
            connection->read_buffer[i + 3] == '\n') {

            char   method[METHOD_MAX_LEN]             = {0};
            char   request_path[REQUEST_PATH_MAX_LEN] = {0};
            char   version[16]                        = {0};
            char   host[HOST_MAX_LEN]                 = {0};
            size_t content_len                        = 0;

            int   header_end = i + 4;
            char* headers    = malloc(header_end + 1);
            if (!headers) {
                return -1;
            }

            memcpy(headers, connection->read_buffer, header_end);
            headers[header_end] = '\0';

            int parsed = sscanf(headers, "%7s %255s %15s", method, request_path,
                                version);

            if (parsed < 2 || !validate_http_method(method) ||
                !validate_request_path(request_path)) {
                free(headers);
                return -1;
            }

            if (parsed >= 3 && strncmp(version, "HTTP/", 5) != 0) {
                free(headers);
                return -1;
            }

            char* host_ptr = strstr(headers, "Host:");
            if (host_ptr) {
                sscanf(host_ptr, "Host: %255s", host);
            }

            char* content_len_ptr = strstr(headers, "Content-Length:");
            if (content_len_ptr) {
                if (sscanf(content_len_ptr, "Content-Length: %zu",
                           &content_len) != 1 ||
                    content_len > MAX_BODY_SIZE) {
                    free(headers);
                    return -1;
                }
            }

            free(headers);

            connection->method       = strdup(method);
            connection->request_path = strdup(request_path);
            connection->host         = strdup(host);
            connection->content_len  = content_len;
            connection->body_start   = header_end;

            if (!connection->method || !connection->request_path ||
                !connection->host) {
                return -1;
            }

            return 1;
        }
    }

    return 0;
}

static int extract_request_body(HTTPServerConnection* connection) {
    if (connection->content_len == 0) {
        return 0;
    }

    connection->body = malloc(connection->content_len);
    if (!connection->body) {
        return -1;
    }

    memcpy(connection->body, connection->read_buffer + connection->body_start,
           connection->content_len);

    return 0;
}

static void
transition_to_wait_response_state(HTTPServerConnection* connection) {
    connection->state = HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE;
    connection->onRequest(connection->context);
}

//----------------- State Handlers -----------------

int http_server_connection_receive_headers(HTTPServerConnection* connection,
                                           uint64_t              mon_time) {
    int bytes_read = read_chunk_into_buffer(connection, mon_time);
    if (bytes_read < 0) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    } else if (bytes_read == 0) {
        return 0;
    }

    int parse_result = parse_http_headers(connection);
    if (parse_result < 0) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    } else if (parse_result == 0) {
        return 0;
    }

    if (connection->content_len == 0) {
        transition_to_wait_response_state(connection);
    } else {
        connection->state = HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY;
    }

    return 0;
}

int http_server_connection_receive_body(HTTPServerConnection* connection,
                                        uint64_t              mon_time) {
    if (connection->read_buffer_size <
        connection->body_start + connection->content_len) {

        int bytes_read = read_chunk_into_buffer(connection, mon_time);
        if (bytes_read < 0) {
            connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        } else if (bytes_read == 0) {
            return 0;
        }
    }

    if (connection->read_buffer_size >=
        connection->body_start + connection->content_len) {

        if (extract_request_body(connection) < 0) {
            connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        }

        transition_to_wait_response_state(connection);
    }

    return 0;
}

//----------------- Task Loop -----------------

void http_server_connection_task_work(void* context, uint64_t mon_time) {
    HTTPServerConnection* connection = (HTTPServerConnection*)context;

    if (connection->state == HTTP_SERVER_CONNECTION_STATE_INIT) {
        connection->start_time         = mon_time;
        connection->last_activity_time = mon_time;
        connection->state = HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS;
        return;
    }

    // Hard lifetime timeout
    if (mon_time - connection->start_time >= TIMEOUT_MS) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return;
    }

    // Idle timeout (no read/write)
    if (mon_time - connection->last_activity_time >= IDLE_TIMEOUT_MS) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return;
    }

    switch (connection->state) {
    case HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS:
        http_server_connection_receive_headers(connection, mon_time);
        break;
    case HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY:
        http_server_connection_receive_body(connection, mon_time);
        break;
    case HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE:
        break;
    case HTTP_SERVER_CONNECTION_STATE_SEND:
        http_server_connection_send(connection, mon_time);
        break;
    case HTTP_SERVER_CONNECTION_STATE_DISPOSE:
        http_server_connection_dispose(connection);
        break;
    }
}

//----------------- Cleanup -----------------

void http_server_connection_dispose(HTTPServerConnection* connection) {
    if (!connection) {
        return;
    }

    if (connection->task) {
        smw_destroy_task(connection->task);
        connection->task = NULL;
    }

    tcp_client_dispose(&connection->tcpClient);

    free(connection->read_buffer);
    free(connection->body);
    free(connection->method);
    free(connection->request_path);
    free(connection->host);
    free(connection->write_buffer);

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
