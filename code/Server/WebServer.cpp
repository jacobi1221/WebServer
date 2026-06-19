/**
 * @file    WebServer.cpp
 * @brief   WebServer 实现 —— Reactor 事件循环与连接管理
 */

#include "WebServer.h"
#include "Epoller.h"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/* ========================================================================
 *  构造函数 / 析构函数
 * ======================================================================== */

WebServer::WebServer(int port, int trigMode, int timeoutMS, bool OptLinger,
              int sqlPort, const char* sqlUser, const char* sqlPwd,
              const char* dbName, int connPoolNum, int threadNum,
              bool openLog, int logLevel, int logQuesize)
    : port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
      listenFd_(-1), srcDir_(nullptr), listenEvent_(0), connEvent_(0)
{
    /* ---- 1. 初始化资源目录 ---- */
    srcDir_ = getcwd(nullptr, 256);   // getcwd 内部 malloc，需 free 释放
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);
    HttpConn::userCount = 0;
    HttpConn::srcDir = srcDir_;

    /* ---- 2. 初始化数据库连接池 ---- */
    SqlConnPool::instance()->init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    /* ---- 3. 创建子模块 ---- */
    timer_ = std::make_unique<HeapTimer>();
    threadpool_ = std::make_unique<ThreadPool>(threadNum);
    epoller_ = std::make_unique<Epoller>();

    /* ---- 4. 设置 epoll 触发模式 ---- */
    initEventMode(trigMode);

    /* ---- 5. 初始化监听 socket ---- */
    if(!initSocket()) { isClose_ = true; }

    /* ---- 6. 启动日志系统 ---- */
    if(openLog)
    {
        Log::Instance()->init(logLevel, "./log", ".log", logQuesize);
        if(isClose_)
        {
            LOG_ERROR("======= Server init error! =======");
        }
        else
        {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger ? "true" : "false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET" : "LT"),
                            (connEvent_ & EPOLLET ? "ET" : "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

WebServer::~WebServer()
{
    isClose_ = true;
    if(listenFd_ >= 0) { close(listenFd_); }
    free(srcDir_);       // getcwd 通过 malloc 分配，需用 free 释放
    SqlConnPool::instance()->closePool();
}

/* ========================================================================
 *  事件循环 —— Reactor 核心
 * ======================================================================== */

void WebServer::start()
{
    int timeMs = -1;    // epoll_wait 超时时间，-1 表示无限等待
    if(!isClose_) { LOG_INFO("===== Server start ====="); }

    while(!isClose_)
    {
        // 计算下一个定时器到期的时间，作为 epoll_wait 的超时
        if(timeoutMS_ > 0) { timeMs = timer_->getNextTick(); }

        // 阻塞等待事件就绪
        int eventCnt = epoller_->wait(timeMs);
        for(int i = 0; i < eventCnt; i++)
        {
            int fd = epoller_->getEventFd(i);
            uint32_t events = epoller_->getEvents(i);

            if(fd == listenFd_)
            {
                // >>> 事件类型 1: 新连接到达
                dealListen();
            }
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // >>> 事件类型 2: 对端关闭或出错 → 关闭连接
                assert(users_.count(fd) > 0);
                closeConn(&users_[fd]);
            }
            else if(events & EPOLLIN)
            {
                // >>> 事件类型 3: 数据可读 → 提交线程池处理
                assert(users_.count(fd) > 0);
                dealRead(&users_[fd]);
            }
            else if(events & EPOLLOUT)
            {
                // >>> 事件类型 4: 缓冲区可写 → 提交线程池处理
                assert(users_.count(fd) > 0);
                dealWrite(&users_[fd]);
            }
            else
            {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

/* ========================================================================
 *  epoll 触发模式配置
 * ======================================================================== */

void WebServer::initEventMode(int trigMode)
{
    /*
     *  默认配置:
     *    listenFd: EPOLLRDHUP                      (检测对端半关闭)
     *    connFd:   EPOLLONESHOT | EPOLLRDHUP       (一个事件只触发一个线程)
     *
     *  trigMode 补充 ET 标志:
     *    0 = listen(LT) + conn(LT)   — 简单场景
     *    1 = listen(LT) + conn(ET)   — 短连接优化
     *    2 = listen(ET) + conn(LT)   — 高并发 accept
     *    3 = listen(ET) + conn(ET)   — 最大吞吐
     */
    listenEvent_ = EPOLLRDHUP;
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;

    switch(trigMode)
    {
        case 0:
            // LT + LT: 默认即可，无需额外标志
            break;
        case 1:
            // ET + LT: 仅连接 socket 使用 ET，减少读事件通知次数
            connEvent_ |= EPOLLET;
            break;
        case 2:
            // LT + ET: 仅监听 socket 使用 ET，减少 accept 唤醒
            listenEvent_ |= EPOLLET;
            break;
        case 3:
            // ET + ET: 全部 ET，高吞吐场景
            listenEvent_ |= EPOLLET;
            connEvent_ |= EPOLLET;
            break;
        default:
            // 未知模式 → 默认全部 ET
            listenEvent_ |= EPOLLET;
            connEvent_ |= EPOLLET;
            break;
    }

    // 通知 HttpConn 当前是否 ET 模式（影响 read/write 循环策略）
    HttpConn::isET = (connEvent_ & EPOLLET);
}

/* ========================================================================
 *  连接管理
 * ======================================================================== */

/**
 * @brief 接受新连接并注册到 epoll
 *
 *  处理流程:
 *   1. 设置非阻塞（ET 模式必须）
 *   2. 禁用 Nagle 算法（减少小包延迟）
 *   3. 初始化 HttpConn（fd, addr, ++userCount）
 *   4. 添加定时器（超时回调 → closeConn）
 *   5. 注册 epoll（EPOLLIN + connEvent_）
 *
 *  @note 必须在 setFdNonblock 之后再 init + addFd，避免工作线程读到阻塞 fd
 */
void WebServer::addClient(int fd, sockaddr_in addr)
{
    assert(fd > 0);

    if(setFdNonblock(fd) < 0)
    {
        LOG_ERROR("setFdNonblock fd[%d] error!", fd);
        ::close(fd);
        return;
    }

    // 禁用 Nagle 算法: 小响应包不必等待 ACK
    int optval = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    users_[fd].init(fd, addr);

    // 如果开启了超时，给连接注册一个定时器
    if(timeoutMS_ > 0)
    {
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::closeConn, this, &users_[fd]));
    }

    epoller_->addFd(fd, EPOLLIN | connEvent_);
    LOG_INFO("Client[%d] in!", users_[fd].getFd());
}

/**
 * @brief 关闭连接 —— 从 epoll 移除，调用 HttpConn::close()
 *
 *  @note 先 delFd 再 close fd: 内核会忽略已关闭 fd 的 epoll 事件，
 *        但 fd 号可能被新连接复用，所以先手动移除更为安全。
 */
void WebServer::closeConn(HttpConn* client)
{
    assert(client);
    LOG_INFO("Client[%d] quit!", client->getFd());

    epoller_->delFd(client->getFd());
    client->close();
}

/**
 * @brief 向 fd 发送错误信息后关闭
 * @param fd   目标 socket 描述符
 * @param info 错误消息字符串（null 时不发送，直接关闭）
 */
void WebServer::sendError(int fd, const char* info)
{
    assert(fd > 0);
    if(info)
    {
        int ret = send(fd, info, strlen(info), MSG_NOSIGNAL);
        if(ret < 0) { LOG_WARN("send error to client[%d] error!", fd); }
    }
    ::close(fd);
}

/* ========================================================================
 *  事件处理（主线程 → 提交到线程池）
 * ======================================================================== */

/**
 * @brief 接受新连接 —— ET 模式下必须循环到返回 -1/EAGAIN
 */
void WebServer::dealListen()
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    do {
        int fd = accept(listenFd_, (struct sockaddr*)&addr, &len);

        if(fd <= 0) { return; }    // EAGAIN 或出错，退出循环
        else if(HttpConn::userCount >= MAX_FD)
        {
            // 连接数已达上限，拒绝并关闭
            sendError(fd, "Server busy!");
            LOG_WARN("client is full!");
            return;
        }

        addClient(fd, addr);
    } while(listenEvent_ & EPOLLET);  // ET 模式下循环到 EAGAIN
}

/**
 * @brief 可读事件: 更新时间戳，提交读任务到线程池
 */
void WebServer::dealRead(HttpConn* client)
{
    assert(client);
    extentTime(client);
    threadpool_->AddTask(std::bind(&WebServer::onRead, this, client));
}

/**
 * @brief 可写事件: 更新时间戳，提交写任务到线程池
 */
void WebServer::dealWrite(HttpConn* client)
{
    assert(client);
    extentTime(client);
    threadpool_->AddTask(std::bind(&WebServer::onWrite, this, client));
}

/**
 * @brief 刷新连接超时时间
 */
void WebServer::extentTime(HttpConn* client)
{
    assert(client);
    if(timeoutMS_ > 0)
    {
        timer_->adjust(client->getFd(), timeoutMS_);
    }
}

/* ========================================================================
 *  业务回调（在线程池内执行）
 * ======================================================================== */

/**
 * @brief 读回调: readv 分散读 → 解析 HTTP 请求 → 构造响应
 *
 *  返回值语义:
 *    ret > 0                         → 读取了数据，继续解析
 *    ret < 0 && readError == EAGAIN  → 数据还没到齐（非阻塞），等待下次 EPOLLIN
 *    ret < 0 && readError != EAGAIN  → 读错误，关闭连接
 *    ret == 0                        → 对端关闭连接
 */
void WebServer::onRead(HttpConn* client)
{
    assert(client);

    int ret = -1;
    int readError = 0;
    ret = client->read(&readError);

    if(ret <= 0 && readError != EAGAIN)
    {
        // 读出错或对端关闭
        closeConn(client);
        return;
    }

    // 数据可读 → 解析 HTTP 请求
    onProcess(client);
}

/**
 * @brief 处理 HTTP 请求，完成后重新注册 epoll 事件
 *
 *  process() 解析 HTTP 并构造 iov 发送缓冲区，返回 true 表示有数据要发送。
 */
void WebServer::onProcess(HttpConn* client)
{
    if(client->process())
    {
        // 有响应数据要发送 → 注册 EPOLLOUT
        epoller_->modFd(client->getFd(), connEvent_ | EPOLLOUT);
    }
    else
    {
        // 解析未完成，继续等待数据 → 注册 EPOLLIN
        epoller_->modFd(client->getFd(), connEvent_ | EPOLLIN);
    }
}

/**
 * @brief 写回调: writev 集中写 → 判断是否保持连接
 *
 *  处理逻辑:
 *   1. toWriteBytes() == 0
 *        → isKeepAlive → 重新进入 process 等待下次请求
 *        → !isKeepAlive → 关闭
 *   2. ret < 0 && writeErrno == EAGAIN
 *        → 缓冲区满，重新注册 EPOLLOUT，等待下次可写
 *   3. ret < 0 && writeErrno != EAGAIN
 *        → 写错误，关闭连接
 */
void WebServer::onWrite(HttpConn* client)
{
    assert(client);

    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);

    if(client->toWriteBytes() == 0)
    {
        // 全部数据已发送完毕
        if(client->isKeepAlive())
        {
            // HTTP Keep-Alive: 继续复用连接，等待下一个请求
            onProcess(client);
            return;
        }
    }
    else if(ret < 0)
    {
        // writev 返回错误
        if(writeErrno == EAGAIN)
        {
            // 发送缓冲区满（非阻塞），等待下次可写事件
            epoller_->modFd(client->getFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }

    // 写完成（非 keep-alive）或 写错误 → 关闭连接
    closeConn(client);
}

/* ========================================================================
 *  Socket 初始化
 * ======================================================================== */

/**
 * @brief 初始化监听 socket: socket() → setsockopt() → bind() → listen()
 * @return true 成功，false 失败（isClose_ 被设为 true）
 */
bool WebServer::initSocket()
{
    int ret;
    struct sockaddr_in addr;
    if(port_ > 65535 || port_ < 1024)
    {
        LOG_ERROR("Port:%d error!", port_);
        return false;
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    // ---- SO_LINGER: 优雅关闭 ----
    struct linger optLinger = {0};
    if(openLinger_)
    {
        optLinger.l_onoff = 1;   // 开启
        optLinger.l_linger = 1;  // 等待 1 秒
    }

    // ---- socket() ----
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0)
    {
        LOG_ERROR("Create socket error! port:%d", port_);
        return false;
    }

    // ---- SO_LINGER ----
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if(ret < 0)
    {
        close(listenFd_);
        LOG_ERROR("Init linger error! port:%d", port_);
        return false;
    }

    // ---- SO_REUSEADDR: 允许重用 TIME_WAIT 状态的地址 ----
    int optval = 1;
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1)
    {
        LOG_ERROR("Set socket SO_REUSEADDR error!");
        close(listenFd_);
        return false;
    }

    // ---- SO_REUSEPORT: 允许端口复用（多进程负载均衡） ----
#ifdef SO_REUSEPORT
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEPORT, (const void*)&optval, sizeof(int));
    if(ret == -1)
    {
        LOG_WARN("Set socket SO_REUSEPORT error (non-fatal)");
    }
#endif

    // ---- bind() ----
    ret = bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr));
    if(ret < 0)
    {
        LOG_ERROR("Bind port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // ---- listen() ----
    // backlog = 6: 已完成队列大小（完整的值 = min(backlog, /proc/sys/net/core/somaxconn)）
    ret = listen(listenFd_, 6);
    if(ret < 0)
    {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // ---- 注册 epoll ----
    ret = epoller_->addFd(listenFd_, listenEvent_ | EPOLLIN);
    if(ret == 0)
    {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }

    setFdNonblock(listenFd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

/**
 * @brief 设置文件描述符为非阻塞模式
 *
 *  ET 模式下必须将 fd 设为非阻塞，否则 read/write 可能阻塞整个事件循环。
 *
 *  @param fd 文件描述符
 *  @return 0 成功，-1 失败
 */
int WebServer::setFdNonblock(int fd)
{
    assert(fd > 0);

    int oldFlags = fcntl(fd, F_GETFL, 0);    // 获取当前状态标志
    if(oldFlags == -1) return -1;

    return fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK);
}
