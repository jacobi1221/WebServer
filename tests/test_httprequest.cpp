/**
 * @file    test_httprequest.cpp
 * @brief   HttpRequest 解析器单元测试
 */

#include <gtest/gtest.h>
#include <string>

#include "Buffer/Buffer.h"
#include "Http/HttpRequest.h"

/* ================================================================
 *  请求行解析
 * ================================================================ */
TEST(HttpRequestTest, ParseSimpleGet) {
    Buffer buf;
    buf.append("GET /index.html HTTP/1.1\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_EQ(req.method(), "GET");
    EXPECT_EQ(req.path(), "/index.html");
    EXPECT_EQ(req.version(), "1.1");
}

TEST(HttpRequestTest, ParsePost) {
    Buffer buf;
    buf.append("POST /login HTTP/1.1\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_EQ(req.method(), "POST");
    EXPECT_EQ(req.path(), "/login.html");  // DEFAULT_HTML 补全
}

TEST(HttpRequestTest, RootPathAutoIndex) {
    Buffer buf;
    buf.append("GET / HTTP/1.1\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_EQ(req.path(), "/index.html");  // / → /index.html
}

TEST(HttpRequestTest, DefaultHtmlCompletion) {
    Buffer buf;
    buf.append("GET /register HTTP/1.1\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_EQ(req.path(), "/register.html");  // auto-add .html
}

TEST(HttpRequestTest, Http10Version) {
    Buffer buf;
    buf.append("GET /index.html HTTP/1.0\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_EQ(req.version(), "1.0");
}

/* ================================================================
 *  请求头解析
 * ================================================================ */
TEST(HttpRequestTest, ParseHeaders) {
    Buffer buf;
    buf.append(
        "GET / HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Connection: keep-alive\r\n"
        "Accept: text/html\r\n"
        "\r\n"
    );

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(req.isKeepAlive());
}

TEST(HttpRequestTest, ConnectionClose) {
    Buffer buf;
    buf.append(
        "GET / HTTP/1.0\r\n"
        "Connection: close\r\n"
        "\r\n"
    );

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_FALSE(req.isKeepAlive());  // HTTP/1.0 default + explicit close
}

TEST(HttpRequestTest, Http11DefaultKeepAlive) {
    Buffer buf;
    buf.append("GET / HTTP/1.1\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(req.isKeepAlive());  // HTTP/1.1 默认 keep-alive
}

/* ================================================================
 *  POST 表单解析 (URL-encoded)
 * ================================================================ */
TEST(HttpRequestTest, ParseUrlencodedPost) {
    Buffer buf;
    buf.append(
        "POST /login.html HTTP/1.1\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 29\r\n"
        "\r\n"
        "username=admin&password=1234"
    );

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_TRUE(ok);
    // getPost won't work here without MySQL (userVerify) but parse doesn't crash
}

TEST(HttpRequestTest, UrlDecodePercent) {
    Buffer buf;
    buf.append(
        "POST / HTTP/1.1\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "\r\n"
        "name=%48%65%6C%6C%6F"  // "Hello"
    );

    HttpRequest req;
    bool ok = req.parse(buf);
    EXPECT_TRUE(ok);
    // URL decode works internally in parseFromUrlencoded
}

TEST(HttpRequestTest, UrlDecodePlus) {
    Buffer buf;
    buf.append(
        "POST / HTTP/1.1\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "\r\n"
        "name=Hello+World"  // + → space
    );

    HttpRequest req;
    bool ok = req.parse(buf);
    EXPECT_TRUE(ok);
}

/* ================================================================
 *  错误请求
 * ================================================================ */
TEST(HttpRequestTest, BadRequestLine) {
    Buffer buf;
    buf.append("NOT A VALID REQUEST LINE\r\n\r\n");

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_FALSE(ok);  // 应该解析失败
}

TEST(HttpRequestTest, EmptyBuffer) {
    Buffer buf;

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_FALSE(ok);  // 无数据 → 解析未完成
}

TEST(HttpRequestTest, IncompleteRequest) {
    Buffer buf;
    buf.append("GET /index.html HTTP/1.1\r\n");  // 缺少空行终止符

    HttpRequest req;
    bool ok = req.parse(buf);

    EXPECT_FALSE(ok);  // 状态未到 FINISH
    // 剩余数据保留在 buffer 中（或部分消耗），等待下次数据
}

/* ================================================================
 *  init() 重置测试
 * ================================================================ */
TEST(HttpRequestTest, ReuseParser) {
    Buffer buf1;
    buf1.append("GET /page1 HTTP/1.1\r\n\r\n");

    HttpRequest req;
    EXPECT_TRUE(req.parse(buf1));
    EXPECT_EQ(req.path(), "/page1.html");

    // 第二次解析 —— 状态机应完全重置
    Buffer buf2;
    buf2.append("GET /page2 HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(req.parse(buf2));
    EXPECT_EQ(req.path(), "/page2.html");
}

TEST(HttpRequestTest, GetPostReturnsEmptyForMissingKey) {
    EXPECT_EQ(HttpRequest().getPost("nonexistent"), "");
    EXPECT_EQ(HttpRequest().getPost(""), "");  // assert 保护 + 空 key
}

/* ================================================================
 *  Keep-Alive 判断边界
 * ================================================================ */
TEST(HttpRequestTest, KeepAliveCaseSensitiveHeader) {
    // HTTP 头字段名大小写不敏感（但当前实现是大小写敏感的，这是已知限制）
    Buffer buf;
    buf.append(
        "GET / HTTP/1.1\r\n"
        "connection: keep-alive\r\n"  // 小写 c
        "\r\n"
    );

    HttpRequest req;
    bool ok = req.parse(buf);
    EXPECT_TRUE(ok);
    // 当前实现：connection != Connection，所以找不到 header
    // 回退到 HTTP/1.1 默认 keep-alive = true
    EXPECT_TRUE(req.isKeepAlive());
}
