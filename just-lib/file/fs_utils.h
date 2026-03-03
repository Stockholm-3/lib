/**
 * @file fs_utils.h
 * @brief Small, reusable filesystem helpers.
 *
 * No global state. All functions are thread-safe.
 */
#ifndef FS_UTILS_H
#define FS_UTILS_H

#include <jansson.h>
#include <stddef.h>

/**
 * Create @p path as a directory if it does not already exist.
 * Succeeds if path already exists and is a directory.
 * @return 0 on success, -1 on failure.
 */
int fs_mkdir(const char* path);

/**
 * Write @p data as indented JSON to @p path atomically.
 *   1. unlink existing file
 *   2. write to <path>.tmp
 *   3. rename into place
 * @return 0 on success, -1 on failure.
 */
int fs_write_json_atomic(const char* path, json_t* data);

/**
 * Lower-case copy of @p src into @p dst.
 * Always NUL-terminates. Truncates silently if dst_size is too small.
 */
void fs_to_lower(const char* src, char* dst, size_t dst_size);

#endif // FS_UTILS_H
