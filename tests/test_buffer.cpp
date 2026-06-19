/**
 * @file    test_buffer.cpp
 * @brief   Buffer 模块单元测试
 */

#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include <unistd.h>

#include "Buffer/Buffer.h"

/* ================================================================
 *  基本读写测试
 * ================================================================ */
TEST(BufferTest, InitialState) {
    Buffer buf(1024);
    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_EQ(buf.writableBytes(), 1024u);
    EXPECT_EQ(buf.prependableBytes(), 0u);
}

TEST(BufferTest, AppendAndRetrieve) {
    Buffer buf(1024);
    const char* data = "Hello, World!";

    buf.append(data, strlen(data));

    EXPECT_EQ(buf.readableBytes(), strlen(data));
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "Hello, World!");

    buf.retrieve(7);  // consume "Hello, "
    EXPECT_EQ(buf.readableBytes(), 6u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "World!");
}

TEST(BufferTest, AppendString) {
    Buffer buf(1024);
    buf.append("Hello");
    buf.append(std::string(", World"));

    EXPECT_EQ(buf.readableBytes(), 12u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "Hello, World");
}

TEST(BufferTest, RetrieveAll) {
    Buffer buf(1024);
    buf.append("test data", 9);
    buf.retrieveAll();

    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_EQ(buf.writableBytes(), 1024u);
    EXPECT_EQ(buf.prependableBytes(), 0u);
}

TEST(BufferTest, RetrieveAllToString) {
    Buffer buf(1024);
    buf.append("Hello");

    std::string result = buf.retrieveAllToStr();
    EXPECT_EQ(result, "Hello");
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(BufferTest, RetrieveUntil) {
    Buffer buf(1024);
    buf.append("Line1\r\nLine2\r\n");

    const char* end = std::search(buf.peek(), buf.beginWriteConst(), "\r\n", "\r\n" + 2);
    buf.retrieveUntil(end + 2);

    EXPECT_EQ(buf.readableBytes(), 6u);  // "Line2\r\n" left
}

/* ================================================================
 *  扩容测试
 * ================================================================ */
TEST(BufferTest, AutoExpand) {
    Buffer buf(16);  // small initial size
    std::string big(100, 'X');

    buf.append(big);
    EXPECT_GE(buf.writableBytes() + buf.readableBytes(), 100u);
    EXPECT_EQ(buf.readableBytes(), 100u);
}

TEST(BufferTest, PrependableAfterRetrieve) {
    Buffer buf(64);
    buf.append("AAAA", 4);   // write 4 bytes
    buf.retrieve(4);         // consume all

    EXPECT_EQ(buf.readableBytes(), 0u);
    EXPECT_GT(buf.prependableBytes(), 0u);  // 4 bytes freed

    // Append more — should use freed space without expanding
    buf.append("BBBB", 4);
    EXPECT_EQ(buf.readableBytes(), 4u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "BBBB");
}

TEST(BufferTest, MultipleAppendRetrieveCycles) {
    Buffer buf(64);

    for(int i = 0; i < 100; i++) {
        buf.append("data", 4);
        buf.retrieve(4);
    }

    // After many cycles, buffer should still be functional
    buf.append("final", 5);
    EXPECT_EQ(buf.readableBytes(), 5u);
}

/* ================================================================
 *  原子指针正确性
 * ================================================================ */
TEST(BufferTest, WritePointerAdvances) {
    Buffer buf(128);

    buf.append("ABCD", 4);
    EXPECT_EQ(buf.readableBytes(), 4u);

    buf.append("EFGH", 4);
    EXPECT_EQ(buf.readableBytes(), 8u);

    buf.retrieve(4);
    EXPECT_EQ(buf.readableBytes(), 4u);
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), "EFGH");
}

/* ================================================================
 *  I/O 测试 (使用 pipe)
 * ================================================================ */
TEST(BufferTest, ReadFdBasic) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    const char* msg = "Hello from pipe!";
    write(pipefd[1], msg, strlen(msg));
    close(pipefd[1]);

    Buffer buf(1024);
    int saveErrno = 0;
    ssize_t n = buf.readFd(pipefd[0], &saveErrno);

    EXPECT_EQ(n, static_cast<ssize_t>(strlen(msg)));
    EXPECT_EQ(buf.readableBytes(), strlen(msg));
    EXPECT_EQ(std::string(buf.peek(), buf.readableBytes()), msg);

    close(pipefd[0]);
}

TEST(BufferTest, WriteFdBasic) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    Buffer buf(1024);
    buf.append("Pipe write test");

    int saveErrno = 0;
    ssize_t n = buf.writeFd(pipefd[1], &saveErrno);
    close(pipefd[1]);

    EXPECT_GT(n, 0);

    char readBuf[64] = {0};
    ssize_t rn = read(pipefd[0], readBuf, sizeof(readBuf) - 1);
    EXPECT_EQ(rn, n);
    EXPECT_STREQ(readBuf, "Pipe write test");

    close(pipefd[0]);
}

TEST(BufferTest, ReadFdWithOverflow) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    // 写入超过 Buffer 默认容量的数据
    std::string bigData(2048, 'Z');
    write(pipefd[1], bigData.data(), bigData.size());
    close(pipefd[1]);

    Buffer buf(128);  // 初始只有 128 字节
    int saveErrno = 0;
    ssize_t n = buf.readFd(pipefd[0], &saveErrno);

    EXPECT_EQ(n, 2048);
    EXPECT_EQ(buf.readableBytes(), 2048u);

    close(pipefd[0]);
}

/* ================================================================
 *  边界条件
 * ================================================================ */
TEST(BufferTest, EmptyAppend) {
    Buffer buf(1024);
    buf.append("", 0);
    EXPECT_EQ(buf.readableBytes(), 0u);
}

TEST(BufferTest, AppendBuffer) {
    Buffer src(1024);
    src.append("Source data");

    Buffer dst(1024);
    dst.append(src);

    EXPECT_EQ(dst.readableBytes(), 11u);
    EXPECT_EQ(std::string(dst.peek(), dst.readableBytes()), "Source data");
}

TEST(BufferTest, EnsureWriteable) {
    Buffer buf(16);
    buf.ensureWriteable(1024);
    EXPECT_GE(buf.writableBytes(), 1024u);
}
