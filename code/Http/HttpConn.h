/**
 * @file    HttpConn.h
 * @brief   HTTP 连接对象 —— 一个 TCP 连接的全生命周期管理
 * @details
 *
 *   HttpConn 封装了一个 TCP 客户端连接的完整生命周期:
 *     init() → read() → process() → write() → close()
 *
 *   关键设计：
 *   1. iovec 双缓冲
 *      - iov_[0] = writeBuff_ (HTTP 响应头)
 *      - iov_[1] = mmap 文件映射 (静态文件内容)
 *      - writev() 一次系统调用完成两段数据的发送
 *
 *   2. 读写分离
 *      - readBuff_: 存储从 socket 读取的 HTTP 请求
 *      - writeBuff_: 存储构造好的 HTTP 响应头
 *
 *   3. 静态成员
 *      - isET: 全局 ET 标志，影响 read/write 的循环策略
 *      - srcDir: 静态文件根目录
 *      - userCount: 当前活跃连接数（原子变量）
 */

#ifndef HTPPCONN_H
#define HTPPCONN_H

#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include "../Buffer/Buffer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

class HttpConn
{
public:
    HttpConn();
    ~HttpConn();

    /**
     * @brief 初始化连接（accept 后调用）
     * @param sockFd socket 文件描述符
     * @param addr   对端地址
     */
    void init(int sockFd, const sockaddr_in& addr);

    /**
     * @brief 从 socket 读取数据到 readBuff_
     *
     *  ET 模式下循环 readv 直到 EAGAIN。
     *
     * @param saveErrno 传出参数: 错误时保存 errno
     * @return 读取字节数，≤0 表示错误或对端关闭
     */
    ssize_t read(int* saveErrno);

    /**
     * @brief 将 iov 数据集中写入 socket
     *
     *  writev 发送 iov_[0] (响应头) + iov_[1] (文件)。
     *  ET 模式或剩余数据 > 阈值时循环直到全部发送或 EAGAIN。
     *
     * @param saveErrno 传出参数: 错误时保存 errno
     * @return 写入字节数
     */
    ssize_t write(int* saveErrno);

    /** @brief 关闭连接: unmount mmap, close fd, --userCount */
    void close();

    /** @brief 获取 socket fd */
    int getFd() const;

    /** @brief 获取对端端口 */
    int getPort() const;

    /** @brief 获取对端 IP 字符串（thread_local 缓冲区，下次调用会覆盖） */
    const char* getIP() const;

    /** @brief 获取对端地址 */
    sockaddr_in getAddr() const;

    /**
     * @brief 解析 HTTP 请求并构造响应
     *
     *  1. HttpRequest::parse(readBuff_)  ← 状态机解析
     *  2. HttpResponse::makeResponse()  ← mmap + 构造头部
     *  3. 设置 iov_[0] (头部) + iov_[1] (文件)
     *
     *  @return true 成功生成响应
     */
    bool process();

    /** @brief iov 中待发送的总字节数 */
    int toWriteBytes() {
        return iov_[0].iov_len + iov_[1].iov_len;
    }

    /** @brief 是否保持连接（HTTP/1.1 keep-alive） */
    bool isKeepAlive() const {
        return request_.isKeepAlive();
    }

    /* ===== 静态配置成员 ===== */
    static bool isET;                   ///< 是否 ET 模式
    static const char* srcDir;          ///< 静态文件根目录
    static std::atomic<int> userCount;  ///< 活跃连接数（原子操作，线程安全）

private:
    int fd_;                            ///< socket 文件描述符
    struct sockaddr_in addr_;           ///< 对端地址

    bool isClose_;                      ///< 连接是否已关闭

    int iovCnt_;                        ///< iovec 有效元素个数 (1 或 2)
    struct iovec iov_[2];               ///< 集中写缓冲区: [0]=响应头, [1]=文件

    Buffer readBuff_;                   ///< 读缓冲区（HTTP 请求）
    Buffer writeBuff_;                  ///< 写缓冲区（HTTP 响应头）

    HttpRequest request_;               ///< HTTP 请求解析器
    HttpResponse response_;             ///< HTTP 响应构造器
};

#endif // HTPPCONN_H
