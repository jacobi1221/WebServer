/**
 * @file    HeapTimer.h
 * @brief   小根堆定时器 —— O(log n) 的超时管理
 * @details
 *
 *   使用二叉堆 + 哈希索引 实现高效定时器:
 *
 *   数据结构:
 *     heap_ = vector<TimerNode{id, expires, callback}>
 *     ref_  = unordered_map<id_t → heap_index>   (O(1) 查找节点位置)
 *
 *   操作复杂度:
 *     add(id, timeout)     — O(log n)  插入 + siftup
 *     adjust(id, timeout)  — O(log n)  siftdown 或 siftup
 *     tick()               — O(k·log n) 弹出所有超时节点并执行回调
 *     doWork(id)           — O(log n)  删除 + 执行回调
 *     getNextTick()        — O(k·log n) 返回距最近超时的 ms 数
 *
 *   线程安全: 仅在主线程（Reactor）操作，不需加锁。
 */

#ifndef HEAPTIMER_H
#define HEAPTIMER_H

#include <functional>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <assert.h>

#include "../Log/Log.h"

/* ===== 类型别名 ===== */
using TimeoutCallBack = std::function<void()>;          ///< 超时回调函数
using Clock           = std::chrono::high_resolution_clock;
using MS              = std::chrono::milliseconds;
using TimeStamp       = Clock::time_point;

/**
 * @struct TimerNode
 * @brief  定时器节点: id + 过期时间 + 回调
 *
 *  按 expires 排序（operator<），构造小根堆。
 */
struct TimerNode {
    int id;                          ///< 节点标识（通常是 fd）
    TimeStamp expires;               ///< 超时时间点
    TimeoutCallBack cb;              ///< 超时回调（如 closeConn）
    bool operator<(const TimerNode& t) const {
        return expires < t.expires;  // 时间越小越优先
    }
};

class HeapTimer {
public:
    HeapTimer() { heap_.reserve(64); }  ///< 预留 64 个节点空间
    ~HeapTimer() { clear(); }

    void add(int id, int timeOut, const TimeoutCallBack& cb);

    void adjust(int id, int newExpires);

    void doWork(int id);

    void clear();

    /**
     * @brief 清理所有超时节点并触发回调
     *
     *  每次 epoll_wait 前调用（通过 getNextTick 触发）。
     *  从堆顶开始弹出所有 expires ≤ now 的节点并执行回调。
     */
    void tick();

    void pop();

    int getNextTick();

private:
    /* ===== 堆操作 ===== */
    void del(size_t i);                                      ///< 删除位置 i 的节点
    void siftup(size_t i);                                   ///< 节点 i 上浮
    bool siftdown(size_t index, size_t n);                   ///< 节点 index 下沉
    void swapNode(size_t i, size_t j);                       ///< 交换节点并更新 ref_

    std::vector<TimerNode> heap_;                             ///< 二叉堆
    std::unordered_map<int, size_t> ref_;                     ///< id → 堆索引
};

#endif // HEAP_TIMER_H
