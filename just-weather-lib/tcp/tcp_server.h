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
 * @brief Accept a pending client connection if one is available.
 *
 * This function attempts to accept a new client from the listening socket.
 * If no connection is currently pending, it returns without error.
 *
 * When a client is accepted, the socket is placed into non-blocking mode and
 * passed to the server's accept callback.
 *
 * If the callback returns a non-zero value, the socket is closed immediately.
 *
 * @param server Pointer to the TCPServer.
 *
 * @return
 *   - 0 on success or if no client is waiting.
 *   - -1 on a fatal accept error.
 */
int tcp_server_accept(TCPServer* server);

/**
 * @brief Scheduler task that polls for incoming client connections.
 *
 * This function is called by the scheduler to service the listening socket.
 * It simply delegates to tcp_server_accept().
 *
 * @param context   Pointer to the TCPServer.
 * @param mon_time  Scheduler timestamp (unused).
 */
void tcp_server_task_work(void* context, uint64_t mon_time);

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

static inline int tcp_server_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif // TCP_SERVER_H
