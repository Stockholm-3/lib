#include "http_server_connection.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum size for header buffer to prevent memory exhaustion attacks
#define MAX_HEADER_SIZE 8192

// Maximum size for request body to prevent memory exhaustion attacks
#define MAX_BODY_SIZE (10 * 1024 * 1024) // 10MB

//-----------------Internal Functions-----------------

void http_server_connection_task_work(void* context, uint64_t mon_time);

static int  http_server_connection_send(HTTPServerConnection* connection);
static int  read_chunk_into_buffer(HTTPServerConnection* connection);
static int  parse_http_headers(HTTPServerConnection* connection);
static int  validate_http_method(const char* method);
static int  validate_request_path(const char* path);
static int  extract_request_body(HTTPServerConnection* connection);
static void transition_to_send_state(HTTPServerConnection* connection);

//----------------------------------------------------

int http_server_connection_initiate(HTTPServerConnection* connection, int fd) {
    tcp_client_initiate(&connection->tcpClient, fd);
    connection->read_buffer      = NULL;
    connection->method           = NULL;
    connection->request_path     = NULL;
    connection->host             = NULL;
    connection->write_buffer     = NULL;
    connection->body             = NULL;
    connection->read_buffer_size = 0;
    connection->content_len      = 0;
    connection->write_size       = 0;
    connection->write_offset     = 0;
    connection->body_start       = 0;
    connection->start_time       = 0;
    connection->state            = HTTP_SERVER_CONNECTION_STATE_INIT;

    connection->task =
        smw_create_task(connection, http_server_connection_task_work);

    return 0;
}

int http_server_connection_initiate_ptr(int                    fd,
                                        HTTPServerConnection** connection_ptr) {
    if (connection_ptr == NULL) {
        return -1;
    }

    HTTPServerConnection* connection =
        (HTTPServerConnection*)malloc(sizeof(HTTPServerConnection));
    if (connection == NULL) {
        return -2;
    }

    int result = http_server_connection_initiate(connection, fd);
    if (result != 0) {
        free(connection);
        return result;
    }

    *(connection_ptr) = connection;

    return 0;
}

void http_server_connection_set_callback(
    HTTPServerConnection* connection, void* context,
    HttpServerConnectionOnRequest on_request) {
    connection->context   = context;
    connection->onRequest = on_request;
}

/**
 * @brief Read a chunk of data from TCP socket into the connection's read buffer
 *
 * Reads up to CHUNK_SIZE bytes from the TCP socket and appends it to the
 * connection's internal read buffer. The buffer is automatically expanded
 * using realloc if needed. Enforces MAX_HEADER_SIZE limit to prevent memory
 * exhaustion attacks.
 *
 * @param connection Pointer to the HTTP server connection
 * @return int Number of bytes read on success, 0 if no data available
 *         (EAGAIN/EWOULDBLOCK), -1 on error, -2 if size limit exceeded
 */
static int read_chunk_into_buffer(HTTPServerConnection* connection) {
    uint8_t chunk_buffer[CHUNK_SIZE];

    int bytes_read = tcp_client_read(&connection->tcpClient, chunk_buffer,
                                     sizeof(chunk_buffer));

    if (bytes_read < 0) {
        return -1; // Real error
    } else if (bytes_read == 0) {
        return 0; // No data available (EAGAIN/EWOULDBLOCK)
    }

    size_t new_size = connection->read_buffer_size + bytes_read;

    // Check size limits based on current state
    size_t max_size = (connection->body_start > 0)
                          ? (connection->body_start + MAX_BODY_SIZE)
                          : MAX_HEADER_SIZE;

    if (new_size > max_size) {
        return -2; // Size limit exceeded
    }

    // Expand the read buffer
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

/**
 * @brief Validate HTTP method string
 *
 * Checks if the method is a recognized HTTP method and contains only
 * uppercase letters.
 *
 * @param method HTTP method string to validate
 * @return int 1 if valid, 0 if invalid
 */
static int validate_http_method(const char* method) {
    if (!method || method[0] == '\0') {
        return 0;
    }

    // Common HTTP methods
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

/**
 * @brief Validate HTTP request path
 *
 * Checks if the path is non-empty and starts with '/' or is '*' (for OPTIONS).
 *
 * @param path Request path to validate
 * @return int 1 if valid, 0 if invalid
 */
static int validate_request_path(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    // Path must start with '/' or be '*'
    if (path[0] == '/' || strcmp(path, "*") == 0) {
        return 1;
    }

    return 0;
}

/**
 * @brief Parse HTTP headers from the connection's read buffer
 *
 * Searches for the HTTP headers end marker (\\r\\n\\r\\n) and extracts the
 * request line and headers. Parses and validates:
 * - HTTP method (GET, POST, etc.)
 * - Request path/URI
 * - Host header
 * - Content-Length header
 *
 * Parsed values are stored in the connection structure as dynamically
 * allocated strings that must be freed later.
 *
 * @param connection Pointer to the HTTP server connection
 * @return int 1 if headers fully parsed and valid, 0 if incomplete (need more
 *         data), -1 on error (memory allocation failure or malformed request)
 */
static int parse_http_headers(HTTPServerConnection* connection) {
    // Search for end of headers marker: \r\n\r\n
    for (size_t i = 0; i <= connection->read_buffer_size - 4; i++) {
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

            // Parse request line (method, path, and version)
            int parsed = sscanf(headers, "%7s %255s %15s", method, request_path,
                                version);

            // Must have at least method and path
            if (parsed < 2) {
                free(headers);
                return -1; // Malformed request line
            }

            // Validate method
            if (!validate_http_method(method)) {
                free(headers);
                return -1; // Invalid HTTP method
            }

            // Validate request path
            if (!validate_request_path(request_path)) {
                free(headers);
                return -1; // Invalid request path
            }

            // Validate HTTP version if present
            if (parsed >= 3) {
                if (strncmp(version, "HTTP/", 5) != 0) {
                    free(headers);
                    return -1; // Invalid HTTP version
                }
            }

            // Parse Host header
            char* host_ptr = strstr(headers, "Host:");
            if (host_ptr) {
                sscanf(host_ptr, "Host: %255s", host);
            }

            // Parse Content-Length header
            char* content_len_ptr = strstr(headers, "Content-Length:");
            if (content_len_ptr) {
                int scanned = sscanf(content_len_ptr, "Content-Length: %zu",
                                     &content_len);
                if (scanned != 1) {
                    free(headers);
                    return -1; // Invalid Content-Length
                }

                // Validate Content-Length
                if (content_len > MAX_BODY_SIZE) {
                    free(headers);
                    return -1; // Body too large
                }
            }

            free(headers);

            // Store parsed values
            connection->method       = strdup(method);
            connection->request_path = strdup(request_path);
            connection->host         = strdup(host);
            connection->content_len  = content_len;
            connection->body_start   = header_end;

            // Check allocation success
            if (!connection->method || !connection->request_path ||
                !connection->host) {
                return -1;
            }

            return 1; // Headers fully parsed and validated
        }
    }

    return 0; // Headers incomplete
}

/**
 * @brief Extract the request body from the read buffer
 *
 * Copies the HTTP request body (if present) from the connection's read buffer
 * into a separate body buffer. The body starts at body_start offset and has
 * length content_len. If content_len is 0, no allocation is performed.
 *
 * @param connection Pointer to the HTTP server connection
 * @return int 0 on success, -1 on memory allocation error
 */
static int extract_request_body(HTTPServerConnection* connection) {
    if (connection->content_len == 0) {
        return 0; // No body to extract
    }

    connection->body = malloc(connection->content_len);
    if (!connection->body) {
        return -1;
    }

    memcpy(connection->body, connection->read_buffer + connection->body_start,
           connection->content_len);

    return 0;
}

/**
 * @brief Transition connection to SEND state and invoke request callback
 *
 * Changes the connection state to HTTP_SERVER_CONNECTION_STATE_SEND and
 * invokes the user-provided onRequest callback to allow the application
 * to generate a response.
 *
 * @param connection Pointer to the HTTP server connection
 */
static void transition_to_send_state(HTTPServerConnection* connection) {
    connection->state = HTTP_SERVER_CONNECTION_STATE_SEND;
    connection->onRequest(connection->context);
}

//-------- State Handlers --------

/**
 * @brief State handler for receiving HTTP request headers
 *
 * Reads data from the TCP socket and attempts to parse HTTP headers. Once
 * headers are complete, transitions to either RECEIVE_BODY (if Content-Length
 * present) or SEND (if no body expected). If headers are malformed or size
 * limits are exceeded, transitions to DISPOSE.
 *
 * This function is called repeatedly by the task work function until headers
 * are fully received and parsed.
 *
 * @param connection Pointer to the HTTP server connection
 * @return int 0 on success or when waiting for more data, -1 on error
 */
int http_server_connection_receive_headers(HTTPServerConnection* connection) {
    if (!connection) {
        return -1;
    }

    // Read incoming data
    int bytes_read = read_chunk_into_buffer(connection);
    if (bytes_read < 0) {
        // Error or size limit exceeded - dispose connection
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    } else if (bytes_read == 0) {
        return 0; // No data yet, continue waiting
    }

    // Try to parse headers
    int parse_result = parse_http_headers(connection);
    if (parse_result < 0) {
        // Malformed request - dispose connection
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return -1;
    } else if (parse_result == 0) {
        return 0; // Headers incomplete, need more data
    }

    // Headers complete and valid! Decide next state
    if (connection->content_len == 0) {
        // No body expected (GET, HEAD, etc.)
        transition_to_send_state(connection);
    } else {
        // Body expected, transition to body reading state
        connection->state = HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY;
    }

    return 0;
}

/**
 * @brief State handler for receiving HTTP request body
 *
 * Continues reading data from the TCP socket until the entire request body
 * (as specified by Content-Length header) has been received. Once complete,
 * extracts the body into a separate buffer and transitions to SEND state.
 * If size limits are exceeded, transitions to DISPOSE.
 *
 * This function is called repeatedly by the task work function until the
 * body is fully received.
 *
 * @param connection Pointer to the HTTP server connection
 * @return int 0 on success or when waiting for more data, -1 on error
 */
int http_server_connection_receive_body(HTTPServerConnection* connection) {
    if (!connection) {
        return -1;
    }

    // Check if we already have all the body data
    if (connection->read_buffer_size <
        connection->body_start + connection->content_len) {
        // Need more data
        int bytes_read = read_chunk_into_buffer(connection);
        if (bytes_read < 0) {
            // Error or size limit exceeded - dispose connection
            connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        } else if (bytes_read == 0) {
            return 0; // No data yet, continue waiting
        }
    }

    // Check again if body is complete
    if (connection->read_buffer_size >=
        connection->body_start + connection->content_len) {
        // Extract the body
        if (extract_request_body(connection) < 0) {
            connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        }

        // Transition to SEND state
        transition_to_send_state(connection);
    }

    return 0;
}

static int http_server_connection_send(HTTPServerConnection* connection) {
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
    } else if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
            return -1;
        }
    }

    // Finished sending
    if (connection->write_offset >= connection->write_size) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
    }

    return 0;
}

/**
 * @brief Main task work function for HTTP server connection state machine
 *
 * This function is called periodically by the task scheduler to process the
 * connection's current state. It handles:
 * - INIT: Sets start time and transitions to RECEIVE_HEADERS
 * - Timeout checking: Disposes connection if TIMEOUT_MS elapsed
 * - State processing: Delegates to appropriate handler function
 *
 * @param context Pointer to HTTPServerConnection (passed as void*)
 * @param mon_time Current monotonic time in milliseconds
 */
void http_server_connection_task_work(void* context, uint64_t mon_time) {
    HTTPServerConnection* connection = (HTTPServerConnection*)context;

    // Handle initialization
    if (connection->state == HTTP_SERVER_CONNECTION_STATE_INIT) {
        connection->start_time = mon_time;
        connection->state      = HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS;
        return;
    }

    // Check for timeout
    if (mon_time - connection->start_time >= TIMEOUT_MS) {
        connection->state = HTTP_SERVER_CONNECTION_STATE_DISPOSE;
        return;
    }

    // Process current state
    switch (connection->state) {
    case HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS:
        http_server_connection_receive_headers(connection);
        break;
    case HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY:
        http_server_connection_receive_body(connection);
        break;
    case HTTP_SERVER_CONNECTION_STATE_SEND:
        http_server_connection_send(connection);
        break;
    case HTTP_SERVER_CONNECTION_STATE_DISPOSE:
        http_server_connection_dispose(connection);
        break;
    }
}

void http_server_connection_dispose(HTTPServerConnection* connection) {
    if (!connection) {
        return;
    }

    // Stop and remove the task first
    if (connection->task) {
        smw_destroy_task(connection->task);
        connection->task = NULL;
    }

    // Dispose TCP client
    tcp_client_dispose(&connection->tcpClient);

    // Free all dynamically allocated memory
    free(connection->read_buffer);
    connection->read_buffer = NULL;

    free(connection->body);
    connection->body = NULL;

    free(connection->method);
    connection->method = NULL;

    free(connection->request_path);
    connection->request_path = NULL;

    free(connection->host);
    connection->host = NULL;

    free(connection->write_buffer);
    connection->write_buffer = NULL;

    connection->read_buffer_size = 0;
    connection->write_size       = 0;
    connection->write_offset     = 0;
    connection->body_start       = 0;
    connection->content_len      = 0;
}

void http_server_connection_dispose_ptr(HTTPServerConnection** connection_ptr) {
    if (connection_ptr == NULL || *(connection_ptr) == NULL) {
        return;
    }

    http_server_connection_dispose(*(connection_ptr));
    free(*(connection_ptr));
    *(connection_ptr) = NULL;
}
