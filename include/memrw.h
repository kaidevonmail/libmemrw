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

#ifndef MEMRW_H
#define MEMRW_H

/**
 * @file memrw.h
 * @brief Cross-process memory read/write interface.
 *
 * This module provides functions to open a target process, read/write its
 * memory (via process_vm_readv/writev syscalls or /proc/pid/mem), and manage
 * its lifecycle. All public functions are thread-safe because they operate
 * on a global context protected by a mutex.
 */

#include <stdbool.h>
#include <sys/types.h>
#include <sys/uio.h>

/**
 * @brief Initialize the internal context. Must be called once at startup
 *        before any other function in this module.
 */
void memrw_init(void);

/**
 * @brief Open and start tracking a process for memory operations.
 * @param pid Target process ID.
 * @return 0 on success, -1 on error (errno is set).
 *
 * Opens /proc/pid/pagemap (O_RDONLY) and /proc/pid/mem (O_RDWR), stores
 * the file descriptors in the internal context, and inserts the PID into
 * the hash table. If the PID is already tracked, errno=EEXIST.
 */
int memrw_open_process(pid_t pid);

/**
 * @brief Close and stop tracking a process.
 * @param pid Target process ID.
 * @return 0 on success, -1 on error (errno is set).
 *
 * Closes the pagemap and mem file descriptors, removes the PID from the
 * hash table, and marks the slot as free. If PID is not tracked, errno=ESRCH.
 */
int memrw_close_process(pid_t pid);

/**
 * @brief Check whether a tracked process should be closed.
 * @param pid Target process ID.
 * @return 1 if the process no longer exists (should close), 0 otherwise.
 *
 * Uses existence of /proc/pid directory as indicator. A return value of 0
 * means the process still exists (may be zombie). Note: other errors such as
 * permission denied also result in 0 to avoid premature closure.
 */
int memrw_process_should_close(pid_t pid);

/**
 * @brief Read data from a remote process's memory using iovec arrays.
 * @param pid        Target process ID (must be tracked).
 * @param local_iov  Array of local destination buffers.
 * @param liovcnt    Number of elements in local_iov.
 * @param remote_iov Array of remote source addresses.
 * @param riovcnt    Number of elements in remote_iov.
 * @param flags      Must be 0.
 * @return Number of bytes read on success, -1 on error (errno is set).
 *
 * Before the syscall, the function checks that the process is tracked, that
 * flags is 0, and that the first and last page of each remote iovec is
 * present in physical memory. If checks fail, returns -1 with appropriate
 * errno (ESRCH, EINVAL, EFAULT, etc.).
 */
ssize_t memrv(pid_t pid,
              const struct iovec *local_iov,
              unsigned long liovcnt,
              const struct iovec *remote_iov,
              unsigned long riovcnt,
              unsigned long flags);

/**
 * @brief Write data to a remote process's memory using iovec arrays.
 * @param pid        Target process ID (must be tracked).
 * @param local_iov  Array of local source buffers.
 * @param liovcnt    Number of elements in local_iov.
 * @param remote_iov Array of remote destination addresses.
 * @param riovcnt    Number of elements in remote_iov.
 * @param flags      Must be 0.
 * @return Number of bytes written on success, -1 on error (errno is set).
 *
 * Similar to memrv but performs a write operation.
 */
ssize_t memwv(pid_t pid,
              const struct iovec *local_iov,
              unsigned long liovcnt,
              const struct iovec *remote_iov,
              unsigned long riovcnt,
              unsigned long flags);

/**
 * @brief Read a single contiguous block of memory from a remote process.
 * @param pid        Target process ID (must be tracked).
 * @param local_buf  Local buffer to receive data.
 * @param remote_buf Remote source address.
 * @param len        Number of bytes to read.
 * @return 0 on success, -1 on error (errno is set).
 *
 * Wrapper around memrv with a single-element iovec array. Returns error if
 * partial read occurs.
 */
int memr(pid_t pid, void *local_buf, void *remote_buf, size_t len);

/**
 * @brief Write a single contiguous block of memory to a remote process.
 * @param pid        Target process ID (must be tracked).
 * @param local_buf  Local buffer containing data to write.
 * @param remote_buf Remote destination address.
 * @param len        Number of bytes to write.
 * @return 0 on success, -1 on error (errno is set).
 *
 * Wrapper around memwv with a single-element iovec array. Returns error if
 * partial write occurs.
 */
int memw(pid_t pid, void *local_buf, void *remote_buf, size_t len);

/**
 * @brief Write memory directly via /proc/pid/mem, bypassing permission checks.
 * @param pid        Target process ID (must be tracked).
 * @param local_buf  Local buffer containing data to write.
 * @param remote_buf Remote destination address.
 * @param len        Number of bytes to write.
 * @return 0 on success, -1 on error (errno is set).
 *
 * This function uses pwrite on the already-open mem_fd. It is intended for
 * writing to memory regions that are normally not writable via process_vm_writev
 * (e.g., text segment). No physical page presence check is performed.
 */
int memwx(pid_t pid, void *local_buf, void *remote_buf, size_t len);

/**
 * @brief Check whether a virtual address is backed by a physical page.
 * @param pid  Target process ID (must be tracked).
 * @param addr Virtual address in the target process.
 * @return true if the page containing addr is present in physical memory,
 *         false otherwise (including errors, errno may be set).
 */
bool memrw_check_present(pid_t pid, void *addr);

#endif  /* MEMRW_H */