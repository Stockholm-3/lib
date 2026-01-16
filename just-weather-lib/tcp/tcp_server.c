#include "tcp_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

//-----------------Internal Functions-----------------

static void tcp_server_task_work(void* context, uint64_t mon_time);
static int  tcp_server_accept(TCPServer* server);
static int  tcp_server_nonblocking(int fd);

//----------------------------------------------------

int tcp_server_initiate(TCPServer* server, const char* port,
                        TcpServerOnAccept on_accept, void* context) {
    server->onAccept = on_accept;
    server->context  = context;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &res) != 0) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd < 0) {
        return -1;
    }

    if (listen(fd, MAX_CLIENTS) < 0) {
        close(fd);
        return -1;
    }

    tcp_server_nonblocking(fd);

    server->listen_fd = fd;

    server->task = smw_create_task(server, tcp_server_task_work);

    return 0;
}

int tcp_server_initiate_ptr(const char* port, TcpServerOnAccept on_accept,
                            void* context, TCPServer** server_ptr) {
    if (server_ptr == NULL) {
        return -1;
    }

    TCPServer* server = (TCPServer*)malloc(sizeof(TCPServer));
    if (server == NULL) {
        return -2;
    }

    int result = tcp_server_initiate(server, port, on_accept, context);
    if (result != 0) {
        free(server);
        return result;
    }

    *(server_ptr) = server;

    return 0;
}

void tcp_server_dispose(TCPServer* server) { smw_destroy_task(server->task); }

void tcp_server_dispose_ptr(TCPServer** server_ptr) {
    if (server_ptr == NULL || *(server_ptr) == NULL) {
        return;
    }

    tcp_server_dispose(*(server_ptr));
    free(*(server_ptr));
    *(server_ptr) = NULL;
}

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
int tcp_server_accept(TCPServer* server) {
    int socket_fd = accept(server->listen_fd, NULL, NULL);
    if (socket_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // ingen ny klient
        }

        perror("failed to accept");
        return -1;
    }

    tcp_server_nonblocking(socket_fd);

    int result = server->onAccept(socket_fd, server->context);
    if (result != 0) {
        close(socket_fd);
    }

    return 1;
}

/**
 * @brief Scheduler task that polls for incoming client connections.
 *
 * This function is called by the scheduler to service the listening socket.
 * It simply delegates to tcp_server_accept().
 *
 * @param context   Pointer to the TCPServer.
 * @param mon_time  Scheduler timestamp (unused).
 */
static void tcp_server_task_work(void* context, uint64_t mon_time) {
    TCPServer* server = context;

    int accepted = 0;
    while (accepted < ACCEPT_BUDGET_PER_TICK) {
        int r = tcp_server_accept(server);
        if (r <= 0) {
            break;
        }
        accepted++;
    }
}

/**
 * @brief Set a file descriptor to non-blocking mode.
 *
 * This function retrieves the current flags of the given file descriptor
 * and adds the O_NONBLOCK flag, so that subsequent I/O calls on the fd
 * do not block. Useful for sockets or pipes in event-driven servers.
 *
 * @param fd The file descriptor to set as non-blocking.
 *
 * @return
 *   - 0 on success.
 *   - -1 on failure (errno is set).
 *
 * @note This function only modifies the file descriptor flags; it does
 *       not perform any other I/O operation.
 */
static int tcp_server_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
