#include "url_query_parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int url_decode(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    while (*src) {
        if (i + 1 >= dst_size) {
            return -1;
        }

        if (*src == '%' && isxdigit((unsigned char)src[1]) &&
            isxdigit((unsigned char)src[2])) {

            int hi = hex_to_int(src[1]);
            int lo = hex_to_int(src[2]);

            if (hi < 0 || lo < 0) {
                return -1;
            }

            dst[i++] = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }

    dst[i] = '\0';
    return 0;
}

void url_query_init(UrlQueryMap* map) {
    if (!map) {
        return;
    }

    memset(map, 0, sizeof(*map));
}

int url_query_parse(const char* query, UrlQueryMap* map) {
    if (!query || !map) {
        return -1;
    }

    url_query_init(map);

    if (*query == '?') {
        query++;
    }

    if (*query == '\0') {
        return 0;
    }

    char buffer[1024];

    if (strlen(query) >= sizeof(buffer)) {
        return -1;
    }

    strcpy(buffer, query);

    char* saveptr = NULL;
    char* pair    = strtok_r(buffer, "&", &saveptr);

    while (pair && map->count < URL_QUERY_PARSER_MAX_PARAMS) {

        char* eq = strchr(pair, '=');

        char* key = pair;
        char* val = "";

        if (eq) {
            *eq = '\0';
            val = eq + 1;
        }

        UrlQueryParam* p = &map->params[map->count];

        if (url_decode(p->key, sizeof(p->key), key) != 0) {
            return -1;
        }

        if (url_decode(p->value, sizeof(p->value), val) != 0) {
            return -1;
        }

        map->count++;

        pair = strtok_r(NULL, "&", &saveptr);
    }

    return 0;
}

const char* url_query_get(const UrlQueryMap* map, const char* key) {
    if (!map || !key) {
        return NULL;
    }

    for (size_t i = 0; i < map->count; i++) {
        if (strcmp(map->params[i].key, key) == 0) {
            return map->params[i].value;
        }
    }

    return NULL;
}

size_t url_query_count(const UrlQueryMap* map) {
    if (!map) {
        return 0;
    }

    return map->count;
}

const UrlQueryParam* url_query_get_at(const UrlQueryMap* map, size_t index) {
    if (!map) {
        return NULL;
    }

    if (index >= map->count) {
        return NULL;
    }

    return &map->params[index];
}
