/**
 * @file    WebServer.h
 * @brief   高性能 Web 服务器主控制器 —— Reactor 核心
 * @details
 *
 *   架构角色：Reactor + Acceptor + Dispatcher
 *
 *   WebServer 是整个 HTTP 服务器的协调中心，它持有:
 *   - Epoller（I/O 多路复用器）
 *   - ThreadPool（业务线程池）
 *   - HeapTimer（连接超时定时器）
 *   - users_（fd → HttpConn 映射表）
 *
 *   事件循环 (start) 负责：
 *     1. epoll_wait 等待事件就绪
 *     2. 分发事件：accept → 新连接，EPOLLIN → 读，EPOLLOUT → 写
 *     3. 超时踢除：定时器 tick 清理空闲连接
 *
 *   并发模型：半同步/半反应堆 (HSHA)
 *   - 主线程（Reactor）：I/O 多路复用 + 事件分发
 *   - 工作线程池：HTTP 解析、文件读取、响应生成
 *
 *   关键设计：
 *   - EPOLLONESHOT 保证同一 fd 只被一个线程处理，避免惊群
 *   - ET 模式需循环 accept/read/write 直到 EAGAIN
 *   - 定时器绑定 closeConn 回调，超时自动关闭连接
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "Epoller.h"
#include "../Log/Log.h"
#include "../Timer/HeapTimer.h"
#include "../Pool/SqlConnPool.h"
#include "../Pool/ThreadPool.h"
#include "../Pool/SqlConnRAII.h"
#include "../Http/HttpConn.h"
#include <cstdint>
#include <netinet/in.h>

class WebServer
{
public:
    /**
     * @brief 构造函数 —— 初始化并启动所有子系统
     * @param port        监听端口 (1024-65535)
     * @param trigMode    epoll 触发模式: 0=LT+LT, 1=ET+LT, 2=LT+ET, 3=ET+ET
     * @param timeoutMS   连接超时 (ms)，≤0 表示不禁用超时踢除
     * @param OptLinger   优雅关闭 (SO_LINGER)
     * @param sqlPort     MySQL 端口
     * @param sqlUser     MySQL 用户名
     * @param sqlPwd      MySQL 密码
     * @param dbName      MySQL 数据库名
     * @param connPoolNum 数据库连接池大小
     * @param threadNum   线程池工作线程数
     * @param openLog     是否开启日志
     * @param logLevel    日志级别 (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
     * @param logQuesize  异步日志队列容量 (≤0 同步模式)
     */
    WebServer(int port, int trigMode, int timeoutMS, bool OptLinger,
              int sqlPort, const char* sqlUser, const char* sqlPwd,
              const char* dbName, int connPoolNum, int threadNum,
              bool openLog, int logLevel, int logQuesize);

    ~WebServer();

    /** @brief 启动事件循环 —— 阻塞直到 isClose_ */
    void start();

private:
    /* ===== 初始化 ===== */
    bool initSocket();
    void initEventMode(int trigMode);

    /* ===== 连接生命周期 ===== */
    void addClient(int fd, sockaddr_in addr);
    void closeConn(HttpConn* client);
    void sendError(int fd, const char* info);
    void extentTime(HttpConn* client);

    /* ===== 事件处理（主线程） ===== */
    void dealListen();               ///< accept 新连接（ET 模式下循环到 EAGAIN）
    void dealRead(HttpConn* client); ///< 提交读任务到线程池
    void dealWrite(HttpConn* client);///< 提交写任务到线程池

    /* ===== 业务回调（线程池内执行） ===== */
    void onRead(HttpConn* client);   ///< 执行读取 → 解析 HTTP
    void onWrite(HttpConn* client);  ///< 执行发送 → 维持或关闭连接
    void onProcess(HttpConn* client);///< 解析完成后重新注册 epoll 事件

    /** @brief 设置 fd 为非阻塞模式（EPOLLET 必须） */
    static int setFdNonblock(int fd);

    static const int MAX_FD = 65536; ///< 最大 fd 数（同时也是最大连接数）

    /* ===== 配置 ===== */
    int port_;
    bool openLinger_;
    int timeoutMS_;
    bool isClose_;
    int listenFd_;
    char* srcDir_;                   ///< 静态文件根目录（getcwd + "/resources/"）

    /* ===== epoll 事件配置 ===== */
    uint32_t listenEvent_;           ///< 监听 fd 的 epoll 事件掩码
    uint32_t connEvent_;             ///< 连接 fd 的 epoll 事件掩码（含 EPOLLONESHOT）

    /* ===== 子模块（unique_ptr 托管生命周期） ===== */
    std::unique_ptr<HeapTimer> timer_;            ///< 小根堆定时器
    std::unique_ptr<ThreadPool> threadpool_;      ///< 线程池
    std::unique_ptr<Epoller> epoller_;            ///< epoll 封装

    /** @brief fd → HttpConn 映射表，支持 O(1) 查找 */
    std::unordered_map<int, HttpConn> users_;
};

#endif // WEBSERVER_H
