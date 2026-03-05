/**
 * @file http_client.c
 * @brief Implementation of asynchronous HTTP/HTTPS client.
 */

#include "http_client.h"

#include "errno.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define CHUNK_SIZE 4096
#define PORTSIZE 100

// Default CA certificate paths for common systems
static const char* g_default_ca_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt",     // Debian/Ubuntu/Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",       // Fedora/RHEL/CentOS
    "/etc/ssl/cert.pem",                      // Alpine/macOS
    "/etc/ssl/ca-bundle.pem",                 // OpenSUSE
    "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD
    NULL};

//---------------Internal functions----------------

static int decode_chunked(const uint8_t* in, size_t in_len, char** out,
                          size_t* out_len);
int parse_url(const char* url, char* hostname, char* port_str, char* path);
static const char* find_ca_cert_path(void);

//----------------------------------------------------

/**
 * @brief Find a valid CA certificate file on the system.
 *
 * @return Path to CA cert file, or NULL if none found.
 */
static const char* find_ca_cert_path(void) {
    for (int i = 0; g_default_ca_paths[i] != NULL; i++) {
        FILE* f = fopen(g_default_ca_paths[i], "r");
        if (f) {
            fclose(f);
            return g_default_ca_paths[i];
        }
    }
    return NULL;
}

/**
 * @brief Decode HTTP chunked transfer encoding.
 *
 * This function decodes a buffer encoded using HTTP/1.1
 * "Transfer-Encoding: chunked" format and produces a
 * contiguous decoded body.
 *
 * Memory for the decoded output is dynamically allocated
 * and must be freed by the caller.
 *
 * @param in       Pointer to the chunked-encoded input buffer.
 * @param in_len  Size of the input buffer in bytes.
 * @param out     Output pointer that will receive the allocated decoded data.
 * @param out_len Output length of the decoded data.
 *
 * @return 0 on success, non-zero on failure.
 *
 * @note The output buffer is always null-terminated for convenience.
 */

static int decode_chunked(const uint8_t* in, size_t in_len, char** out,
                          size_t* out_len) {
    if (!in || !out || !out_len) {
        return -1;
    }

    size_t pos   = 0;
    size_t alloc = 1024;
    char*  buf   = malloc(alloc);
    if (!buf) {
        return -2;
    }
    size_t buf_len = 0;

    while (pos < in_len) {
        /* read chunk size line (hex) */
        size_t line_start = pos;
        while (pos < in_len &&
               !(in[pos] == '\r' && pos + 1 < in_len && in[pos + 1] == '\n')) {
            pos++;
        }

        if (pos >= in_len) {
            free(buf);
            return -3;
        }

        size_t line_len = pos - line_start;
        if (line_len == 0) {
            free(buf);
            return -4;
        }

        /* parse hex size */
        char* hex = malloc(line_len + 1);
        if (!hex) {
            free(buf);
            return -5;
        }
        memcpy(hex, in + line_start, line_len);
        hex[line_len] = '\0';

        char*         endptr     = NULL;
        unsigned long chunk_size = strtoul(hex, &endptr, 16);
        if (endptr == hex) {
            free(hex);
            free(buf);
            return -6;
        }
        free(hex);

        /* advance past CRLF */
        pos += 2; /* skip \r\n */

        if (chunk_size == 0) {
            /* consume trailing CRLF after last chunk if present */
            if (pos + 1 < in_len && in[pos] == '\r' && in[pos + 1] == '\n') {
                pos += 2;
            }
            break; /* done */
        }

        /* ensure we have chunk_size bytes available */
        if (pos + chunk_size > in_len) {
            free(buf);
            return -7;
        }

        /* append chunk data */
        if (buf_len + chunk_size + 1 > alloc) {
            while (buf_len + chunk_size + 1 > alloc) {
                alloc *= 2;
            }
            char* nbuf = realloc(buf, alloc);
            if (!nbuf) {
                free(buf);
                return -8;
            }
            buf = nbuf;
        }

        memcpy(buf + buf_len, in + pos, chunk_size);
        buf_len += chunk_size;
        pos += chunk_size;

        /* expect CRLF after chunk data */
        if (pos + 1 >= in_len || in[pos] != '\r' || in[pos + 1] != '\n') {
            free(buf);
            return -9;
        }
        pos += 2;
    }

    /* null-terminate */
    if (buf_len + 1 > alloc) {
        char* nbuf = realloc(buf, buf_len + 1);
        if (!nbuf) {
            free(buf);
            return -10;
        }
        buf = nbuf;
    }
    buf[buf_len] = '\0';

    *out     = buf;
    *out_len = buf_len;
    return 0;
}

int http_client_init(const char* url, HttpClient** client_ptr,
                     const char* port) {
    if (url == NULL || client_ptr == NULL) {
        return -1;
    }

    if (strlen(url) > http_client_max_url_length) {
        return -2;
    }

    HttpClient* client = (HttpClient*)calloc(1, sizeof(HttpClient));
    if (client == NULL) {
        return -3;
    }

    /* ensure all fields start zeroed to avoid undefined state */
    client->state = HTTP_CLIENT_STATE_INIT;

    client->callback = NULL;
    client->timer    = 0;

    /* copy url (url buffer already zeroed by calloc) */
    strcpy(client->url, url);

    /* explicit initialization for clarity */
    client->tcp_conn    = NULL; // Also covers tls_conn in union
    client->hostname[0] = '\0';
    client->path[0]     = '\0';
    client->port[0]     = '\0';

    client->write_buffer     = NULL;
    client->write_size       = 0;
    client->write_offset     = 0;
    client->read_buffer      = NULL;
    client->read_buffer_size = 0;
    client->body_start       = 0;
    client->content_len      = 0;
    client->status_code      = 0;
    client->body             = NULL;

    client->is_https        = 0;
    client->ca_cert_path[0] = '\0';

    /* If custom port provided, override the default */
    if (port != NULL && strlen(port) > 0) {
        strncpy(client->port, port, sizeof(client->port) - 1);
        client->port[sizeof(client->port) - 1] = '\0';
    }

    /* Task is NOT registered in SMW here; callers that want async SMW-driven
     * operation must register explicitly (e.g. via http_client_get). Sync
     * callers that drive the client in their own loop must not appear in the
     * global SMW task list or two threads will race on the same state machine.
     */
    client->task = NULL;

    *(client_ptr) = client;

    return 0;
}

int http_client_set_ca_cert(HttpClient* client, const char* ca_path) {
    if (client == NULL || ca_path == NULL) {
        return -1;
    }

    strncpy(client->ca_cert_path, ca_path, sizeof(client->ca_cert_path) - 1);
    client->ca_cert_path[sizeof(client->ca_cert_path) - 1] = '\0';

    return 0;
}

int http_client_get(const char* url, const char* port, uint64_t timeout,
                    HttpClientCallback callback, void* context) {
    HttpClient* client = NULL;
    if (http_client_init(url, &client, port) != 0) {
        return -1;
    }

    client->timeout  = timeout;
    client->callback = callback;
    client->context  = context;

    /* Register in SMW so the main-thread event loop drives this client.
     * This is safe because http_client_get is called from request-handler
     * threads, never from compute threads that already run their own loop.
     */
    client->task = smw_create_task(client, http_client_work);

    return 0;
}

HttpClientState http_client_work_init(HttpClient* client) {
    // 1. Capture the custom port if one was provided during init/get
    char custom_port[16] = {0};
    int  has_custom_port = (client->port[0] != '\0');
    if (has_custom_port) {
        strncpy(custom_port, client->port, sizeof(custom_port) - 1);
    }

    // 2. Parse the URL.
    // This will fill client->port with either the URL's port (:5959)
    // or the scheme default (80/443).
    if (parse_url(client->url, client->hostname, client->port, client->path) !=
        0) {
        if (client->callback != NULL) {
            client->callback("ERROR", "Invalid URL", client->context);
        }
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    // 3. OVERRIDE: If the user provided a port as a function argument,
    // it must overwrite whatever parse_url just did.
    if (has_custom_port) {
        strncpy(client->port, custom_port, sizeof(client->port) - 1);
        client->port[sizeof(client->port) - 1] = '\0';
    }

    // 4. Scheme Detection
    client->is_https = (strncmp(client->url, "https://", 8) == 0);

    // Debug log to verify the final decision
    printf("[HTTP_CLIENT] Final Connection Target: %s://%s:%s%s\n",
           client->is_https ? "https" : "http", client->hostname, client->port,
           client->path);

    return HTTP_CLIENT_STATE_CONNECT;
}

HttpClientState http_client_work_connect(HttpClient* client) {
    if (client->is_https) {
        /* ---------- HTTPS (unchanged logic) ---------- */

        TLSClient* tls_client = malloc(sizeof(TLSClient));
        if (!tls_client) {
            client->callback("ERROR", "Memory allocation failed",
                             client->context);
            return HTTP_CLIENT_STATE_DISPOSE;
        }

        if (tls_client_init(tls_client) != 0) {
            client->callback("ERROR", "TLS initialization failed",
                             client->context);
            free(tls_client);
            return HTTP_CLIENT_STATE_DISPOSE;
        }

        const char* ca_path = client->ca_cert_path[0] ? client->ca_cert_path
                                                      : find_ca_cert_path();

        if (!ca_path ||
            tls_client_load_ca_cert_file(tls_client, ca_path) != 0) {
            client->callback("ERROR", "Failed to load CA certificates",
                             client->context);
            tls_client_dispose(tls_client);
            free(tls_client);
            return HTTP_CLIENT_STATE_DISPOSE;
        }

        int r = tls_client_connect(tls_client, client->hostname, client->port);

        if (r < 0) {
            client->callback("ERROR", "Failed to initiate TLS connection",
                             client->context);
            tls_client_dispose(tls_client);
            free(tls_client);
            return HTTP_CLIENT_STATE_DISPOSE;
        }

        client->tls_conn = tls_client;

        return (r == 1) ? HTTP_CLIENT_STATE_TLS_HANDSHAKE
                        : HTTP_CLIENT_STATE_WRITING;
    }

    /* ---------- HTTP (FIXED) ---------- */

    TCPClient* tcp_client = malloc(sizeof(TCPClient));
    if (!tcp_client) {
        client->callback("ERROR", "Memory allocation failed", client->context);
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    tcp_client->fd = -1;

    int r = tcp_client_connect(tcp_client, client->hostname, client->port);

    if (r < 0) {
        client->callback("ERROR", "Failed to initiate connection",
                         client->context);
        free(tcp_client);
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    client->tcp_conn = tcp_client;

    // IMPORTANT: distinguish states
    if (r == 1) {
        return HTTP_CLIENT_STATE_CONNECTING; // EINPROGRESS
    }

    return HTTP_CLIENT_STATE_WRITING; // connected immediately
}

HttpClientState http_client_work_connecting(HttpClient* client) {
    // This state is only for plain HTTP (TCP)
    if (client->is_https) {
        return HTTP_CLIENT_STATE_DISPOSE; // Should not reach here
    }

    if (client->tcp_conn == NULL || client->tcp_conn->fd < 0) {
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    int fd = client->tcp_conn->fd;

    // Check if connection has completed
    int       error = 0;
    socklen_t len   = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    if (error == 0) {
        // Connection successful!
        return HTTP_CLIENT_STATE_WRITING;
    } else if (error == EINPROGRESS || error == EALREADY) {
        // Still connecting, try again next tick
        return HTTP_CLIENT_STATE_CONNECTING;
    } else {
        // Connection failed
        if (client->callback != NULL) {
            client->callback("ERROR", "Connection failed", client->context);
        }
        return HTTP_CLIENT_STATE_DISPOSE;
    }
}

HttpClientState http_client_work_tls_handshake(HttpClient* client) {
    // This state is only for HTTPS (TLS)
    if (!client->is_https || client->tls_conn == NULL) {
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    int result = tls_client_handshake(client->tls_conn);

    if (result == 0) {
        // Handshake completed successfully
        printf("[HTTP_CLIENT] TLS handshake completed\n");
        return HTTP_CLIENT_STATE_WRITING;
    } else if (result == 1) {
        // Still in progress, try again next tick
        return HTTP_CLIENT_STATE_TLS_HANDSHAKE;
    } else {
        // Handshake failed
        if (client->callback != NULL) {
            client->callback("ERROR", "TLS handshake failed", client->context);
        }
        return HTTP_CLIENT_STATE_DISPOSE;
    }
}

HttpClientState http_client_work_writing(HttpClient* client) {
    if (client->write_buffer == NULL) {
        client->write_buffer = malloc(2048);
        if (client->write_buffer == NULL) {
            if (client->callback != NULL) {
                client->callback("ERROR", "Memory allocation failed",
                                 client->context);
            }
            return HTTP_CLIENT_STATE_DISPOSE;
        }

        // browser-like User-Agent and headers
        int len = snprintf(
            (char*)client->write_buffer, 2048,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
            "Accept: application/json, text/html, application/xml, */*\r\n"
            "Accept-Language: en-US,en;q=0.9\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n"
            "\r\n",
            client->path, client->hostname);

        client->write_size   = len;
        client->write_offset = 0;
    }

    // Send data (works for both TCP and TLS)
    ssize_t sent;
    if (client->is_https) {
        sent = tls_client_write(client->tls_conn,
                                client->write_buffer + client->write_offset,
                                client->write_size - client->write_offset);
    } else {
        sent = tcp_client_write(client->tcp_conn,
                                client->write_buffer + client->write_offset,
                                client->write_size - client->write_offset);
    }

    if (sent < 0) {
        if (client->callback != NULL) {
            client->callback("ERROR", "Send failed", client->context);
        }
        return HTTP_CLIENT_STATE_DISPOSE;
    } else if (sent == 0) {
        // Would block, try again later
        return HTTP_CLIENT_STATE_WRITING;
    }

    client->write_offset += sent;

    if (client->write_offset >= client->write_size) {
        free(client->write_buffer);
        client->write_buffer = NULL;
        return HTTP_CLIENT_STATE_READING;
    }

    return HTTP_CLIENT_STATE_WRITING;
}

HttpClientState http_client_work_reading(HttpClient* client) {
    if (!client) {
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    uint8_t chunk_buffer[CHUNK_SIZE];

    // Read data (works for both TCP and TLS)
    int bytes_read;
    if (client->is_https) {
        bytes_read = tls_client_read(client->tls_conn, chunk_buffer,
                                     sizeof(chunk_buffer));
    } else {
        bytes_read = tcp_client_read(client->tcp_conn, chunk_buffer,
                                     sizeof(chunk_buffer));
    }

    if (bytes_read == -2) {
        /* EOF from peer: treat as end-of-stream */
        if (client->body_start > 0) {
            size_t remaining =
                client->read_buffer_size > client->body_start
                    ? client->read_buffer_size - client->body_start
                    : 0;

            if (client->chunked) {
                /* decode chunked body */
                char*  decoded = NULL;
                size_t dec_len = 0;
                int    rc =
                    decode_chunked(client->read_buffer + client->body_start,
                                   remaining, &decoded, &dec_len);
                if (rc != 0) {
                    if (client->callback) {
                        client->callback("ERROR", "Chunked decode failed",
                                         client->context);
                    }
                    return HTTP_CLIENT_STATE_DISPOSE;
                }

                client->body        = (uint8_t*)decoded;
                client->content_len = dec_len;
                return HTTP_CLIENT_STATE_DONE;
            } else {
                client->content_len = remaining;
                if (client->content_len > 0) {
                    client->body = malloc(client->content_len + 1);
                    if (client->body) {
                        memcpy(client->body,
                               client->read_buffer + client->body_start,
                               client->content_len);
                        client->body[client->content_len] = '\0';
                    }
                }
                return HTTP_CLIENT_STATE_DONE;
            }
        }

        /* No headers parsed yet but connection closed - nothing to do */
        return HTTP_CLIENT_STATE_DISPOSE;
    } else if (bytes_read < 0) {
        if (client->callback) {
            client->callback("ERROR", "Read failed", client->context);
        }
        return HTTP_CLIENT_STATE_DISPOSE;
    } else if (bytes_read == 0) {
        /* No data available right now (non-blocking). Try again later. */
        return HTTP_CLIENT_STATE_READING;
    }

    // Grow buffer
    size_t   new_size   = client->read_buffer_size + bytes_read;
    uint8_t* new_buffer = realloc(client->read_buffer, new_size);
    if (!new_buffer) {
        if (client->callback) {
            client->callback("ERROR", "Memory allocation failed",
                             client->context);
        }
        return HTTP_CLIENT_STATE_DISPOSE;
    }

    client->read_buffer = new_buffer;
    memcpy(client->read_buffer + client->read_buffer_size, chunk_buffer,
           bytes_read);
    client->read_buffer_size += bytes_read;

    // Parse headers if not done yet
    if (client->body_start == 0) {
        for (int i = 0; i <= client->read_buffer_size - 4; i++) {
            if (client->read_buffer[i] == '\r' &&
                client->read_buffer[i + 1] == '\n' &&
                client->read_buffer[i + 2] == '\r' &&
                client->read_buffer[i + 3] == '\n') {

                int   header_end = i + 4;
                char* headers    = malloc(header_end + 1);
                if (!headers) {
                    if (client->callback) {
                        client->callback("ERROR", "Memory allocation failed",
                                         client->context);
                    }
                    return HTTP_CLIENT_STATE_DISPOSE;
                }

                memcpy(headers, client->read_buffer, header_end);
                headers[header_end] = '\0';

                // Parse status code
                int  status_code     = 0;
                char status_text[64] = {0};
                sscanf(headers, "HTTP/1.%*d %d %63[^\r\n]", &status_code,
                       status_text);

                // Parse Content-Length
                size_t content_len     = 0;
                char*  content_len_ptr = strstr(headers, "Content-Length:");
                if (content_len_ptr) {
                    sscanf(content_len_ptr, "Content-Length: %zu",
                           &content_len);
                }

                // Detect Transfer-Encoding: chunked
                client->chunked =
                    (strstr(headers, "Transfer-Encoding: chunked") != NULL);
                // Detect explicit Connection: close
                client->connection_close =
                    (strstr(headers, "Connection: close") != NULL);

                /* headers parsed */
                free(headers);

                client->status_code = status_code;
                client->content_len =
                    content_len; // 0 means unknown when not present
                client->body_start = header_end;

                break;
            }
        }
    }

    // Check if we have complete response
    if (client->body_start > 0) {
        if (client->content_len > 0) {
            // Known content length -> wait until we have full body
            if (client->read_buffer_size >=
                client->body_start + client->content_len) {
                client->body = malloc(client->content_len + 1);
                if (client->body) {
                    memcpy(client->body,
                           client->read_buffer + client->body_start,
                           client->content_len);
                    client->body[client->content_len] = '\0';
                }
                return HTTP_CLIENT_STATE_DONE;
            }
        } else if (client->chunked) {
            // Check for terminating chunk sequence "0\r\n\r\n" in buffer
            const char   TERM[]   = "0\r\n\r\n";
            const size_t TERM_LEN = 5;
            size_t       found    = SIZE_MAX;
            for (size_t j = client->body_start;
                 j + TERM_LEN <= client->read_buffer_size; j++) {
                if (memcmp(client->read_buffer + j, TERM, TERM_LEN) == 0) {
                    found = j;
                    break;
                }
            }

            if (found != SIZE_MAX) {
                size_t total_len = (found + TERM_LEN) - client->body_start;
                /* decode chunked data present in buffer */
                char*  decoded = NULL;
                size_t dec_len = 0;
                int    rc =
                    decode_chunked(client->read_buffer + client->body_start,
                                   total_len, &decoded, &dec_len);
                if (rc != 0) {
                    if (client->callback) {
                        client->callback("ERROR", "Chunked decode failed",
                                         client->context);
                    }
                    return HTTP_CLIENT_STATE_DISPOSE;
                }
                client->body        = (uint8_t*)decoded;
                client->content_len = dec_len;
                return HTTP_CLIENT_STATE_DONE;
            }

            return HTTP_CLIENT_STATE_READING;
        } else {
            // No content-length and not chunked -> assume server will close
            // connection. For HTTPS, rely on close_notify. For HTTP, peek.
            if (client->is_https) {
                // TLS will return -2 when close_notify is received
                return HTTP_CLIENT_STATE_READING;
            } else {
                // Use MSG_PEEK for HTTP
                int fd = client->tcp_conn ? client->tcp_conn->fd : -1;
                if (fd >= 0) {
                    uint8_t peekbuf[1];
                    ssize_t p = recv(fd, peekbuf, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (p == 0) {
                        // EOF detected: finalize body
                        client->content_len =
                            client->read_buffer_size > client->body_start
                                ? client->read_buffer_size - client->body_start
                                : 0;
                        if (client->content_len > 0) {
                            client->body = malloc(client->content_len + 1);
                            if (client->body) {
                                memcpy(client->body,
                                       client->read_buffer + client->body_start,
                                       client->content_len);
                                client->body[client->content_len] = '\0';
                            }
                        }
                        return HTTP_CLIENT_STATE_DONE;
                    } else if (p < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            return HTTP_CLIENT_STATE_READING;
                        } else {
                            if (client->callback) {
                                client->callback("ERROR", "Peek failed",
                                                 client->context);
                            }
                            return HTTP_CLIENT_STATE_DISPOSE;
                        }
                    }
                }
            }
        }
    }

    return HTTP_CLIENT_STATE_READING;
}

HttpClientState http_client_work_done(HttpClient* client) {

    if (client->callback != NULL) {
        if (client->status_code >= 200 && client->status_code < 300) {
            client->callback("RESPONSE",
                             client->body ? (char*)client->body : "",
                             client->context);
        } else {
            char error_info[256];
            snprintf(error_info, sizeof(error_info), "HTTP %d: %s",
                     client->status_code,
                     client->body ? (char*)client->body : "");

            client->callback("ERROR", error_info, client->context);
        }
    }

    return HTTP_CLIENT_STATE_DISPOSE;
}

void http_client_work(void* context, uint64_t mon_time) {
    HttpClient* client = (HttpClient*)context;

    if (!client) {
        return;
    }

    if (client->timer == 0) {
        client->timer = mon_time;
    } else if (mon_time >= client->timer + client->timeout) {
        if (client->callback != NULL) {
            client->callback("TIMEOUT", NULL, client->context);
        }

        // Mark as disposed
        HttpClient* tmp = client;
        client          = NULL; // prevent any further use in this function
        http_client_dispose(&tmp);
        return;
    }

    if (!client) {
        return;
    }

    switch (client->state) {
    case HTTP_CLIENT_STATE_INIT:
        client->state = http_client_work_init(client);
        break;

    case HTTP_CLIENT_STATE_CONNECT:
        client->state = http_client_work_connect(client);
        break;

    case HTTP_CLIENT_STATE_CONNECTING:
        client->state = http_client_work_connecting(client);
        break;

    case HTTP_CLIENT_STATE_TLS_HANDSHAKE:
        client->state = http_client_work_tls_handshake(client);
        break;

    case HTTP_CLIENT_STATE_WRITING:
        client->state = http_client_work_writing(client);
        break;

    case HTTP_CLIENT_STATE_READING:
        client->state = http_client_work_reading(client);
        break;

    case HTTP_CLIENT_STATE_DONE:
        client->state = http_client_work_done(client);
        break;

    case HTTP_CLIENT_STATE_DISPOSE:
        // Only dispose if not NULL
        if (client) {
            HttpClient* tmp = client;
            client          = NULL;
            http_client_dispose(&tmp);
        }
        break;
    }
}

void http_client_dispose(HttpClient** client_ptr) {
    if (!client_ptr || !*client_ptr) {
        return;
    }

    HttpClient* client = *client_ptr;

    if (client->task) {
        smw_destroy_task(client->task);
        client->task = NULL;
    }

    if (client->is_https && client->tls_conn) {
        tls_client_dispose(client->tls_conn);
        free(client->tls_conn);
        client->tls_conn = NULL;
    } else if (!client->is_https && client->tcp_conn) {
        tcp_client_disconnect(client->tcp_conn);
        free(client->tcp_conn);
        client->tcp_conn = NULL;
    }

    free(client->read_buffer);
    free(client->body);
    free(client->write_buffer);

    free(client);

    *client_ptr = NULL;
}

/**
 * @brief Parse an HTTP or HTTPS URL.
 *
 * Extracts:
 *   - Hostname
 *   - Port (explicit or inferred)
 *   - Path
 *
 * Examples:
 *   - http://example.com → host=example.com, port=80, path=/
 *   - https://example.com:8443/api → host=example.com, port=8443, path=/api
 *
 * @param url      Input URL.
 * @param hostname Output buffer for hostname.
 * @param port     Output buffer for port.
 * @param path     Output buffer for path.
 *
 * @return 0 on success, non-zero on failure.
 */
int parse_url(const char* url, char* hostname, char* port_str, char* path) {
    if (url == NULL || hostname == NULL || port_str == NULL || path == NULL) {
        return -1;
    }

    // 1. Initial Defaults
    char        default_port[6] = "80";
    const char* start           = url;

    // 2. Detect Scheme and set appropriate default port
    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
        strcpy(default_port, "80");
    } else if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
        strcpy(default_port, "443");
    }

    // 3. Find end of hostname (stops at ':', '/', or end of string)
    const char* end = start;
    while (*end && *end != ':' && *end != '/') {
        end++;
    }

    int hostname_len = (int)(end - start);
    if (hostname_len <= 0 || hostname_len > 255) {
        return -1; // Invalid hostname
    }

    memcpy(hostname, start, hostname_len);
    hostname[hostname_len] = '\0';

    // 4. Handle Port
    if (*end == ':') {
        end++; // Skip ':'
        const char* port_start = end;
        while (*end && *end != '/') {
            end++;
        }
        int port_len = (int)(end - port_start);
        if (port_len > 0 && port_len < 6) {
            memcpy(port_str, port_start, port_len);
            port_str[port_len] = '\0';
        } else {
            // Found ':' but no valid digits (e.g., "example.com:/")
            // Fallback to scheme default
            strcpy(port_str, default_port);
        }
    } else {
        // No ':' found, use the default port determined by the scheme
        strcpy(port_str, default_port);
    }

    // 5. Handle Path
    if (*end == '/') {
        // Copy the rest of the string as the path
        strncpy(path, end, 511);
        path[511] = '\0';
    } else {
        // No path provided (end of string reached), default to "/"
        strcpy(path, "/");
    }

    return 0;
}
