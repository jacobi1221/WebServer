/**
 * @file    Epoller.cpp
 * @brief   Epoller 实现
 */

#include "Epoller.h"
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

/**
 * @brief 创建 epoll 实例并预留事件数组空间
 *
 *  使用 epoll_create1(0) 而非老旧的 epoll_create(size)。
 *  epoll_create1 的 flags 参数支持 EPOLL_CLOEXEC，避免 fork 后 fd 泄漏。
 */
Epoller::Epoller(int maxEvent)
    : epollFd_(epoll_create1(0))
    , events_(maxEvent)
{
    assert(epollFd_ >= 0 && events_.size() > 0);
}

Epoller::~Epoller() {
    close(epollFd_);
}

bool Epoller::addFd(int fd, uint32_t events) {
    if(fd < 0) return false;
    epoll_event ev = {0};
    ev.data.fd = fd;
    ev.events = events;
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
}

bool Epoller::modFd(int fd, uint32_t events) {
    if(fd < 0) return false;
    epoll_event ev = {0};
    ev.data.fd = fd;
    ev.events = events;
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

bool Epoller::delFd(int fd) {
    if(fd < 0) return false;
    // kernel 2.6.9+ 在 EPOLL_CTL_DEL 时忽略 event 参数，传 nullptr 即可
    return 0 == epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Epoller::wait(int timeoutMs) {
    return epoll_wait(epollFd_, &events_[0], static_cast<int>(events_.size()), timeoutMs);
}

int Epoller::getEventFd(size_t i) const {
    assert(i < events_.size());
    return events_[i].data.fd;
}

uint32_t Epoller::getEvents(size_t i) const {
    assert(i < events_.size());
    return events_[i].events;
}
