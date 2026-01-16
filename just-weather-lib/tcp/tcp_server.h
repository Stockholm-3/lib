/**
 * @file tcp_server.h
 * @brief Non-blocking TCP server with callback-based connection handling.
 *
 * This module provides a lightweight TCP server implementation that listens
 * for incoming connections on a specified port and handles client connections
 * through a user-provided callback function.
 *
 * The server operates in non-blocking mode and integrates with a scheduler
 * task system to poll for incoming connections without blocking the main
 * event loop. When a client connects, the server accepts the connection,
 * configures it as non-blocking, and invokes the registered callback.
 *
 * Key features:
 * - Non-blocking I/O for both listening and accepted sockets
 * - Callback-based connection handling
 * - Integration with scheduler for asynchronous operation
 * - Support for up to MAX_CLIENTS (512) pending connections
 * - Both stack and heap allocation patterns
 *
 * The accept callback receives the client file descriptor and a user-defined
 * context pointer, allowing flexible integration with higher-level protocols
 * (e.g., HTTP servers).
 *
 * @note All sockets created by this module are configured in non-blocking mode.
 * @note The server binds to all available network interfaces (INADDR_ANY).
 *
 * @see smw.h
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#define POSIX_C_SOURCE 200809L
#include "smw.h"

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_CLIENTS 512

// Max ammount of clients it accepts per one work tick
#define ACCEPT_BUDGET_PER_TICK 10

typedef int (*TcpServerOnAccept)(int client_fd, void* context);

typedef struct {
    int listen_fd;

    TcpServerOnAccept onAccept;
    void*             context;

    SmwTask* task;

} TCPServer;

/**
 * @brief Initialize a TCP server and begin listening on a port.
 *
 * This function creates a non-blocking listening socket, binds it to the
 * specified port on all available network interfaces, and starts listening
 * for incoming connections.
 *
 * A task is also registered to periodically poll the listening socket
 * and invoke the accept callback when clients connect.
 *
 * @param server     Pointer to the TCPServer to initialize.
 * @param port       Port number or service name to bind to (e.g. "8080").
 * @param on_accept  Callback invoked when a client connection is accepted.
 * @param context    User-defined pointer passed to the accept callback.
 *
 * @return 0 on success, -1 on failure.
 *
 * @note The listening socket is configured in non-blocking mode.
 * @note The server will accept up to MAX_CLIENTS pending connections.
 */
int tcp_server_initiate(TCPServer* server, const char* port,
                        TcpServerOnAccept on_accept, void* context);

/**
 * @brief Allocate and initialize a TCPServer.
 *
 * This function dynamically allocates a TCPServer structure and initializes
 * it by calling tcp_server_initiate().
 *
 * If initialization fails, all allocated memory is released.
 *
 * @param port       Port number or service name to bind to.
 * @param on_accept  Callback invoked when a client connects.
 * @param context    User-defined pointer passed to the accept callback.
 * @param server_ptr Output pointer that will receive the allocated TCPServer.
 *
 * @return
 *   - 0 on success.
 *   - -1 if server_ptr is NULL.
 *   - -2 if memory allocation fails.
 *   - Any error code returned by tcp_server_initiate().
 */
int tcp_server_initiate_ptr(const char* port, TcpServerOnAccept on_accept,
                            void* context, TCPServer** server_ptr);

/**
 * @brief Shut down a TCPServer and release its scheduler task.
 *
 * This function stops the server's task and releases internal resources.
 * It does not free the TCPServer structure itself.
 *
 * @param server Pointer to the TCPServer to dispose.
 */
void tcp_server_dispose(TCPServer* server);

/**
 * @brief Destroy and free a dynamically allocated TCPServer.
 *
 * This function calls tcp_server_dispose(), frees the server structure,
 * and sets the caller's pointer to NULL.
 *
 * @param server_ptr Pointer to a TCPServer pointer.
 */
void tcp_server_dispose_ptr(TCPServer** server_ptr);

#endif // TCP_SERVER_H
