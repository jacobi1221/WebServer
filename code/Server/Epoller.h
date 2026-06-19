/**
 * @file    Epoller.h
 * @brief   Linux epoll 的 C++ RAII 封装
 * @details
 *
 *   封装了 epoll 的三个核心操作:
 *     - epoll_ctl(ADD / MOD / DEL)
 *     - epoll_wait
 *
 *   Epoller 持有 epoll 实例 fd 和事件数组，提供类型安全的
 *   事件注册/修改接口。epollFd_ 在析构时自动关闭。
 */

#ifndef EPOLLER_H
#define EPOLLER_H

#include <sys/epoll.h>
#include <vector>

class Epoller {
public:
    /**
     * @brief 创建 epoll 实例
     * @param maxEvent 每次 epoll_wait 最大返回的事件数（预留 vector 容量）
     */
    explicit Epoller(int maxEvent = 1024);

    ~Epoller();

    /**
     * @brief 向 epoll 注册 fd
     * @param fd     要监听的文件描述符
     * @param events 事件掩码（EPOLLIN | EPOLLET | EPOLLONESHOT ...）
     * @return true 成功，false 失败
     */
    bool addFd(int fd, uint32_t events);

    /**
     * @brief 修改已注册 fd 的监听事件
     *
     *  与 EPOLLONESHOT 配合使用：事件处理完后需要 modFd 重新激活监听。
     *
     * @param fd     已注册的文件描述符
     * @param events 新的事件掩码
     * @return true 成功，false 失败
     */
    bool modFd(int fd, uint32_t events);

    /**
     * @brief 从 epoll 移除 fd
     *
     *  即使不调用 delFd，内核也会在 fd close 时自动清理。
     *  但 fd 号可能被新连接复用，显式移除更为安全。
     *
     * @param fd 要移除的文件描述符
     * @return true 成功，false 失败
     */
    bool delFd(int fd);

    /**
     * @brief 等待事件就绪
     * @param timeoutMs 超时 (ms)，-1 表示无限等待
     * @return 就绪事件数，出错返回 -1
     */
    int wait(int timeoutMs = -1);

    /** @brief 获取第 i 个就绪事件的 fd */
    int getEventFd(size_t i) const;

    /** @brief 获取第 i 个就绪事件的事件类型掩码 */
    uint32_t getEvents(size_t i) const;

private:
    int epollFd_;                           ///< epoll 实例文件描述符
    std::vector<struct epoll_event> events_;///< epoll_wait 就绪事件数组
};

#endif // EPOLLER_H
