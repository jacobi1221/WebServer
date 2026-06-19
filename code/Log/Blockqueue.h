/**
 * @file    Blockqueue.h
 * @brief   线程安全阻塞双端队列 —— 生产者-消费者模型的底层实现
 * @details
 *
 *   BlockDeque 是一个模板化的有界阻塞双端队列:
 *
 *   生产者 (push_back / push_front):
 *     - 队列满时 wait，直到消费者 pop 或 Close
 *     - push 后 notify 一个消费者
 *
 *   消费者 (pop):
 *     - 队列空时 wait，直到生产者 push 或 Close
 *     - pop 后 notify 一个生产者
 *
 *   特性:
 *     - 双条件变量 (condConsumer_ + condProducer_) 精确唤醒
 *     - Close() 安全通知所有等待线程退出
 *     - 支持超时 pop (带 timeout 的重载)
 *
 *   使用场景:
 *     - 异步日志: 多个线程 push 日志，一个后台线程 pop 写入文件
 */

#ifndef BLOCKQUEUE_H
#define BLOCKQUEUE_H

#include <mutex>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <assert.h>

template<class T>
class BlockDeque {
public:
    /** @param MaxCapacity 队列最大容量 */
    explicit BlockDeque(size_t MaxCapacity = 1000);
    ~BlockDeque();

    void clear();        ///< 清空队列
    bool empty();        ///< 队列是否为空
    bool full();         ///< 队列是否已满
    void Close();        ///< 关闭队列，通知所有等待线程退出
    size_t size();       ///< 当前元素数量
    size_t capacity();   ///< 最大容量

    T front();           ///< 队首元素
    T back();            ///< 队尾元素

    void push_back(const T& item);   ///< 生产者: 队尾插入（队列满则阻塞）
    void push_front(const T& item);  ///< 生产者: 队首插入（队列满则阻塞）

    bool pop(T& item);               ///< 消费者: 队首弹出（队列空则阻塞，Close 返回 false）
    bool pop(T& item, int timeout);  ///< 消费者: 带超时的弹出

    void flush();                    ///< 唤醒一个消费者线程

private:
    std::deque<T> deq_;              ///< 底层双端队列

    size_t capacity_;                ///< 最大容量

    std::mutex mtx_;                 ///< 互斥锁
    bool isClose_;                   ///< 队列关闭标志

    std::condition_variable condConsumer_; ///< 消费者条件变量
    std::condition_variable condProducer_; ///< 生产者条件变量
};


/* ========================================================================
 *  Template Implementations
 * ======================================================================== */

template<class T>
BlockDeque<T>::BlockDeque(size_t MaxCapacity) : capacity_(MaxCapacity) {
    assert(MaxCapacity > 0);
    isClose_ = false;
}

template<class T>
BlockDeque<T>::~BlockDeque() {
    Close();
}

/** @brief 关闭队列: 清空所有元素，通知所有等待线程 */
template<class T>
void BlockDeque<T>::Close() {
    {
        std::lock_guard<std::mutex> locker(mtx_);
        deq_.clear();
        isClose_ = true;
    }
    condProducer_.notify_all();   // 唤醒所有阻塞的生产者
    condConsumer_.notify_all();   // 唤醒所有阻塞的消费者
}

/** @brief 唤醒一个消费者线程（用于强制刷新缓冲） */
template<class T>
void BlockDeque<T>::flush() {
    condConsumer_.notify_one();
}

template<class T>
void BlockDeque<T>::clear() {
    std::lock_guard<std::mutex> locker(mtx_);
    deq_.clear();
}

template<class T>
T BlockDeque<T>::front() {
    std::lock_guard<std::mutex> locker(mtx_);
    assert(!deq_.empty());
    return deq_.front();
}

template<class T>
T BlockDeque<T>::back() {
    std::lock_guard<std::mutex> locker(mtx_);
    assert(!deq_.empty());
    return deq_.back();
}

template<class T>
size_t BlockDeque<T>::size() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size();
}

template<class T>
size_t BlockDeque<T>::capacity() {
    std::lock_guard<std::mutex> locker(mtx_);
    return capacity_;
}

/**
 * @brief 生产者: 队尾插入元素
 *
 *  如果队列已满，阻塞等待消费者 pop 释放空间。
 *  如果队列已关闭（isClose_），直接返回不插入。
 */
template<class T>
void BlockDeque<T>::push_back(const T& item) {
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.size() >= capacity_ && !isClose_) {
        condProducer_.wait(locker);       // 队列满 → 等待消费者
    }
    if(isClose_) { return; }
    deq_.push_back(item);
    condConsumer_.notify_one();           // 通知消费者
}

template<class T>
void BlockDeque<T>::push_front(const T& item) {
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.size() >= capacity_ && !isClose_) {
        condProducer_.wait(locker);
    }
    if(isClose_) { return; }
    deq_.push_front(item);
    condConsumer_.notify_one();
}

template<class T>
bool BlockDeque<T>::empty() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.empty();
}

template<class T>
bool BlockDeque<T>::full() {
    std::lock_guard<std::mutex> locker(mtx_);
    return deq_.size() >= capacity_;
}

/**
 * @brief 消费者: 队首弹出元素（阻塞版本）
 *
 *  队列空时阻塞等待，直到有生产者 push 或 Close()。
 *
 *  @return true 成功弹出, false 队列已关闭
 */
template<class T>
bool BlockDeque<T>::pop(T& item) {
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.empty()) {
        condConsumer_.wait(locker);        // 队列空 → 等待生产者
        if(isClose_) { return false; }     // 关闭 → 退出
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();            // 通知生产者（队列有空位）
    return true;
}

/**
 * @brief 消费者: 队首弹出元素（超时版本）
 *
 *  队列空时等待指定秒数，超时返回 false。
 *
 *  @param item    传出参数: 弹出的元素
 *  @param timeout 超时时间（秒）
 *  @return true 成功, false 超时或关闭
 */
template<class T>
bool BlockDeque<T>::pop(T& item, int timeout) {
    std::unique_lock<std::mutex> locker(mtx_);
    while(deq_.empty()) {
        if(condConsumer_.wait_for(locker, std::chrono::seconds(timeout))
                == std::cv_status::timeout) {
            return false;                  // 超时
        }
        if(isClose_) { return false; }     // 关闭
    }
    item = deq_.front();
    deq_.pop_front();
    condProducer_.notify_one();
    return true;
}

#endif // BLOCKQUEUE_H
