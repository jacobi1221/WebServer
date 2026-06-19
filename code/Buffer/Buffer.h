/**
 * 用户态读写缓冲区 —— 动态扩容 + 分散读
 *
 *   Buffer 是 HTTP 连接的核心缓冲区，设计要点:
 *
 *   1. 内存布局 (readPos_ / writePos_ 双指针):
 *      ┌─── prependable ───┬───── readable ─────┬───── writable ───┐
 *      │   [0, readPos_)   │ [readPos_, writePos_) │ [writePos_, cap) │
 *      └───────────────────┴──────────────────────┴──────────────────┘
 *
 *   2. 分散读 (readv):
 *      优先写入 Buffer 剩余空间，溢出写入栈上临时缓冲区。
 *      一次 readv 系统调用即可获取所有数据，ET 模式下循环到 EAGAIN。
 *
 *   3. 动态扩容:
 *      makeSpace_ 策略 —— 优先整理碎片（前移），碎片不够再扩容。
 *
 *   4. 原子读写指针:
 *      readPos_ 和 writePos_ 使用 std::atomic<size_t>，
 *      因为 HTTP 连接可能被线程池的不同线程读写。
 */

#ifndef BUFFER_H
#define BUFFER_H

#include <cstring>
#include <iostream>
#include <unistd.h>
#include <sys/uio.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <assert.h>

class Buffer {
public:
    /*initBuffSize 初始容量（默认 1KB） */
    Buffer(int initBuffSize = 1024);
    ~Buffer() = default;

    /* ===== 容量查询 ===== */
    size_t writableBytes() const;        ///< 可写入字节数
    size_t readableBytes() const;        ///< 可读取字节数
    size_t prependableBytes() const;     ///< 前置空闲字节数

    /* ===== 读取操作 ===== */
    const char* peek() const;            ///< 获取读指针（不消耗）
    void retrieve(size_t len);           ///< 消费 len 字节
    void retrieveUntil(const char* end); ///< 消费到指定位置
    void retrieveAll();                  ///< 清空缓冲区（重置读写指针）
    std::string retrieveAllToStr();      ///< 取出全部数据为 string 并清空

    /* ===== 写入操作 ===== */
    void ensureWriteable(size_t len);    ///< 保证可写空间 ≥ len
    void hasWritten(size_t len);         ///< 提交已写入 len 字节

    const char* beginWriteConst() const; ///< 获取写指针（只读）
    char* beginWrite();                  ///< 获取写指针（可写）

    void append(const std::string& str);
    void append(const char* str, size_t len);
    void append(const void* data, size_t len);
    void append(const Buffer& buff);

    /* ===== I/O ===== */
    ssize_t readFd(int fd, int* Errno);  ///< readv 分散读
    ssize_t writeFd(int fd, int* Errno); ///< write 写入 fd

private:
    char* beginPtr_();                   ///< buffer_ 起始地址（可写）
    const char* beginPtr_() const;       ///< buffer_ 起始地址（只读）

    /**
     *  1. writable + prependable ≥ len → 前移 readable 段，整理碎片
     *  2. 否则                        → resize 扩容
     */
    void makeSpace_(size_t len);

    std::vector<char> buffer_;              ///< 底层存储
    std::atomic<std::size_t> readPos_;      ///< 读指针位置
    std::atomic<std::size_t> writePos_;     ///< 写指针位置
};

#endif // BUFFER_H
