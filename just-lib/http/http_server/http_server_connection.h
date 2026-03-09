/**
 * @file http_server_connection.h
 * @brief HTTP/1.x server connection handler with asynchronous request
 * processing
 *
 * Implements a state-machine-based HTTP server connection that processes
 * requests asynchronously via smw task callbacks. Each connection progresses
 * through the following states:
 *
 *   INIT -> RECEIVE_HEADERS -> RECEIVE_BODY -> WAIT_RESPONSE -> SEND -> DISPOSE
 *
 * RECEIVE_BODY is skipped when Content-Length is 0.
 *
 * The application is notified via the OnRequest callback once a complete
 * request has been received. It must call http_server_connection_respond()
 * from within or after that callback to send a response. The connection is
 * automatically disposed after the response is fully sent, or on timeout/error.
 *
 * Timeouts:
 *   - Hard lifetime:  TIMEOUT_MS     from connection start
 *   - Idle:           IDLE_TIMEOUT_MS since last TCP read/write activity
 *   Both are enforced in the task loop and trigger immediate disposal.
 */

#ifndef HTTP_SERVER_CONNECTION_H
#define HTTP_SERVER_CONNECTION_H

#include "smw.h"
#include "tcp_client.h"

#include <stddef.h>
#include <stdint.h>

/** Maximum bytes read from the TCP socket per task tick */
#define CHUNK_SIZE 256

/** Maximum length of the HTTP method string (e.g. "OPTIONS\0") */
#define METHOD_MAX_LEN 9

/** Maximum length of the HTTP request path/URI */
#define REQUEST_PATH_MAX_LEN 256

/** Maximum length of the Host header value */
#define HOST_MAX_LEN 256

/**
 * Hard connection lifetime limit in milliseconds.
 * The connection is disposed if this elapses from the moment it was initiated,
 * regardless of activity.
 */
#define TIMEOUT_MS 10000

/**
 * Idle timeout in milliseconds.
 * The connection is disposed if no TCP read or write activity occurs within
 * this window. This catches stalled clients and slow application handlers
 * stuck in WAIT_RESPONSE.
 */
#define IDLE_TIMEOUT_MS 5000

/**
 * @brief Callback invoked when a complete HTTP request has been received.
 *
 * The application must call http_server_connection_respond() before or after
 * this callback returns to provide a response. If no response is set within
 * IDLE_TIMEOUT_MS of inactivity, the connection will be disposed.
 *
 * @param context  User-provided pointer supplied to
 * http_server_connection_set_callback()
 */
typedef int (*HttpServerConnectionOnRequest)(void* context);

/**
 * @brief State machine states for an HTTP server connection.
 */
typedef enum {
    HTTP_SERVER_CONNECTION_STATE_INIT,
    HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS,
    HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY,
    HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE,
    HTTP_SERVER_CONNECTION_STATE_SEND,
    HTTP_SERVER_CONNECTION_STATE_DISPOSE,
} HttpServerConnectionState;

/**
 * @brief HTTP server connection context.
 *
 * All fields are managed internally. Callers should treat this as opaque
 * and interact only via the public API functions.
 */
typedef struct {
    TCPClient                     tcpClient;
    SmwTask*                      task;
    HttpServerConnectionState     state;
    void*                         context;
    HttpServerConnectionOnRequest onRequest;

    uint64_t start_time;
    uint64_t last_activity_time;

    /** Parsed HTTP method — heap allocated, NULL until headers are parsed */
    char* method;

    /** Parsed request path/URI — heap allocated, NULL until headers are parsed
     */
    char* request_path;

    /** Parsed Host header value — heap allocated, NULL until headers are parsed
     */
    char* host;

    /** Content-Length value from headers; 0 if no body */
    size_t content_len;

    /** Accumulation buffer for incoming TCP data (headers + body) */
    uint8_t* read_buffer;
    size_t   read_buffer_size;

    /** Offset into read_buffer where the body starts (after the \r\n\r\n) */
    size_t body_start;

    /** Extracted request body — heap allocated, NULL if no body */
    uint8_t* body;

    /** Response buffer populated by http_server_connection_respond() */
    uint8_t* write_buffer;
    size_t   write_size;
    size_t   write_offset;
} HTTPServerConnection;

/**
 * @brief Initialise a connection from an accepted socket file descriptor.
 *
 * The connection begins processing immediately on the next smw tick.
 *
 * @param connection  Caller-allocated connection struct to initialise.
 * @param fd          Accepted TCP socket file descriptor.
 * @return 0 on success, negative on error.
 */
int http_server_connection_initiate(HTTPServerConnection* connection, int fd);

/**
 * @brief Allocate and initialise a connection from an accepted socket.
 *
 * Equivalent to malloc + http_server_connection_initiate(). On success,
 * *connection_ptr is set to the allocated connection. The caller is
 * responsible for eventual disposal via http_server_connection_dispose_ptr().
 *
 * @param fd              Accepted TCP socket file descriptor.
 * @param connection_ptr  Output parameter for the allocated connection.
 * @return 0 on success, negative on error.
 */
int http_server_connection_initiate_ptr(int                    fd,
                                        HTTPServerConnection** connection_ptr);

/**
 * @brief Register the request callback and user context.
 *
 * Must be called before the first smw tick processes this connection.
 *
 * @param connection  Target connection.
 * @param context     Opaque pointer passed to on_request.
 * @param on_request  Callback invoked when a full request is ready.
 */
void http_server_connection_set_callback(
    HTTPServerConnection* connection, void* context,
    HttpServerConnectionOnRequest on_request);

/**
 * @brief Provide a response buffer to send to the client.
 *
 * Must be called from within or after the OnRequest callback, and only
 * while the connection is in the WAIT_RESPONSE state. The data is copied
 * internally; the caller may free its buffer immediately after this returns.
 *
 * @param connection  Target connection.
 * @param data        Response bytes (e.g. a full HTTP response).
 * @param len         Number of bytes in data.
 * @return 0 on success, negative on error.
 */
int http_server_connection_respond(HTTPServerConnection* connection,
                                   const void* data, size_t len);

/**
 * @brief Dispose of connection resources and remove its smw task.
 *
 * Safe to call on a zero-initialised or already-disposed connection.
 * After this returns the struct is zeroed and must not be used again
 * unless re-initialised.
 */
void http_server_connection_dispose(HTTPServerConnection* connection);

/**
 * @brief Dispose of the connection and free the heap-allocated struct.
 *
 * Sets *connection_ptr to NULL on return.
 */
void http_server_connection_dispose_ptr(HTTPServerConnection** connection_ptr);

#endif // HTTP_SERVER_CONNECTION_H
