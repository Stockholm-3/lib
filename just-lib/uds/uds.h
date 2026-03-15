/*
 * uds.h — blocking Unix Domain Socket, client and server
 *
 * Wire format: [ uint32_t length (4 bytes, big-endian) ][ payload ]
 *
 * Every call blocks until complete or an error occurs.
 * Run the server's accept/recv loop in its own thread.
 */

#ifndef UDS_H
#define UDS_H

#include <stddef.h>
#include <stdint.h>

/* Largest single message accepted (guards against runaway senders) */
#define UDS_MAX_MSG (4u * 1024u * 1024u) /* 4 MB */

typedef enum {
    UDS_OK     = 0,
    UDS_ERR    = -1, /* I/O error, check errno   */
    UDS_CLOSED = -2, /* peer closed connection   */
    UDS_TOOBIG = -3, /* message > UDS_MAX_MSG    */
    UDS_NOMEM  = -4, /* malloc failed            */
    UDS_BADARG = -5, /* NULL pointer passed in   */
} UDSStatus;

/* -------------------------------------------------------------------------
 * Server
 * ---------------------------------------------------------------------- */

typedef struct {
    int  listen_fd;
    char path[108]; /* stored so destroy() can unlink it */
} UDSServer;

/*
 * uds_server_init — bind + listen on socket_path.
 * Returns UDS_OK or UDS_ERR (check errno).
 */
UDSStatus uds_server_init(UDSServer* srv, const char* socket_path);

/*
 * uds_server_accept — block until a client connects.
 * Returns a connected fd >= 0 on success, -1 on error.
 */
int uds_server_accept(UDSServer* srv);

/*
 * uds_server_destroy — close listen fd and unlink the socket file.
 */
void uds_server_destroy(UDSServer* srv);

/* -------------------------------------------------------------------------
 * Client
 * ---------------------------------------------------------------------- */

typedef struct {
    int fd;
} UDSClient;

/*
 * uds_client_init — connect to socket_path, blocks until connected.
 * Returns UDS_OK or UDS_ERR (check errno).
 */
UDSStatus uds_client_init(UDSClient* cli, const char* socket_path);

/*
 * uds_client_destroy — close the connection.
 */
void uds_client_destroy(UDSClient* cli);

/* -------------------------------------------------------------------------
 * Send / recv  (work on any connected fd — server-side or client-side)
 * ---------------------------------------------------------------------- */

/*
 * uds_send — send `len` bytes from `data`.
 * Blocks until all bytes are written or an error occurs.
 */
UDSStatus uds_send(int fd, const void* data, uint32_t len);

/*
 * uds_recv — receive one message into a heap-allocated buffer.
 * Blocks until the full message arrives.
 *
 * On UDS_OK:
 *   *out_data  — heap-allocated buffer, always null-terminated
 *   *out_len   — payload length in bytes (excludes the null terminator)
 *   Caller must free(*out_data).
 */
UDSStatus uds_recv(int fd, uint8_t** out_data, uint32_t* out_len);

#endif /* UDS_H */
