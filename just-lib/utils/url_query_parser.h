/**
 * @file url_query_parser.h
 * @brief URL query string parser.
 *
 * Parses URL query strings into key-value pairs
 * with proper URL decoding.
 */

#ifndef URL_QUERY_PARSER_H
#define URL_QUERY_PARSER_H

#include <stddef.h>

/**
 * @def URL_QUERY_PARSER_MAX_PARAMS
 * @brief Maximum number of query parameters.
 */
#ifndef URL_QUERY_PARSER_MAX_PARAMS
#    define URL_QUERY_PARSER_MAX_PARAMS 64
#endif

/**
 * @def URL_QUERY_PARSER_MAX_KEY
 * @brief Maximum key length (including null terminator).
 */
#ifndef URL_QUERY_PARSER_MAX_KEY
#    define URL_QUERY_PARSER_MAX_KEY 64
#endif

/**
 * @def URL_QUERY_PARSER_MAX_VALUE
 * @brief Maximum value length (including null terminator).
 */
#ifndef URL_QUERY_PARSER_MAX_VALUE
#    define URL_QUERY_PARSER_MAX_VALUE 256
#endif

/**
 * @struct url_query_param
 * @brief Represents one URL query parameter.
 */
typedef struct {
    char key[URL_QUERY_PARSER_MAX_KEY];
    char value[URL_QUERY_PARSER_MAX_VALUE];
} UrlQueryParam;

/**
 * @struct url_query_map
 * @brief Holds parsed URL query parameters.
 */
typedef struct {
    UrlQueryParam params[URL_QUERY_PARSER_MAX_PARAMS];
    size_t        count;
} UrlQueryMap;

/**
 * @brief Initialize a query map.
 *
 * Must be called before parsing.
 *
 * @param map Pointer to query map.
 */
void url_query_init(UrlQueryMap* map);

/**
 * @brief Parse a URL query string.
 *
 * Example:
 *   "?name=John&age=25"
 *
 * @param query Query string (may start with '?').
 * @param map   Output parameter map.
 *
 * @return 0 on success, -1 on error.
 */
int url_query_parse(const char* query, UrlQueryMap* map);

/**
 * @brief Get parameter value by key.
 *
 * @param map Query map.
 * @param key Parameter name.
 *
 * @return Value string or NULL if not found.
 */
const char* url_query_get(const UrlQueryMap* map, const char* key);

/**
 * @brief Get number of parsed parameters.
 *
 * @param map Query map.
 *
 * @return Parameter count.
 */
size_t url_query_count(const UrlQueryMap* map);

/**
 * @brief Get parameter by index.
 *
 * @param map   Query map.
 * @param index Parameter index.
 *
 * @return Parameter pointer or NULL.
 */
const UrlQueryParam* url_query_get_at(const UrlQueryMap* map, size_t index);

#endif /* URL_QUERY_PARSER_H */
