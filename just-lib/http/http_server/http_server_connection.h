/**
 * @file http_server_connection.h
 * @brief HTTP server connection handler with asynchronous request processing
 *
 * This module provides a state-machine-based HTTP server connection handler
 * that processes HTTP requests asynchronously. It reads HTTP headers and body,
 * parses the request, and allows the application to send responses via
 * callbacks.
 *
 * The OnRequest callback is invoked when a full request is ready.
 * The application must call http_server_connection_respond() to send data.
 */

#ifndef HTTP_SERVER_CONNECTION_H
#define HTTP_SERVER_CONNECTION_H

#include "smw.h"
#include "tcp_client.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @def CHUNK_SIZE
 * @brief Maximum number of bytes to read from TCP socket per iteration
 */
#define CHUNK_SIZE 256

/**
 * @def METHOD_MAX_LEN
 * @brief Maximum length of HTTP method string (e.g., "GET", "POST")
 */
#define METHOD_MAX_LEN 9

/**
 * @def REQUEST_PATH_MAX_LEN
 * @brief Maximum length of HTTP request path/URI
 */
#define REQUEST_PATH_MAX_LEN 256

/**
 * @def HOST_MAX_LEN
 * @brief Maximum length of Host header value
 */
#define HOST_MAX_LEN 256

/**
 * @def TIMEOUT_MS
 * @brief Hard connection lifetime timeout in milliseconds (10 seconds)
 *
 * If a connection doesn't complete within this time from start_time,
 * it will be automatically disposed.
 */
#define TIMEOUT_MS 10000

/**
 * @typedef HttpServerConnectionOnRequest
 * @brief Callback function type for handling HTTP requests
 *
 * This callback is invoked when a complete HTTP request has been received
 * and parsed. The application should use this callback to generate a response
 * by calling http_server_connection_respond().
 *
 * @param context User-provided context pointer set via
 *                http_server_connection_set_callback()
 */
typedef int (*HttpServerConnectionOnRequest)(void* context);

/**
 * @enum HttpServerConnectionState
 * @brief State machine states for HTTP connection processing
 *
 * INIT -> RECEIVE_HEADERS -> RECEIVE_BODY -> WAIT_RESPONSE -> SEND -> DISPOSE
 */
typedef enum {
    /** Initial state, sets start time and transitions to RECEIVE_HEADERS */
    HTTP_SERVER_CONNECTION_STATE_INIT,

    /** Reading and parsing HTTP request headers */
    HTTP_SERVER_CONNECTION_STATE_RECEIVE_HEADERS,

    /** Reading HTTP request body (if Content-Length > 0) */
    HTTP_SERVER_CONNECTION_STATE_RECEIVE_BODY,

    /** Waiting for user to call http_server_connection_respond() */
    HTTP_SERVER_CONNECTION_STATE_WAIT_RESPONSE,

    /** Sending HTTP response to client */
    HTTP_SERVER_CONNECTION_STATE_SEND,

    /** Final state, cleanup and disposal of connection resources */
    HTTP_SERVER_CONNECTION_STATE_DISPOSE,
} HttpServerConnectionState;

/**
 * @struct HTTPServerConnection
 * @brief HTTP server connection context and state
 *
 * Contains all state needed to handle an HTTP connection including the
 * TCP socket, parsed request data, response buffers, and state machine info.
 */
typedef struct {
    /** Underlying TCP client connection */
    TCPClient tcpClient;

    /** Task handle for asynchronous processing */
    SmwTask* task;

    /** Current state in the connection state machine */
    HttpServerConnectionState state;

    /** User-provided context pointer passed to callbacks */
    void* context;

    /** Callback function invoked when request is complete */
    HttpServerConnectionOnRequest onRequest;

    /** Start time (monotonic ms) for hard lifetime timeout */
    uint64_t start_time;

    /** Last activity time (monotonic ms) for idle timeout */
    uint64_t last_activity_time;

    /** HTTP method string (e.g., "GET", "POST") - dynamically allocated */
    char* method;

    /** HTTP request path/URI - dynamically allocated */
    char* request_path;

    /** Host header value - dynamically allocated */
    char* host;

    /** Content-Length header value, 0 if no body */
    size_t content_len;

    /** Buffer holding all received data (headers + body) */
    uint8_t* read_buffer;

    /** Current size of read_buffer in bytes */
    size_t read_buffer_size;

    /** Extracted request body - dynamically allocated */
    uint8_t* body;

    /** Byte offset in read_buffer where body starts (after \r\n\r\n) */
    size_t body_start;

    /** Response buffer to send to client */
    uint8_t* write_buffer;

    /** Total size of write_buffer in bytes */
    size_t write_size;

    /** Number of bytes already sent from write_buffer */
    size_t write_offset;
} HTTPServerConnection;

/**
 * @brief Initialize an HTTP server connection from an accepted socket
 */
int http_server_connection_initiate(HTTPServerConnection* connection, int fd);

/**
 * @brief Initialize an HTTP server connection with dynamic allocation
 */
int http_server_connection_initiate_ptr(int                    fd,
                                        HTTPServerConnection** connection_ptr);

/**
 * @brief Set the request callback and user context for the connection
 */
void http_server_connection_set_callback(
    HTTPServerConnection* connection, void* context,
    HttpServerConnectionOnRequest on_request);

/**
 * @brief Send a response to the client
 *
 * Must be called from within or after the OnRequest callback.
 */
int http_server_connection_respond(HTTPServerConnection* connection,
                                   const void* data, size_t len);

/**
 * @brief Dispose of connection resources and stop the task
 */
void http_server_connection_dispose(HTTPServerConnection* connection);

/**
 * @brief Dispose of connection and free the connection structure itself
 */
void http_server_connection_dispose_ptr(HTTPServerConnection** connection_ptr);

#endif // HTTP_SERVER_CONNECTION_H
