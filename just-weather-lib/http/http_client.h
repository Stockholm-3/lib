#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "smw.h"
#include "tcp_client.h"

#ifndef http_client_max_url_length
#    define http_client_max_url_length 1024
#endif

typedef enum {
    HTTP_CLIENT_STATE_INIT       = 0,
    HTTP_CLIENT_STATE_CONNECT    = 1,
    HTTP_CLIENT_STATE_CONNECTING = 2,
    HTTP_CLIENT_STATE_WRITING    = 3,
    HTTP_CLIENT_STATE_READING    = 4, // kanske lägger till connecting
    HTTP_CLIENT_STATE_DONE       = 5,
    HTTP_CLIENT_STATE_DISPOSE    = 6,

} HttpClientState;

typedef struct {
    HttpClientState state;
    SmwTask*        task;
    char            url[http_client_max_url_length + 1];
    uint64_t        timeout;

    void (*callback)(const char* event, const char* response);

    uint64_t timer;

    uint8_t* write_buffer;
    size_t   write_size;
    size_t   write_offset;

    uint8_t* read_buffer;      // Buffer for incoming data
    size_t   read_buffer_size; // Current size of read buffer
    size_t   body_start;       // Position where HTTP body starts
    size_t   content_len;      // Content-Length from headers
    int      status_code;      // HTTP status code (200, 404, etc.)
    uint8_t* body;             // Extracted response body

    /* HTTP transfer encoding support */
    int chunked;          // Transfer-Encoding: chunked detected
    int connection_close; // Connection: close header present

    TCPClient*
         tcp_conn; // Handle to TCP connection, är en tcp connection struct
    char hostname[256]; // Parsed from URL
    char path[512];     // Parsed from URL
    char port[16];      // Parsed from URL
    char response[8192];
} HttpClient;

/**
 * @brief Create and initialize an HTTP client instance.
 *
 * This function allocates a new HttpClient, initializes all fields,
 * stores the URL, and registers a scheduler task to drive its
 * internal state machine.
 *
 * @param u_rl       HTTP or HTTPS URL to request.
 * @param client_ptr Output pointer that receives the allocated client.
 * @param port       Optional explicit port override (may be NULL).
 *
 * @return
 *   - 0 on success
 *   - -1 if arguments are invalid
 *   - -2 if URL is too long
 *   - -3 if memory allocation fails
 *
 * @note The client is created in HTTP_CLIENT_STATE_INIT state.
 */

/**
 * @brief Start an asynchronous HTTP GET request.
 *
 * This function creates a new HttpClient and schedules it to
 * execute a GET request on the given URL.
 *
 * Results and errors are delivered via the provided callback.
 *
 * @param url       URL to fetch.
 * @param timeout   Timeout in scheduler ticks.
 * @param callback  Callback invoked on response, error, or timeout.
 * @param port      Optional port override.
 *
 * @return 0 on success, non-zero on failure.
 */
int http_client_get(const char* url, uint64_t timeout,
                    void (*callback)(const char* event, const char* response),
                    const char* port);

/**
 * @brief Scheduler-driven HTTP client state machine.
 *
 * This function is repeatedly called by the scheduler and
 * advances the client through its lifecycle:
 *
 * INIT → CONNECT → CONNECTING → WRITING → READING → DONE → DISPOSE
 *
 * It also enforces the configured timeout.
 *
 * @param context  Pointer to HttpClient.
 * @param mon_time Current scheduler time.
 */
void http_client_work(void* context, uint64_t mon_time);

/**
 * @brief Destroy an HTTP client and free all resources.
 *
 * Stops the scheduler task, releases memory, and invalidates
 * the client pointer.
 *
 * @param client_ptr Pointer to HttpClient pointer.
 */
void http_client_dispose(HttpClient** client_ptr);

#endif // http_client_h
