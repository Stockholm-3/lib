#include "file_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int file_lock_acquire_exclusive(const char* path) {
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        {
            return -1;
        }
    }
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void file_lock_release(int fd) {
    if (fd < 0)
        return;
    flock(fd, LOCK_UN);
    close(fd);
}

int file_lock_try_shared(const char* path) {
    int fd = open(path, O_CREAT | O_RDONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
