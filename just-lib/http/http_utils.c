#include "http_utils.h"

#include "response_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int send_response(HTTPServerConnection* conn, int status,
                  const char* content_type, const char* body, size_t body_len) {
    if (!conn || !content_type || (!body && body_len > 0)) {
        return -1;
    }

    char header[256];
    int  header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 %d %s\r\n"
                                "Content-Type: %s\r\n"
                                "Access-Control-Allow-Origin: *\r\n"
                                "Content-Length: %zu\r\n\r\n",
                               status, status == 200 ? "OK" : "Error",
                               content_type, body_len);

    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        return -2; // header formatting error or overflow
    }

    size_t total_len = (size_t)header_len + body_len;

    uint8_t* resp = malloc(total_len);
    if (!resp) {
        return -3;
    }

    memcpy(resp, header, header_len);
    if (body_len > 0) {
        memcpy(resp + header_len, body, body_len);
    }

    /* NEW: hand off to the official async API */
    int rc = http_server_connection_respond(conn, resp, total_len);

    /* respond() copies the buffer, so we must free ours */
    free(resp);

    return rc;
}

int send_json_error(HTTPServerConnection* conn, int status,
                    const char* reason) {
    char* json = response_builder_error(
        status, response_builder_get_error_type(status), reason);
    if (!json) {
        return -1;
    }
    int ret =
        send_response(conn, status, "application/json", json, strlen(json));
    free(json);
    return ret;
}

void split_path_and_query(const char* request_path, char* path_out,
                          size_t path_len, char* query_out, size_t query_len) {
    char* question_mark = strchr(request_path, '?');
    if (question_mark) {
        size_t plen = question_mark - request_path;
        if (plen >= path_len) {
            plen = path_len - 1;
        }
        strncpy(path_out, request_path, plen);
        path_out[plen] = '\0';
        strncpy(query_out, question_mark + 1, query_len - 1);
        query_out[query_len - 1] = '\0';
    } else {
        strncpy(path_out, request_path, path_len - 1);
        path_out[path_len - 1] = '\0';
        query_out[0]           = '\0';
    }
}
