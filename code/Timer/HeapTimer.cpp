/**
 * @file    HeapTimer.cpp
 * @brief   小根堆定时器实现
 */

#include "HeapTimer.h"

/* ========================================================================
 *  堆操作原语
 * ======================================================================== */

/**
 * @brief 节点 i 向上调整（sift-up / bubble-up）
 *
 *  与父节点 j=(i-1)/2 比较，如果当前节点更小则交换并继续上浮。
 *  用于 add() 插入新节点后的堆调整。
 */
void HeapTimer::siftup(size_t i)
{
    assert(i < heap_.size());
    while(i > 0)
    {
        size_t j = (i - 1) / 2;          // 父节点索引
        if(heap_[j] < heap_[i]) { break; } // 父节点更小 → 堆性质满足
        swapNode(i, j);
        i = j;
    }
}

/**
 * @brief 节点 index 向下调整（sift-down / heapify）
 *
 *  与左右子节点比较，如果子节点更小则交换并继续下沉。
 *  n 是堆的有效大小（del_ 时会暂时缩小 1）。
 *
 *  @return true 如果节点发生了移动
 */
bool HeapTimer::siftdown(size_t index, size_t n)
{
    assert(index < heap_.size());
    assert(n <= heap_.size());

    size_t i = index;
    size_t j = i * 2 + 1;                // 左子节点索引
    while(j < n)
    {
        // 选择左右子节点中更小的那个
        if(j + 1 < n && heap_[j + 1] < heap_[j]) j++;

        // 当前节点已经比子节点更小 → 停止
        if(heap_[i] < heap_[j]) break;

        swapNode(i, j);
        i = j;
        j = i * 2 + 1;
    }
    return i > index;                    // 发生过下沉
}

/**
 * @brief 交换堆中 i 和 j 位置的节点，并同步更新 ref_ 索引映射
 */
void HeapTimer::swapNode(size_t i, size_t j)
{
    assert(i < heap_.size());
    assert(j < heap_.size());
    std::swap(heap_[i], heap_[j]);
    ref_[heap_[i].id] = i;
    ref_[heap_[j].id] = j;
}

/* ========================================================================
 *  公共接口
 * ======================================================================== */

/**
 * @brief 添加或更新定时器
 *
 *  新节点：在堆尾插入后 siftup
 *  已有节点：更新 expires + cb，向下或向上调整
 */
void HeapTimer::add(int id, int timeout, const TimeoutCallBack& cb)
{
    assert(id >= 0);
    size_t i;

    if(ref_.count(id) == 0)
    {
        // 新节点: 插入堆尾，上浮调整
        i = heap_.size();
        ref_[id] = i;
        heap_.push_back({id, Clock::now() + MS(timeout), cb});
        siftup(i);
    }
    else
    {
        // 已有节点: 更新时间和回调
        i = ref_[id];
        heap_[i].expires = Clock::now() + MS(timeout);
        heap_[i].cb = cb;

        // 先尝试下沉（时间可能变大了），如果没移动再上浮
        if(!siftdown(i, heap_.size()))
        {
            siftup(i);
        }
    }
}

/**
 * @brief 删除指定 id 的节点并执行其回调
 *
 *  关键：先 del_ 移除节点，再执行回调。
 *  防止回调（如 closeConn）修改堆导致错删或悬空。
 */
void HeapTimer::doWork(int id)
{
    if(heap_.empty() || ref_.count(id) == 0) { return; }

    size_t i = ref_[id];
    TimerNode node = heap_[i];
    del(i);               // 先从堆中移除
    node.cb();             // 再执行回调（安全）
}

/**
 * @brief 删除堆中位置 index 的节点
 *
 *  算法:
 *   1. 交换 index 和最后一个节点 (n)
 *   2. 以 n 为新大小限制，对 index 进行调整
 *   3. 删除尾节点
 */
void HeapTimer::del(size_t index)
{
    assert(!heap_.empty() && index < heap_.size());

    size_t i = index;
    size_t n = heap_.size() - 1;
    assert(i <= n);

    if(i < n)
    {
        // 交换到末尾
        swapNode(i, n);

        // 以 n (不含原尾节点) 为界限调整
        if(!siftdown(i, n))
        {
            siftup(i);
        }
    }

    // 删除原尾节点
    ref_.erase(heap_.back().id);
    heap_.pop_back();
}

/**
 * @brief 调整指定 id 的超时时间
 *
 *  更新 expires 为 now + timeout(ms)，然后下沉或上浮。
 */
void HeapTimer::adjust(int id, int timeout)
{
    assert(!heap_.empty() && ref_.count(id) > 0);

    size_t i = ref_[id];
    heap_[i].expires = Clock::now() + MS(timeout);

    // 先下沉后上浮 —— 时间可能变早也可能变晚
    if(!siftdown(i, heap_.size()))
    {
        siftup(i);
    }
}

/**
 * @brief 清理超时节点并触发回调
 *
 *  从堆顶开始，弹出所有 expires ≤ now 的节点。
 */
void HeapTimer::tick()
{
    if(heap_.empty()) { return; }

    while(!heap_.empty())
    {
        TimerNode node = heap_.front();
        // 以毫秒精度检查是否超时
        if(std::chrono::duration_cast<MS>(node.expires - Clock::now()).count() > 0)
        {
            break;           // 堆顶未超时 → 后面的也不会超时
        }

        pop();               // 先移除
        node.cb();           // 再执行回调
    }
}

void HeapTimer::pop()
{
    assert(!heap_.empty());
    del(0);                 // 删除堆顶
}

void HeapTimer::clear()
{
    ref_.clear();
    heap_.clear();
}

/**
 * @brief 返回距最近超时的 ms 数（用于 epoll_wait timeout）
 *
 *  先 tick() 清理已超时节点，再检查堆顶。
 *
 *  @return 距超时的 ms 数，-1 表示无定时器（epoll_wait 无限等待）
 */
int HeapTimer::getNextTick()
{
    tick();                  // 先清理超时节点

    if(!heap_.empty())
    {
        auto res = std::chrono::duration_cast<MS>(heap_.front().expires - Clock::now()).count();
        if(res < 0) { res = 0; }     // 刚超时 → 立即返回
        return static_cast<int>(res);
    }

    return -1;               // 无定时器 → 无限等待
}
