#include "Buffer.h"

Buffer::Buffer(int initBuffSize) : buffer_(initBuffSize), readPos_(0), writePos_(0) 
{}

/* ========================================================================
 *  容量查询
 * ======================================================================== */

size_t Buffer::readableBytes() const {
    return writePos_ - readPos_;
}

size_t Buffer::writableBytes() const {
    return buffer_.size() - writePos_;
}

size_t Buffer::prependableBytes() const {
    return readPos_;             // [0, readPos_) 是已消费区域，可被复用
}

/* ========================================================================
 *  读取操作
 * ======================================================================== */

const char* Buffer::peek() const {
    return beginPtr_() + readPos_;
}

void Buffer::retrieve(size_t len) {
    assert(len <= readableBytes());
    readPos_ += len;             // 原子操作，推进读指针
}

void Buffer::retrieveUntil(const char* end) {
    assert(peek() <= end);
    retrieve(end - peek());
}

void Buffer::retrieveAll() {
    readPos_ = 0;
    writePos_ = 0;               // 双指针归零，复用底层 vector 内存
}

std::string Buffer::retrieveAllToStr() {
    std::string str(peek(), readableBytes());
    retrieveAll();
    return str;
}

/* ========================================================================
 *  写入操作
 * ======================================================================== */

const char* Buffer::beginWriteConst() const {
    return beginPtr_() + writePos_;
}

char* Buffer::beginWrite() {
    return beginPtr_() + writePos_;
}

void Buffer::hasWritten(size_t len) {
    writePos_ += len;            // 原子操作，推进写指针
}

void Buffer::append(const std::string& str) {
    append(str.data(), str.length());
}

void Buffer::append(const void* data, size_t len) {
    assert(data);
    append(static_cast<const char*>(data), len);
}

void Buffer::append(const char* str, size_t len) {
    assert(str);
    ensureWriteable(len);
    std::copy(str, str + len, beginWrite());
    hasWritten(len);
}

void Buffer::append(const Buffer& buff) {
    append(buff.peek(), buff.readableBytes());
}

void Buffer::ensureWriteable(size_t len) {
    if(writableBytes() < len) {
        makeSpace_(len);          // 空间不足，触发整理或扩容
    }
    assert(writableBytes() >= len);
}

/* ========================================================================
 *  I/O 操作
 * ======================================================================== */

/**
 * 分散读 —— 使用 readv 从 fd 读取数据
 *
 *  设计思路:
 *    iov[0] = Buffer 剩余可写空间    (避免一次拷贝)
 *    iov[1] = 栈上临时缓冲区 16KB     (溢出缓冲区)
 *
 *  如果数据量 ≤ writable，只用了 iov[0]，无需额外拷贝。
 *  如果数据量 > writable，溢出部分从 extrabuf 拷贝到 Buffer（触发扩容）。
 *
 *  ET 模式下，调用方会循环 readFd 直到返回 EAGAIN。
 *
 *  @param saveErrno 传出 errno
 *  @return 读取字节数，<0 表示错误（errno 在 saveErrno 中）
 */
ssize_t Buffer::readFd(int fd, int* saveErrno)
{
    // 栈上 16KB 临时缓冲区（16KB 足够覆盖大多数 HTTP 请求头）
    char extrabuf[16384];
    struct iovec iov[2];
    const size_t writable = writableBytes();

    iov[0].iov_base = beginPtr_() + writePos_;
    iov[0].iov_len = writable;
    iov[1].iov_base = extrabuf;
    iov[1].iov_len = sizeof(extrabuf);

    const ssize_t len = readv(fd, iov, 2);
    if(len < 0)
    {
        *saveErrno = errno;
    }
    else if(static_cast<size_t>(len) <= writable)
    {
        // 全部数据写入 Buffer 内部空间
        writePos_ += len;
    }
    else
    {
        // 数据溢出到 extrabuf: 先填满 Buffer，再 append 剩余部分
        writePos_ = buffer_.size();
        append(extrabuf, len - writable);
    }
    return len;
}

/**
 * @brief 将 Buffer 数据写入 fd
 *
 *  @return 写入字节数，<0 表示错误
 */
ssize_t Buffer::writeFd(int fd, int* saveErrno)
{
    size_t readable = readableBytes();
    if(readable == 0) return 0;

    ssize_t len = ::write(fd, peek(), readable);
    if(len < 0)
    {
        *saveErrno = errno;
        return len;
    }
    retrieve(len);
    return len;
}

/* ========================================================================
 *  内部辅助
 * ======================================================================== */

char* Buffer::beginPtr_()             { return buffer_.data(); }
const char* Buffer::beginPtr_() const { return buffer_.data(); }

/**
 * @brief 整理或扩容空间
 *
 *  策略:
 *   1. writable + prependable ≥ len:
 *      将 readable 段前移到 buffer_[0]，释放后面的空间。
 *      避免了无谓的 resize。
 *
 *   2. 否则:
 *      resize 到 writePos_+len+1，保证足够空间。
 */
void Buffer::makeSpace_(size_t len)
{
    if(writableBytes() + prependableBytes() < len)
    {
        // 空间确实不够 → 扩容
        buffer_.resize(writePos_ + len + 1);
    }
    else
    {
        // 空间够，但碎片多 → 前移整理
        size_t readable = readableBytes();
        std::copy(beginPtr_() + readPos_, beginPtr_() + writePos_, beginPtr_());
        readPos_ = 0;
        writePos_ = readPos_ + readable;
        assert(readable == readableBytes());
    }
}
