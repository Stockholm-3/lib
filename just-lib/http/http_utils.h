#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <http_server_connection.h>
#include <stddef.h>

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
                  const char* content_type, const char* body, size_t body_len);

int send_json_message(HTTPServerConnection* conn, int status,
                      const char* message);

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
                          size_t path_len, char* query_out, size_t query_len);

#endif
