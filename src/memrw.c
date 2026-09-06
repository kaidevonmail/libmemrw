/*
 * Copyright 2026 kaidev <kaidevonmail@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include "memrw.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <pthread.h>

/* Internal constants */
#define MAX_PROCESSES 256
#define HASH_SIZE     521   /* prime number > MAX_PROCESSES */

struct memrw_context {
    struct {
        pid_t pid;
        int pagemap_fd;
        int mem_fd;
    } process[MAX_PROCESSES];
    int process_count;
    int pid_hash[HASH_SIZE];
};

/* Global context protected by mutex */
static struct memrw_context memrw_ctx;
static pthread_mutex_t memrw_mutex = PTHREAD_MUTEX_INITIALIZER;

static int memrw_hash_find(struct memrw_context *ctx, pid_t pid) {
    int hash = pid % HASH_SIZE;
    int i = hash;
    do {
        int val = ctx->pid_hash[i];
        if (val == -1) return -1;               /* empty, not found */
        if (val != -2) {                        /* -2 is tombstone */
            if (ctx->process[val].pid == pid) {
                return val;
            }
        }
        i = (i + 1) % HASH_SIZE;
    } while (i != hash);
    return -1;
}

static int memrw_hash_insert(struct memrw_context *ctx, pid_t pid, int index) {
    if (memrw_hash_find(ctx, pid) != -1) {
        errno = EEXIST;
        return -1;
    }

    int hash = pid % HASH_SIZE;
    int i = hash;
    while (ctx->pid_hash[i] != -1 && ctx->pid_hash[i] != -2) {
        i = (i + 1) % HASH_SIZE;
        if (i == hash) {
            errno = ENOSPC;
            return -1;
        }
    }

    ctx->pid_hash[i] = index;
    return 0;
}

static int memrw_hash_remove(struct memrw_context *ctx, pid_t pid) {
    int hash = pid % HASH_SIZE;
    int i = hash;
    do {
        int val = ctx->pid_hash[i];
        if (val == -1) {
            return -1;                          /* not found */
        }
        if (val != -2 && ctx->process[val].pid == pid) {
            ctx->pid_hash[i] = -2;              /* mark as deleted */
            return 0;
        }
        i = (i + 1) % HASH_SIZE;
    } while (i != hash);
    return -1;
}

static int find_free_slot(struct memrw_context *ctx) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (ctx->process[i].pid == 0) {
            return i;
        }
    }
    return -1;
}

/* Check if a virtual address is backed by a physical page.
 * Assumes mutex is held and process is already tracked. */
static bool check_pfn_locked(int idx, uintptr_t addr) {
    int fd = memrw_ctx.process[idx].pagemap_fd;
    if (fd < 0) {
        errno = EBADF;
        return false;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;  /* fallback */
    }

    uint64_t page_num = (uint64_t)addr / (uint64_t)page_size;
    off_t offset = (off_t)(page_num * 8);

    uint64_t entry = 0;
    ssize_t n = pread(fd, &entry, sizeof(entry), offset);
    if (n != sizeof(entry)) {
        return false;
    }

    /* Present bit is bit 63 */
    return ((entry >> 63) & 0x1ULL) == 1;
}

void memrw_init(void) {
    pthread_mutex_lock(&memrw_mutex);

    memset(&memrw_ctx, 0, sizeof(memrw_ctx));
    for (int i = 0; i < MAX_PROCESSES; i++) {
        memrw_ctx.process[i].pid = 0;          /* 0 means free slot */
        memrw_ctx.process[i].pagemap_fd = -1;
        memrw_ctx.process[i].mem_fd = -1;
    }
    for (int i = 0; i < HASH_SIZE; i++) {
        memrw_ctx.pid_hash[i] = -1;            /* empty slot */
    }
    memrw_ctx.process_count = 0;

    pthread_mutex_unlock(&memrw_mutex);
}

int memrw_open_process(pid_t pid) {
    pthread_mutex_lock(&memrw_mutex);

    /* Check if pid is already tracked */
    if (memrw_hash_find(&memrw_ctx, pid) != -1) {
        errno = EEXIST;
        pthread_mutex_unlock(&memrw_mutex);
        return -1;
    }

    /* Open pagemap and mem files */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
    int pagemap_fd = open(path, O_RDONLY);
    if (pagemap_fd < 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&memrw_mutex);
        errno = saved_errno;
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int mem_fd = open(path, O_RDWR);
    if (mem_fd < 0) {
        int saved_errno = errno;
        close(pagemap_fd);
        pthread_mutex_unlock(&memrw_mutex);
        errno = saved_errno;
        return -1;
    }

    /* Find a free slot */
    int idx = find_free_slot(&memrw_ctx);
    if (idx == -1) {
        close(pagemap_fd);
        close(mem_fd);
        pthread_mutex_unlock(&memrw_mutex);
        errno = ENOSPC;
        return -1;
    }

    /* Fill process info */
    memrw_ctx.process[idx].pid = pid;
    memrw_ctx.process[idx].pagemap_fd = pagemap_fd;
    memrw_ctx.process[idx].mem_fd = mem_fd;

    /* Insert into hash table */
    if (memrw_hash_insert(&memrw_ctx, pid, idx) != 0) {
        /* Insert failed, rollback */
        int saved_errno = errno;
        close(pagemap_fd);
        close(mem_fd);
        memrw_ctx.process[idx].pid = 0;
        memrw_ctx.process[idx].pagemap_fd = -1;
        memrw_ctx.process[idx].mem_fd = -1;
        pthread_mutex_unlock(&memrw_mutex);
        errno = saved_errno;
        return -1;
    }

    memrw_ctx.process_count++;
    pthread_mutex_unlock(&memrw_mutex);
    return 0;
}

int memrw_close_process(pid_t pid) {
    pthread_mutex_lock(&memrw_mutex);

    int idx = memrw_hash_find(&memrw_ctx, pid);
    if (idx == -1) {
        pthread_mutex_unlock(&memrw_mutex);
        errno = ESRCH;
        return -1;
    }

    /* Close file descriptors */
    if (memrw_ctx.process[idx].pagemap_fd >= 0) {
        close(memrw_ctx.process[idx].pagemap_fd);
        memrw_ctx.process[idx].pagemap_fd = -1;
    }
    if (memrw_ctx.process[idx].mem_fd >= 0) {
        close(memrw_ctx.process[idx].mem_fd);
        memrw_ctx.process[idx].mem_fd = -1;
    }

    /* Remove from hash table */
    if (memrw_hash_remove(&memrw_ctx, pid) != 0) {
        /* Should never happen because we found it above */
        pthread_mutex_unlock(&memrw_mutex);
        errno = EIO;
        return -1;
    }

    /* Mark slot as free */
    memrw_ctx.process[idx].pid = 0;
    memrw_ctx.process_count--;

    pthread_mutex_unlock(&memrw_mutex);
    return 0;
}

int memrw_process_should_close(pid_t pid) {
    pthread_mutex_lock(&memrw_mutex);

    /* First check if pid is even tracked */
    if (memrw_hash_find(&memrw_ctx, pid) == -1) {
        pthread_mutex_unlock(&memrw_mutex);
        return 1;   /* not tracked, close? caller should handle */
    }

    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d", pid);
    int ret = stat(path, &st);
    pthread_mutex_unlock(&memrw_mutex);

    if (ret == -1) {
        /* Only treat ENOENT as process gone; other errors (permission etc.)
           should not force closure */
        return (errno == ENOENT) ? 1 : 0;
    }
    return 0;
}

/* Common validation for memrv/memwv */
static int memrw_prepare_rw(pid_t pid, const struct iovec *remote_iov,
                            unsigned long riovcnt, unsigned long flags,
                            int *idx_out) {
    if (flags != 0) {
        errno = EINVAL;
        return -1;
    }

    int idx = memrw_hash_find(&memrw_ctx, pid);
    if (idx == -1) {
        errno = ESRCH;
        return -1;
    }

    /* Check physical presence for each remote iovec (first and last page) */
    for (unsigned long i = 0; i < riovcnt; i++) {
        uintptr_t base = (uintptr_t)remote_iov[i].iov_base;
        size_t len = remote_iov[i].iov_len;
        if (len == 0)
            continue;

        /* Check first page */
        if (!check_pfn_locked(idx, base)) {
            int saved = errno;
            if (saved == 0)
                saved = EFAULT;
            errno = saved;
            return -1;
        }

        /* Check last page if it differs from first */
        uintptr_t end = base + len - 1;
        if ((end / sysconf(_SC_PAGESIZE)) != (base / sysconf(_SC_PAGESIZE))) {
            if (!check_pfn_locked(idx, end)) {
                int saved = errno;
                if (saved == 0)
                    saved = EFAULT;
                errno = saved;
                return -1;
            }
        }
    }

    *idx_out = idx;
    return 0;
}

ssize_t memrv(pid_t pid,
              const struct iovec *local_iov,
              unsigned long liovcnt,
              const struct iovec *remote_iov,
              unsigned long riovcnt,
              unsigned long flags) {
    pthread_mutex_lock(&memrw_mutex);

    int idx;
    if (memrw_prepare_rw(pid, remote_iov, riovcnt, flags, &idx) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&memrw_mutex);
        errno = saved_errno;
        return -1;
    }

    ssize_t ret = syscall(SYS_process_vm_readv, pid, local_iov, liovcnt,
                          remote_iov, riovcnt, flags);
    int saved_errno = errno;
    pthread_mutex_unlock(&memrw_mutex);
    errno = saved_errno;
    return ret;
}

ssize_t memwv(pid_t pid,
              const struct iovec *local_iov,
              unsigned long liovcnt,
              const struct iovec *remote_iov,
              unsigned long riovcnt,
              unsigned long flags) {
    pthread_mutex_lock(&memrw_mutex);

    int idx;
    if (memrw_prepare_rw(pid, remote_iov, riovcnt, flags, &idx) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&memrw_mutex);
        errno = saved_errno;
        return -1;
    }

    ssize_t ret = syscall(SYS_process_vm_writev, pid, local_iov, liovcnt,
                          remote_iov, riovcnt, flags);
    int saved_errno = errno;
    pthread_mutex_unlock(&memrw_mutex);
    errno = saved_errno;
    return ret;
}

int memr(pid_t pid, void *local_buf, void *remote_buf, size_t len) {
    struct iovec local_iov = {
        .iov_base = local_buf,
        .iov_len = len
    };
    struct iovec remote_iov = {
        .iov_base = remote_buf,
        .iov_len = len
    };

    ssize_t n = memrv(pid, &local_iov, 1, &remote_iov, 1, 0);
    if (n < 0) {
        return -1;
    }
    if ((size_t)n != len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int memw(pid_t pid, void *local_buf, void *remote_buf, size_t len) {
    struct iovec local_iov = {
        .iov_base = local_buf,
        .iov_len = len
    };
    struct iovec remote_iov = {
        .iov_base = remote_buf,
        .iov_len = len
    };

    ssize_t n = memwv(pid, &local_iov, 1, &remote_iov, 1, 0);
    if (n < 0) {
        return -1;
    }
    if ((size_t)n != len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int memwx(pid_t pid, void *local_buf, void *remote_buf, size_t len) {
    pthread_mutex_lock(&memrw_mutex);

    int idx = memrw_hash_find(&memrw_ctx, pid);
    if (idx == -1) {
        pthread_mutex_unlock(&memrw_mutex);
        errno = ESRCH;
        return -1;
    }

    int mem_fd = memrw_ctx.process[idx].mem_fd;
    if (mem_fd < 0) {
        pthread_mutex_unlock(&memrw_mutex);
        errno = EBADF;
        return -1;
    }

    ssize_t n = pwrite(mem_fd, local_buf, len, (off_t)(uintptr_t)remote_buf);
    int saved_errno = errno;
    pthread_mutex_unlock(&memrw_mutex);

    if (n < 0) {
        errno = saved_errno;
        return -1;
    }
    if ((size_t)n != len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

bool memrw_check_present(pid_t pid, void *addr) {
    pthread_mutex_lock(&memrw_mutex);

    int idx = memrw_hash_find(&memrw_ctx, pid);
    if (idx == -1) {
        pthread_mutex_unlock(&memrw_mutex);
        errno = ESRCH;
        return false;
    }

    bool present = check_pfn_locked(idx, (uintptr_t)addr);
    int saved_errno = errno;
    pthread_mutex_unlock(&memrw_mutex);
    errno = saved_errno;
    return present;
}