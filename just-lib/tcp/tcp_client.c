#include "tcp_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int tcp_client_initiate(TCPClient* c, int fd) {
    c->fd = fd;
    return 0;
}

int tcp_client_connect(TCPClient* c, const char* host, const char* port) {
    printf("TCP_DEBUG: tcp_client_connect called with host='%s', port='%s'\n",
           host, port);

    if (c->fd >= 0) {
        printf("TCP_DEBUG: Socket already connected (fd=%d)\n", c->fd);
        return -1;
    }

    struct addrinfo  hints = {0};
    struct addrinfo* res   = NULL;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int gai_result = getaddrinfo(host, port, &hints, &res);
    if (gai_result != 0) {
        printf("TCP_DEBUG: getaddrinfo failed: %s\n", gai_strerror(gai_result));
        return -1;
    }

    int fd = -1;
    int rc = -1;

    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        // non-blocking
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int cr = connect(fd, rp->ai_addr, rp->ai_addrlen);

        if (cr == 0) {
            // connected immediately
            rc = 0;
            break;
        }

        if (cr < 0 && errno == EINPROGRESS) {
            // connection in progress (normal for non-blocking)
            rc = 1;
            break;
        }

        // real failure
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        return -1;
    }

    c->fd = fd;
    return rc;
}

int tcp_client_write(TCPClient* c, const uint8_t* buf, size_t len) {
    int n = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // Would block, try again later
        }
        return -1; // Real error
    }
    return n;
}

int tcp_client_read(TCPClient* c, uint8_t* buf, size_t len) {
    /* Clear errno so we can distinguish EOF (recv==0) from previous errors */
    errno = 0;
    int n = recv(c->fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; // no data available right now
        }
        return -1; // real error
    }

    if (n == 0) {
        return -2; // EOF (peer closed connection)
    }

    return n;
}

void tcp_client_disconnect(TCPClient* c) {
    if (c->fd >= 0) {
        close(c->fd);
    }

    c->fd = -1;
}

void tcp_client_dispose(TCPClient* c) { tcp_client_disconnect(c); }
