/**
 * @file file_lock.h
 * @brief Thin wrappers around POSIX flock.
 *
 * Blocking variants are safe to call from worker threads.
 * Non-blocking variants return -1 / set errno = EWOULDBLOCK when busy,
 * so they can be polled from SMW task work functions.
 */
#ifndef FILE_LOCK_H
#define FILE_LOCK_H

/**
 * Open @p path and acquire an exclusive (write) lock, blocking until
 * all shared holders release.
 *
 * @return fd >= 0 (the lock handle) on success, -1 on failure.
 */
int file_lock_acquire_exclusive(const char* path);

/**
 * Release a lock acquired by file_lock_acquire_exclusive().
 * Unlocks and closes the fd. No-op if @p fd < 0.
 */
void file_lock_release(int fd);

/**
 * Open @p path (creating it if necessary) and attempt a non-blocking
 * shared (read) lock.
 *
 * @return fd >= 0 on success.
 *         -1 with errno == EWOULDBLOCK if the exclusive lock is held.
 *         -1 with another errno on hard error.
 */
int file_lock_try_shared(const char* path);

#endif // FILE_LOCK_H
