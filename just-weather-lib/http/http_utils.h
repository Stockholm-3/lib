#include "response_builder.h"

#include <http_server_connection.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Builds and assigns an HTTP response to a server connection.
 *
 * This function constructs a basic HTTP/1.1 response including status line,
 * Content-Type, CORS header, and Content-Length. The response body is appended
 * after the headers, and the resulting buffer is assigned to the connection's
 * write buffer.
 *
 * Memory for the response is dynamically allocated and ownership is transferred
 * to the HTTPServerConnection. The caller must ensure the buffer is freed after
 * it is sent.
 *
 * @param conn         Pointer to an active HTTPServerConnection.
 * @param status       HTTP status code (e.g., 200, 400, 500).
 * @param content_type MIME type of the response body.
 * @param body         Pointer to the response body data.
 * @param body_len     Length of the response body in bytes.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int send_response(HTTPServerConnection* conn, int status,
                  const char* content_type, const char* body, size_t body_len) {
    char header[256];
    int  header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 %d %s\r\n"
                                "Content-Type: %s\r\n"
                                "Access-Control-Allow-Origin: *\r\n"
                                "Content-Length: %zu\r\n\r\n",
                               status, status == 200 ? "OK" : "Error",
                               content_type, body_len);

    size_t   total_len = header_len + body_len;
    uint8_t* resp      = malloc(total_len + 1);
    if (!resp) {
        return -1;
    }

    memcpy(resp, header, header_len);
    memcpy(resp + header_len, body, body_len);

    conn->write_buffer = resp;
    conn->write_size   = total_len;

    return 0;
}

/**
 * @brief Sends a JSON-formatted error response.
 *
 * This function builds a standardized JSON error object using the response
 * builder utilities and sends it as an HTTP response with the given status
 * code.
 *
 * The generated JSON string is freed internally after the response buffer
 * has been constructed.
 *
 * @param conn   Pointer to an active HTTPServerConnection.
 * @param status HTTP error status code.
 * @param reason Human-readable explanation of the error.
 *
 * @return 0 on success, -1 on failure.
 */
int send_json_error(HTTPServerConnection* conn, int status,
                    const char* reason) {
    char* json = response_builder_error(
        status, response_builder_get_error_type(status), reason);
    if (!json)
        return -1;
    int ret =
        send_response(conn, status, "application/json", json, strlen(json));
    free(json);
    return ret;
}

/**
 * @brief Splits a request path into path and query components.
 *
 * Given a request path of the form "/path/to/resource?key=value",
 * this function separates the path and query string into independent
 * buffers. If no query string is present, the query output is set to
 * an empty string.
 *
 * Output strings are always null-terminated.
 *
 * @param request_path Input request path (must be null-terminated).
 * @param path_out     Output buffer for the path portion.
 * @param path_len     Size of the path output buffer.
 * @param query_out    Output buffer for the query portion.
 * @param query_len    Size of the query output buffer.
 */
void split_path_and_query(const char* request_path, char* path_out,
                          size_t path_len, char* query_out, size_t query_len) {
    char* question_mark = strchr(request_path, '?');
    if (question_mark) {
        size_t plen = question_mark - request_path;
        if (plen >= path_len)
            plen = path_len - 1;
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
