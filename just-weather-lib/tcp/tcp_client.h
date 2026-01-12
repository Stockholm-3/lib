/**
 * @file tcp_client.h
 * @brief Non-blocking TCP client for connecting to remote hosts.
 *
 * This module provides a lightweight TCP client implementation that supports
 * non-blocking connection establishment and I/O operations. It wraps standard
 * POSIX socket operations with a simplified interface suitable for event-driven
 * or scheduler-based network programming.
 *
 * The client can be initialized with an existing socket descriptor (useful for
 * server-accepted connections) or can establish new outbound connections to
 * remote hosts using hostname and port strings.
 *
 * Key features:
 * - Non-blocking connection establishment
 * - Hostname resolution via getaddrinfo()
 * - Support for both IPv4 and IPv6
 * - Distinguishes between temporary unavailability and connection closure
 * - SIGPIPE prevention on write operations
 * - Simple, minimal API with explicit resource management
 *
 * All I/O operations are designed to work with non-blocking sockets and
 * return appropriate values to support integration with event loops, poll(),
 * select(), or scheduler task systems.
 *
 * @note Sockets created by this module are automatically configured in
 *       non-blocking mode during connection establishment.
 */

#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stddef.h>
#define POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int fd;
} TCPClient;

/**
 * @brief Initialize a TCP client with an existing file descriptor.
 *
 * This function assigns a socket file descriptor to a TCPClient instance.
 * It does not perform any network operations or validation on the descriptor.
 *
 * @param c   Pointer to the TCPClient structure to initialize.
 * @param fd  A valid socket file descriptor, or -1 if no socket is assigned.
 *
 * @return 0 on success.
 *
 * @note This function does not check whether the file descriptor is connected
 *       or valid. The caller is responsible for providing a correct descriptor.
 */
int tcp_client_initiate(TCPClient* c, int fd);

/**
 * @brief Create and initiate a non-blocking TCP connection to a remote host.
 *
 * This function resolves the given host and port using getaddrinfo(), creates
 * a socket, sets it to non-blocking mode, and attempts to connect.
 *
 * If the connection cannot be completed immediately, EINPROGRESS is treated
 * as a successful initiation, and the socket is left in a connecting state.
 *
 * On success, the socket file descriptor is stored in the TCPClient structure.
 *
 * @param c     Pointer to the TCPClient to use for the connection.
 * @param host  Hostname or IP address of the remote peer.
 * @param port  Service name or numeric port (e.g. "80", "443").
 *
 * @return 0 on success, -1 on failure.
 *
 * @warning If @p c already has an open file descriptor, this function will
 * fail.
 *
 * @note The connection may not be fully established when this function returns.
 *       The caller should use poll(), select(), or similar mechanisms to detect
 *       when the socket becomes writable to confirm connection completion.
 */
int tcp_client_connect(TCPClient* c, const char* host, const char* port);

/**
 * @brief Send data over a TCP connection.
 *
 * This function sends up to @p len bytes from the provided buffer to the
 * remote peer using the TCP socket stored in the client.
 *
 * MSG_NOSIGNAL is used to prevent SIGPIPE from being raised if the connection
 * has been closed by the peer.
 *
 * @param c    Pointer to the connected TCPClient.
 * @param buf  Buffer containing data to send.
 * @param len  Number of bytes to send.
 *
 * @return The number of bytes sent on success, or -1 on error.
 *
 * @note In non-blocking mode, this function may send fewer bytes than
 * requested. The caller should handle partial writes.
 */
int tcp_client_write(TCPClient* c, const uint8_t* buf, size_t len);

/**
 * @brief Receive data from a TCP connection.
 *
 * This function attempts to read up to @p len bytes from the TCP socket
 * into the provided buffer.
 *
 * The socket is expected to be in non-blocking mode.
 *
 * @param c    Pointer to the connected TCPClient.
 * @param buf  Destination buffer for received data.
 * @param len  Maximum number of bytes to read.
 *
 * @return
 *   - A positive value indicating the number of bytes read.
 *   - 0 if no data is currently available (EAGAIN / EWOULDBLOCK).
 *   - -2 if the peer has closed the connection (EOF).
 *   - -1 on a fatal read error.
 *
 * @note This function distinguishes between temporary lack of data and
 *       connection closure, which is important for event-driven I/O.
 */
int tcp_client_read(TCPClient* c, uint8_t* buf, size_t len);

/**
 * @brief Close the TCP connection associated with the client.
 *
 * If a valid socket is present, it is closed and the file descriptor
 * is reset to -1.
 *
 * @param c Pointer to the TCPClient to disconnect.
 *
 * @note This function is safe to call even if the client is already
 * disconnected.
 */
void tcp_client_disconnect(TCPClient* c);

/**
 * @brief Release all resources associated with a TCPClient.
 *
 * This function currently closes the socket connection by calling
 * tcp_client_disconnect(). It exists to provide a symmetric cleanup
 * API and may be extended in the future.
 *
 * @param c Pointer to the TCPClient to dispose.
 */
void tcp_client_dispose(TCPClient* c);

#endif // TCP_CLIENT_H
