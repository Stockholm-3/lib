#include "fs_utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fs_mkdir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int fs_write_json_atomic(const char* path, json_t* data) {
    // Remove stale destination so readers never see partial content.
    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }

    char tmp[512 + 4];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -1;
    }

    if (json_dump_file(data, tmp, JSON_INDENT(2)) != 0) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void fs_to_lower(const char* src, char* dst, size_t dst_size) {
    size_t i = 0;
    for (; i + 1 < dst_size && src[i]; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}
