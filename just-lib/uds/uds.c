/*
 * uds.c — blocking Unix Domain Socket implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "uds.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static UDSStatus write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p    = (const uint8_t*)buf;
    size_t         left = n;

    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return UDS_ERR;
        }
        if (w == 0) {
            return UDS_CLOSED;
        }
        p += w;
        left -= (size_t)w;
    }
    return UDS_OK;
}

static UDSStatus read_all(int fd, void* buf, size_t n) {
    uint8_t* p    = (uint8_t*)buf;
    size_t   left = n;

    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return UDS_ERR;
        }
        if (r == 0) {
            return UDS_CLOSED;
        }
        p += r;
        left -= (size_t)r;
    }
    return UDS_OK;
}

UDSStatus uds_send(int fd, const void* data, uint32_t len) {
    if (!data) {
        return UDS_BADARG;
    }

    // Write 4-byte big-endian length prefix
    uint8_t hdr[4] = {
        (uint8_t)(len >> 24),
        (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),
        (uint8_t)(len),
    };

    UDSStatus s = write_all(fd, hdr, sizeof(hdr));
    if (s != UDS_OK) {
        return s;
    }

    return write_all(fd, data, len);
}

UDSStatus uds_recv(int fd, uint8_t** out_data, uint32_t* out_len) {
    if (!out_data || !out_len) {
        return UDS_BADARG;
    }

    // Read 4-byte length prefix
    uint8_t   hdr[4];
    UDSStatus s = read_all(fd, hdr, sizeof(hdr));
    if (s != UDS_OK) {
        return s;
    }

    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | ((uint32_t)hdr[3]);

    if (len > UDS_MAX_MSG) {
        return UDS_TOOBIG;
    }

    uint8_t* buf = malloc(len + 1);
    if (!buf) {
        return UDS_NOMEM;
    }

    s = read_all(fd, buf, len);
    if (s != UDS_OK) {
        free(buf);
        return s;
    }

    buf[len]  = '\0';
    *out_data = buf;
    *out_len  = len;
    return UDS_OK;
}

UDSStatus uds_server_init(UDSServer* srv, const char* socket_path) {
    if (!srv || !socket_path) {
        return UDS_BADARG;
    }

    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;

    // unlink trash
    unlink(socket_path);

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        perror("uds_server: socket");
        return UDS_ERR;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("uds_server: bind");
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return UDS_ERR;
    }

    if (listen(srv->listen_fd, 5) < 0) {
        perror("uds_server: listen");
        close(srv->listen_fd);
        unlink(socket_path);
        srv->listen_fd = -1;
        return UDS_ERR;
    }

    strncpy(srv->path, socket_path, sizeof(srv->path) - 1);
    return UDS_OK;
}

int uds_server_accept(UDSServer* srv) {
    if (!srv || srv->listen_fd < 0) {
        return -1;
    }

    int fd = accept(srv->listen_fd, NULL, NULL);
    if (fd < 0) {
        perror("uds_server: accept");
    }
    return fd;
}

void uds_server_destroy(UDSServer* srv) {
    if (!srv) {
        return;
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    if (srv->path[0] != '\0') {
        unlink(srv->path);
        srv->path[0] = '\0';
    }
}

UDSStatus uds_client_init(UDSClient* cli, const char* socket_path) {
    if (!cli || !socket_path) {
        return UDS_BADARG;
    }

    memset(cli, 0, sizeof(*cli));
    cli->fd = -1;

    cli->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli->fd < 0) {
        perror("uds_client: socket");
        return UDS_ERR;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(cli->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("uds_client: connect");
        close(cli->fd);
        cli->fd = -1;
        return UDS_ERR;
    }

    return UDS_OK;
}

void uds_client_destroy(UDSClient* cli) {
    if (!cli) {
        return;
    }
    if (cli->fd >= 0) {
        close(cli->fd);
        cli->fd = -1;
    }
}
