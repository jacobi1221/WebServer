/**
 * @file    HttpConn.cpp
 * @brief   HttpConn 实现 —— 读写缓冲管理 + HTTP 解析调度
 */

#include "HttpConn.h"
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>

/* ===== 静态成员定义 ===== */
const char* HttpConn::srcDir;
std::atomic<int> HttpConn::userCount;
bool HttpConn::isET;

/* ========================================================================
 *  构造 / 析构
 * ======================================================================== */

HttpConn::HttpConn()
{
    fd_ = -1;
    addr_ = {0};
    isClose_ = true;    // 初始状态: 已关闭，直到 init() 激活
}

HttpConn::~HttpConn()
{
    close();
}

/* ========================================================================
 *  连接生命周期
 * ======================================================================== */

void HttpConn::init(int fd, const sockaddr_in& addr)
{
    assert(fd > 0);
    userCount++;                     // 原子自增
    addr_ = addr;
    fd_ = fd;
    writeBuff_.retrieveAll();        // 清空写缓冲区
    readBuff_.retrieveAll();         // 清空读缓冲区
    isClose_ = false;

    LOG_INFO("Client[%d](%s:%d) in, userCount:%d",
             fd_, getIP(), getPort(), (int)userCount);
}

void HttpConn::close()
{
    response_.unmapFile();            // 解除 mmap 映射
    if(isClose_ == false)
    {
        isClose_ = true;
        userCount--;                  // 原子自减

        LOG_INFO("Client[%d](%s:%d) quit, userCount:%d",
                 fd_, getIP(), getPort(), (int)userCount);

        ::close(fd_);
        fd_ = -1;                     // 防止 double-close 和 fd 复用竞态
    }
}

/* ========================================================================
 *  Getters
 * ======================================================================== */

int HttpConn::getFd() const { return fd_; }

struct sockaddr_in HttpConn::getAddr() const { return addr_; }

const char* HttpConn::getIP() const {
    // thread_local: 每个线程独立的静态缓冲区，避免并发写同一内存
    static thread_local char ipBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr_.sin_addr, ipBuf, sizeof(ipBuf));
    return ipBuf;
}

int HttpConn::getPort() const {
    return ntohs(addr_.sin_port);
}

/* ========================================================================
 *  I/O: 分散读 + 集中写
 * ======================================================================== */

/**
 * @brief 分散读 —— 将 socket 数据读入 readBuff_
 *
 *  ET 模式：循环 readv 直到返回 EAGAIN（数据读完）。
 *  LT 模式：读一次即返回（下次有数据会再次触发 EPOLLIN）。
 */
ssize_t HttpConn::read(int* saveErrno)
{
    ssize_t len = -1;
    do {
        len = readBuff_.readFd(fd_, saveErrno);
        if(len <= 0) break;          // EAGAIN 或错误
    } while(isET);                   // ET 模式循环到 EAGAIN

    return len;
}

/**
 * @brief 集中写 —— 将 iov 数据通过 writev 发送
 *
 *  写循环条件:
 *    isET                                             → 循环到 EAGAIN
 *    toWriteBytes() > kMaxIovBytes (10240, LT 模式)  → 积压超过阈值时继续写
 *
 *  iovec 偏移管理:
 *    writev 返回已发送字节数 len:
 *    - len > iov[0].iov_len: 说明写完了 iov[0]，还写了部分 iov[1] → 更新 iov[1] 偏移
 *    - len <= iov[0].iov_len: 只写了 iov[0] 的一部分 → 更新 iov[0] 偏移
 */
ssize_t HttpConn::write(int* saveErrno)
{
    // LT 模式下攒批写入阈值：积压超过此值继续写，减少 epoll_wait 次数
    static const size_t kMaxIovBytes = 10240;

    ssize_t len = -1;
    do {
        len = writev(fd_, iov_, iovCnt_);

        if(len <= 0)
        {
            // 对端关闭或出错
            *saveErrno = errno;
            break;
        }

        if(toWriteBytes() == 0)
        {
            // 全部数据发送完毕
            break;
        }
        else if(static_cast<size_t>(len) > iov_[0].iov_len)
        {
            // writev 写完了 iov[0] (响应头) + 写了部分 iov[1] (文件)
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);

            if(iov_[0].iov_len)
            {
                writeBuff_.retrieveAll();
                iov_[0].iov_len = 0;
            }
        }
        else
        {
            // writev 只写了 iov[0] 的一部分
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len;
            iov_[0].iov_len -= len;
            writeBuff_.retrieve(len);
        }
    } while(isET || toWriteBytes() > kMaxIovBytes);

    return len;
}

/* ========================================================================
 *  HTTP 处理流水线
 * ======================================================================== */

/**
 * @brief 解析 HTTP 请求并构造响应
 *
 *  流程:
 *   1. 调用 HttpRequest::parse(readBuff_) 解析请求行/头/体
 *   2. 根据解析结果调用 HttpResponse::init() + makeResponse()
 *   3. 设置 iov_[0] = writeBuff_（响应头）
 *   4. 如果有 mmap 文件，设置 iov_[1] = file（文件内容）
 *
 *  @return true 成功构造响应
 */
bool HttpConn::process()
{
    request_.init();

    if(readBuff_.readableBytes() <= 0)
    {
        // 缓冲区无数据，等待下次读取
        return false;
    }
    else if(request_.parse(readBuff_))
    {
        // 解析成功 → 构造 200 响应
        LOG_DEBUG("%s", request_.path().c_str());
        response_.init(srcDir, request_.path(), request_.isKeepAlive(), 200);
    }
    else
    {
        // 解析失败 → 构造 400 响应
        response_.init(srcDir, request_.path(), false, 400);
    }

    // 生成 HTTP 响应到 writeBuff_
    response_.makeResponse(writeBuff_);

    // 设置 iovec
    iov_[0].iov_base = const_cast<char*>(writeBuff_.peek());
    iov_[0].iov_len = writeBuff_.readableBytes();
    iovCnt_ = 1;

    // 如果有 mmap 映射的文件内容，设置 iov[1]
    if(response_.fileLen() > 0 && response_.file())
    {
        iov_[1].iov_base = response_.file();
        iov_[1].iov_len = response_.fileLen();
        iovCnt_ = 2;            // 两个 iov 段: 头部 + 文件
    }

    LOG_DEBUG("filesize:%d, %d to %d", response_.fileLen(), iovCnt_, toWriteBytes());
    return true;
}
