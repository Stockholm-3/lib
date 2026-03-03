/**
 * @file smw_file_reader.h
 * @brief Generic async non-blocking file reader for SMW tasks.
 *
 * Acquires a shared flock on a guard file, reads the target file in
 * chunks, fires a callback with the heap-allocated NUL-terminated
 * contents, then self-destructs.
 *
 * Usage
 * -----
 *   smw_file_reader_start(lock_path, file_path, context, on_done);
 *
 * The callback owns the buffer and must free() it on SFR_OK.
 * On all error statuses buf == NULL and len == 0.
 */
#ifndef SMW_FILE_READER_H
#define SMW_FILE_READER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SFR_OK,         /* buf/len valid — caller must free(buf) */
    SFR_NOT_FOUND,  /* file does not exist                   */
    SFR_LOCK_ERROR, /* could not acquire shared lock         */
    SFR_READ_ERROR, /* file exists but read failed           */
} SfrStatus;

typedef void (*SfrOnDone)(void* context, SfrStatus status, char* buf,
                          size_t len);

/**
 * Create an SMW task that reads @p file_path while holding a shared
 * flock on @p lock_path.
 *
 * @return 0 on success (task created), -1 on failure.
 */
int smw_file_reader_start(const char* lock_path, const char* file_path,
                          void* context, SfrOnDone on_done);

/* SMW work function — register this with smw_create_task(). */
void smw_file_reader_work(void* context, uint64_t mon_time);

#endif // SMW_FILE_READER_H
