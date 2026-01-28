/**
 * @file http_client.h
 * @brief Asynchronous HTTP client with scheduler-driven state machine.
 *
 * This module implements a non-blocking HTTP client that performs
 * HTTP GET requests asynchronously via a state machine integrated
 * with a scheduler.
 *
 * Features:
 * - URL parsing (hostname, port, path)
 * - TCP connection establishment via TCPClient
 * - Non-blocking I/O with dynamic read/write buffers
 * - HTTP request construction with standard headers
 * - Response parsing (status code, headers, body)
 * - Content-Length and chunked transfer decoding
 * - Connection management and timeout handling
 *
 * Client lifecycle states:
 * INIT → CONNECT → CONNECTING → WRITING → READING → DONE → DISPOSE
 *
 * Callbacks are used to report:
 * - Successful HTTP response
 * - Errors (connection, memory, parsing)
 * - Timeout events
 *
 * Memory management:
 * - Read and write buffers are allocated dynamically.
 * - Chunked bodies are decoded into dynamically allocated memory.
 * - Client must be disposed via http_client_dispose().
 *
 * @note Maximum URL length is configurable via http_client_max_url_length
 *       (default: 1024 characters).
 *
 * @see tcp_client.h
 * @see smw.h
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "smw.h"
#include "tcp_client.h"

#ifndef http_client_max_url_length
#    define http_client_max_url_length 1024
#endif

/**
 * @brief Callback type for HTTP client events.
 *
 * Events:
 * - "RESPONSE" → successful HTTP response
 * - "ERROR" → error during connection, parsing, or memory allocation
 * - "TIMEOUT" → request timed out
 *
 * @param event    Event type string.
 * @param response Response body or error message (may be NULL for TIMEOUT).
 * @param context  User-provided context pointer.
 */
typedef void (*HttpClientCallback)(const char* event, const char* response,
                                   void* context);

/**
 * @brief HTTP client state machine states.
 */
typedef enum {
    HTTP_CLIENT_STATE_INIT = 0, /**< Initialize client and parse URL */
    HTTP_CLIENT_STATE_CONNECT,  /**< Allocate TCP client and initiate connection
                                 */
    HTTP_CLIENT_STATE_CONNECTING, /**< Wait for TCP connection to complete */
    HTTP_CLIENT_STATE_WRITING,    /**< Send HTTP request */
    HTTP_CLIENT_STATE_READING,    /**< Read HTTP response headers and body */
    HTTP_CLIENT_STATE_DONE,       /**< Response received and callback invoked */
    HTTP_CLIENT_STATE_DISPOSE     /**< Clean up resources */
} HttpClientState;

/**
 * @brief Asynchronous HTTP client structure.
 *
 * Fields include:
 * - State machine tracking
 * - Scheduler task pointer
 * - URL and parsed components
 * - TCP connection handle
 * - Dynamic read/write buffers
 * - Parsed response info: status code, body, headers
 * - Chunked transfer and connection-close detection
 * - User callback and context
 */
typedef struct {
    HttpClientState state;
    SmwTask*        task;
    char            url[http_client_max_url_length + 1];
    uint64_t        timeout;

    HttpClientCallback callback;
    void*              context;

    uint64_t timer;

    uint8_t* write_buffer;
    size_t   write_size;
    size_t   write_offset;

    uint8_t* read_buffer;
    size_t   read_buffer_size;
    size_t   body_start;
    size_t   content_len;
    int      status_code;
    uint8_t* body;

    int chunked;
    int connection_close;

    TCPClient* tcp_conn;
    char       hostname[256];
    char       path[512];
    char       port[16];
    char       response[8192];
} HttpClient;

/**
 * @brief Start an asynchronous HTTP GET request.
 *
 * Initializes the client, sets callback/context, timeout, and schedules
 * it to run via the state machine.
 *
 * @param url       URL to fetch.
 * @param port      Optional port override.
 * @param timeout   Timeout in scheduler ticks.
 * @param callback  Callback invoked on response/error/timeout.
 * @param context   User-provided context pointer.
 *
 * @return 0 on success, non-zero on failure.
 */
int http_client_get(const char* url, const char* port, uint64_t timeout,
                    HttpClientCallback callback, void* context);

/**
 * @brief Scheduler-driven HTTP client state machine.
 *
 * Advances the client through its lifecycle and handles timeout detection.
 *
 * @param context  Pointer to HttpClient instance.
 * @param mon_time Current scheduler time.
 */
void http_client_work(void* context, uint64_t mon_time);

/**
 * @brief Destroy an HTTP client and free all resources.
 *
 * Stops the scheduler task, releases memory, closes TCP connection,
 * and invalidates the client pointer.
 *
 * @param client_ptr Pointer to HttpClient pointer.
 */
void http_client_dispose(HttpClient** client_ptr);

/**
 * @brief Parse an HTTP or HTTPS URL into hostname, port, and path.
 *
 * Examples:
 * - http://example.com       → host=example.com, port=80, path=/
 * - https://example.com:8443 → host=example.com, port=8443, path=/
 * - http://example.com/api   → host=example.com, port=80, path=/api
 *
 * @param url      Input URL.
 * @param hostname Output buffer for hostname (max 256 bytes).
 * @param port     Output buffer for port string (max 16 bytes).
 * @param path     Output buffer for path (max 512 bytes).
 *
 * @return 0 on success, non-zero on failure.
 */
int parse_url(const char* url, char* hostname, char* port, char* path);
#endif // HTTP_CLIENT_H
