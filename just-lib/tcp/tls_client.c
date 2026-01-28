/**
 * @file tls_client.c
 * @brief Implementation of non-blocking TLS client using mbedtls.
 */

#include "tls_client.h"

#include "mbedtls/debug.h"
#include "mbedtls/error.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Debug callback for mbedtls (optional)
static void tls_debug(void* ctx, int level, const char* file, int line,
                      const char* str) {
    (void)ctx;
    printf("TLS_DEBUG [%d] %s:%d: %s", level, file, line, str);
}

// Custom send callback for non-blocking socket
static int tls_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd  = *(int*)ctx;
    int ret = send(fd, buf, len, MSG_NOSIGNAL);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    return ret;
}

// Custom receive callback for non-blocking socket
static int tls_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd  = *(int*)ctx;
    int ret = recv(fd, buf, len, 0);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    if (ret == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }

    return ret;
}

int tls_client_init(TLSClient* c) {
    memset(c, 0, sizeof(TLSClient));
    c->fd                 = -1;
    c->handshake_complete = 0;

    // Initialize mbedtls contexts
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_x509_crt_init(&c->cacert);

    // Seed the random number generator
    const char* pers = "tls_client";
    int         ret =
        mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        printf("TLS_DEBUG: mbedtls_ctr_drbg_seed failed: -0x%04x\n", -ret);
        tls_client_dispose(c);
        return -1;
    }

    return 0;
}

int tls_client_load_ca_cert(TLSClient* c, const char* ca_pem) {
    int ret = mbedtls_x509_crt_parse(&c->cacert, (const unsigned char*)ca_pem,
                                     strlen(ca_pem) + 1);
    if (ret < 0) {
        printf("TLS_DEBUG: mbedtls_x509_crt_parse failed: -0x%04x\n", -ret);
        return -1;
    }

    if (ret > 0) {
        printf("TLS_DEBUG: %d certificates failed to parse\n", ret);
    }

    return 0;
}

int tls_client_load_ca_cert_file(TLSClient* c, const char* ca_path) {
    int ret = mbedtls_x509_crt_parse_file(&c->cacert, ca_path);
    if (ret < 0) {
        printf("TLS_DEBUG: mbedtls_x509_crt_parse_file failed: -0x%04x\n",
               -ret);
        return -1;
    }

    if (ret > 0) {
        printf("TLS_DEBUG: %d certificates failed to parse from file\n", ret);
    }

    return 0;
}

int tls_client_connect(TLSClient* c, const char* host, const char* port) {
    printf("TLS_DEBUG: tls_client_connect called with host='%s', port='%s'\n",
           host, port);

    if (c->fd >= 0) {
        printf("TLS_DEBUG: Socket already connected (fd=%d)\n", c->fd);
        return -1;
    }

    // Step 1: Establish TCP connection
    struct addrinfo  hints = {0};
    struct addrinfo* res   = NULL;
    hints.ai_family        = AF_UNSPEC;
    hints.ai_socktype      = SOCK_STREAM;
    hints.ai_protocol      = IPPROTO_TCP;

    printf("TLS_DEBUG: Calling getaddrinfo...\n");
    int gai_result = getaddrinfo(host, port, &hints, &res);
    if (gai_result != 0) {
        printf("TLS_DEBUG: getaddrinfo failed: %s\n", gai_strerror(gai_result));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        printf("TLS_DEBUG: Creating socket with family=%d\n", rp->ai_family);
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (fd < 0) {
            printf("TLS_DEBUG: socket() failed: %s\n", strerror(errno));
            continue;
        }

        printf("TLS_DEBUG: Setting non-blocking mode\n");
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        printf("TLS_DEBUG: Calling connect()...\n");
        int connect_result = connect(fd, rp->ai_addr, rp->ai_addrlen);

        if (connect_result == 0 || errno == EINPROGRESS) {
            printf("TLS_DEBUG: TCP connection initiated successfully\n");
            break;
        }

        printf("TLS_DEBUG: Connection failed, trying next address\n");
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        printf("TLS_DEBUG: All TCP connection attempts failed\n");
        return -1;
    }

    c->fd = fd;

    // Step 2: Configure SSL/TLS
    printf("TLS_DEBUG: Configuring SSL/TLS\n");

    int ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        printf("TLS_DEBUG: mbedtls_ssl_config_defaults failed: -0x%04x\n",
               -ret);
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    // Set CA certificate for verification
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

    // Set RNG
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    // Optional: Set debug callback
    // mbedtls_ssl_conf_dbg(&c->conf, tls_debug, NULL);
    // mbedtls_debug_set_threshold(4);

    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0) {
        printf("TLS_DEBUG: mbedtls_ssl_setup failed: -0x%04x\n", -ret);
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    // Set hostname for SNI (Server Name Indication)
    ret = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (ret != 0) {
        printf("TLS_DEBUG: mbedtls_ssl_set_hostname failed: -0x%04x\n", -ret);
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    // Set custom I/O callbacks
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, tls_send, tls_recv, NULL);

    // Step 3: Perform TLS handshake
    printf("TLS_DEBUG: Starting TLS handshake\n");
    return tls_client_handshake(c);
}

int tls_client_handshake(TLSClient* c) {
    if (c->handshake_complete) {
        return 0;
    }

    int ret = mbedtls_ssl_handshake(&c->ssl);

    if (ret == 0) {
        printf("TLS_DEBUG: TLS handshake completed successfully\n");

        // Verify the server certificate
        uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (flags != 0) {
            char vrfy_buf[512];
            mbedtls_x509_crt_verify_info(vrfy_buf, sizeof(vrfy_buf), "  ! ",
                                         flags);
            printf("TLS_DEBUG: Certificate verification failed:\n%s\n",
                   vrfy_buf);
            return -1;
        }

        printf("TLS_DEBUG: Certificate verification succeeded\n");
        c->handshake_complete = 1;
        return 0;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        // Handshake in progress, need to call again
        return 1;
    }

    // Error occurred
    char error_buf[100];
    mbedtls_strerror(ret, error_buf, sizeof(error_buf));
    printf("TLS_DEBUG: TLS handshake failed: -0x%04x (%s)\n", -ret, error_buf);
    return -1;
}

int tls_client_write(TLSClient* c, const uint8_t* buf, size_t len) {
    if (!c->handshake_complete) {
        printf("TLS_DEBUG: Cannot write - handshake not complete\n");
        return -1;
    }

    int ret = mbedtls_ssl_write(&c->ssl, buf, len);

    if (ret > 0) {
        return ret;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
        return 0; // Would block
    }

    printf("TLS_DEBUG: mbedtls_ssl_write failed: -0x%04x\n", -ret);
    return -1;
}

int tls_client_read(TLSClient* c, uint8_t* buf, size_t len) {
    if (!c->handshake_complete) {
        printf("TLS_DEBUG: Cannot read - handshake not complete\n");
        return -1;
    }

    int ret = mbedtls_ssl_read(&c->ssl, buf, len);

    if (ret > 0) {
        return ret;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0; // Would block
    }

    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        ret == MBEDTLS_ERR_NET_CONN_RESET) {
        printf("TLS_DEBUG: Connection closed by peer\n");
        return -2; // EOF
    }

    printf("TLS_DEBUG: mbedtls_ssl_read failed: -0x%04x\n", -ret);
    return -1;
}

void tls_client_disconnect(TLSClient* c) {
    if (c->fd >= 0) {
        // Send close_notify alert
        if (c->handshake_complete) {
            printf("TLS_DEBUG: Sending close_notify\n");
            mbedtls_ssl_close_notify(&c->ssl);
        }

        close(c->fd);
        c->fd = -1;
    }

    c->handshake_complete = 0;
}

void tls_client_dispose(TLSClient* c) {
    tls_client_disconnect(c);

    // Free mbedtls contexts
    mbedtls_x509_crt_free(&c->cacert);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
}
