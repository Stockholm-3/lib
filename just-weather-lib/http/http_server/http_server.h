
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "http_server_connection.h"
#include "smw.h"
#include "tcp_server.h"

typedef int (*HttpServerOnConnection)(void*                 context,
                                      HTTPServerConnection* connection);

typedef struct {
    HttpServerOnConnection onConnection;

    TCPServer tcpServer;
    SmwTask*  task;

} HTTPServer;

/**
 * @brief Initialize an HTTP server.
 *
 * This function creates a TCP server listening on port 10680 and
 * registers an accept callback that creates HTTPServerConnection
 * objects for each new client.
 *
 * A scheduler task is also created for future server-wide processing.
 *
 * @param server         Pointer to the HTTPServer to initialize.
 * @param on_connection Callback invoked for each accepted HTTP connection.
 *
 * @return 0 on success, non-zero on failure.
 */
int http_server_initiate(HTTPServer*            server,
                         HttpServerOnConnection on_connection);

/**
 * @brief Allocate and initialize an HTTP server.
 *
 * This function dynamically allocates an HTTPServer and initializes
 * it using http_server_initiate().
 *
 * @param on_connection Callback invoked when a client connects.
 * @param server_ptr    Output pointer receiving the allocated HTTPServer.
 *
 * @return
 *   - 0 on success
 *   - -1 if server_ptr is NULL
 *   - -2 if memory allocation fails
 *   - Any error returned by http_server_initiate()
 */
int http_server_initiate_ptr(HttpServerOnConnection on_connection,
                             HTTPServer**           server_ptr);

/**
 * @brief Shut down an HTTP server.
 *
 * This function stops the underlying TCP server and destroys
 * the scheduler task associated with the HTTP server.
 *
 * @param server Pointer to the HTTPServer.
 */
void http_server_dispose(HTTPServer* server);

/**
 * @brief Destroy and free an HTTPServer.
 *
 * This function shuts down the server, frees its memory,
 * and sets the caller's pointer to NULL.
 *
 * @param server_ptr Pointer to an HTTPServer pointer.
 */
void http_server_dispose_ptr(HTTPServer** server_ptr);

#endif // HTTP_SERVER_H
