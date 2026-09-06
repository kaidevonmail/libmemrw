# memrw - 跨进程内存读写库

## 简介

`memrw` 是一个用于 Linux 的轻量级 C 库，提供跨进程内存读写功能。它允许你打开目标进程，读取或写入其内存，并管理进程跟踪状态。该库基于 `process_vm_readv` / `process_vm_writev` 系统调用和 `/proc/pid/mem` 文件接口，同时支持物理页存在性检查，适用于调试、监控或注入工具的开发。

## 功能特性

- 进程跟踪管理：通过哈希表高效记录已打开的进程。
- 内存读写：
  - 向量读写：支持 `iovec` 数组（`memrv` / `memwv`）。
  - 单块读写：简化接口（`memr` / `memw`）。
  - 直接写入：通过 `/proc/pid/mem` 绕过常规写保护（`memwx`）。
- 物理页检查：在执行读写前验证目标地址是否存在于物理内存中，避免触发缺页异常。
- 线程安全：所有公共函数使用互斥锁保护内部状态。
- 错误处理完善：区分进程不存在、权限不足、部分读写等错误。

## 依赖

- Linux 操作系统
- pthread 库（编译时链接 `-pthread`）
- 支持 `process_vm_readv` / `process_vm_writev` 的内核（≥ 3.2）

## 快速开始

1. 将 `memrw.h` 和 `memrw.c` 添加到你的项目中。
2. 在程序启动时调用 `memrw_init()` 初始化内部上下文。
3. 使用 `memrw_open_process(pid)` 打开目标进程。
4. 调用读写函数操作内存。
5. 使用完毕后调用 `memrw_close_process(pid)` 释放资源。

### 编译示例

```bash
gcc -o mytool mytool.c memrw.c -pthread
```

## API 概览

| 函数 | 说明 |
|------|------|
| `void memrw_init(void)` | 初始化全局上下文（必须首先调用） |
| `int memrw_open_process(pid_t pid)` | 打开并跟踪进程 |
| `int memrw_close_process(pid_t pid)` | 关闭并停止跟踪进程 |
| `int memrw_process_should_close(pid_t pid)` | 检查进程是否已退出 |
| `ssize_t memrv(pid_t pid, const struct iovec *local_iov, unsigned long liovcnt, const struct iovec *remote_iov, unsigned long riovcnt, unsigned long flags)` | 向量读取远程内存 |
| `ssize_t memwv(pid_t pid, const struct iovec *local_iov, unsigned long liovcnt, const struct iovec *remote_iov, unsigned long riovcnt, unsigned long flags)` | 向量写入远程内存 |
| `int memr(pid_t pid, void *local_buf, void *remote_buf, size_t len)` | 读取单块连续内存 |
| `int memw(pid_t pid, void *local_buf, void *remote_buf, size_t len)` | 写入单块连续内存 |
| `int memwx(pid_t pid, void *local_buf, void *remote_buf, size_t len)` | 直接通过 `/proc/pid/mem` 写入（绕过保护） |

## 使用示例

```c
#include "memrw.h"
#include <stdio.h>
#include <stdint.h>

int main() {
    memrw_init();

    pid_t target_pid = 1234;
    if (memrw_open_process(target_pid) != 0) {
        perror("open process");
        return 1;
    }

    // 读取远程地址 0x7f1234567000 开始的 8 字节
    uint64_t value;
    if (memr(target_pid, &value, (void*)0x7f1234567000, sizeof(value)) != 0) {
        perror("read memory");
        memrw_close_process(target_pid);
        return 1;
    }
    printf("Read value: 0x%lx\n", value);

    // 写入新值
    value = 0xdeadbeef;
    if (memw(target_pid, &value, (void*)0x7f1234567000, sizeof(value)) != 0) {
        perror("write memory");
    }

    memrw_close_process(target_pid);
    return 0;
}
```

## 注意事项

- **权限**：访问其他进程内存通常需要 root 或与目标进程相同的 UID，并具备 `PTRACE_MODE_ATTACH_REALCREDS` 权限。
- **物理页检查**：`memrv` 和 `memwv` 会在操作前检查远程地址对应的物理页是否存在（present bit）。如果页未驻留，返回 `EFAULT`。这会略微降低性能，但提高了可靠性。
- **`memwx` 的特殊性**：该函数直接写入 `/proc/pid/mem`，可以修改通常只读的内存区域（如代码段）。使用时需谨慎。
- **进程退出处理**：建议定期调用 `memrw_process_should_close(pid)` 检查目标进程是否退出，并及时调用 `memrw_close_process` 释放资源。
- **线程安全**：所有公共函数均为线程安全，可在多线程环境中使用。

## 许可证

本项目采用 Apache License 2.0 许可证。详情见 [LICENSE](LICENSE) 文件。

Copyright 2026 kaidev <kaidevonmail@gmail.com>