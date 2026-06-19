/**
 * @file    ThreadPool.h
 * @brief   固定大小线程池 —— 生产者-消费者模型
 * @details
 *
 *   设计要点:
 *
 *   1. shared_ptr<Pool> 手法:
 *      构造函数启动 N 个工作线程，lambda 通过 [pool = pool_] 捕获 Pool 的
 *      shared_ptr。即使 ThreadPool 对象被 move 或销毁，工作线程持有的
 *      pool 引用仍然有效，析构时通过 isClosed + notify_all + join 安全退出。
 *
 *   2. wait-loop 模式:
 *      while(true) {
 *          if(tasks not empty) → lock → pop → unlock → execute → relock
 *          else if(isClosed)   → break
 *          else                → cond.wait()
 *      }
 *      关键：执行任务时 unlock mtx，允许多个线程并发执行任务。
 *
 *   3. AddTask 线程安全:
 *      lock → emplace → unlock → notify_one
 *      只唤醒一个线程（而非 notify_all），减少惊群开销。
 *
 *   4. 禁止拷贝，允许移动:
 *      ThreadPool 只允许 std::move 转移所有权。
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>
#include <functional>
#include <assert.h>

class ThreadPool {
public:
    /**
     * @brief 创建线程池并启动工作线程
     * @param threadCount 工作线程数（默认 8）
     */
    explicit ThreadPool(size_t threadCount = 8): pool_(std::make_shared<Pool>()) {
        assert(threadCount > 0);

        for(size_t i = 0; i < threadCount; i++)
        {
            // 每个线程独立运行 worker loop
            std::thread worker([pool = pool_] {
                std::unique_lock<std::mutex> locker(pool->mtx);

                while(true)
                {
                    if(!pool->tasks.empty())
                    {
                        // 有任务: 取出并执行（执行时释放锁）
                        auto task = std::move(pool->tasks.front());
                        pool->tasks.pop();
                        locker.unlock();
                        task();
                        locker.lock();
                    }
                    else if(pool->isClosed)
                    {
                        break;              // 线程池关闭 → 退出
                    }
                    else
                    {
                        pool->cond.wait(locker);  // 无任务 → 等待唤醒
                    }
                }
            });

            pool_->threads.push_back(std::move(worker));
        }
    }

    /* ===== 生命周期管理 ===== */
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ThreadPool(ThreadPool&&) = default;
    ThreadPool& operator=(ThreadPool&&) = default;

    ~ThreadPool() {
        if(static_cast<bool>(pool_))
        {
            {
                std::lock_guard<std::mutex> locker(pool_->mtx);
                pool_->isClosed = true;
            }
            pool_->cond.notify_all();        // 唤醒所有线程通知关闭

            for(std::thread& t : pool_->threads)
            {
                if(t.joinable()) { t.join(); }
            }
        }
    }

    /**
     * @brief 向线程池提交任务
     * @tparam F 可调用对象类型
     * @param task 任务函数 (void())
     *
     *  任务被 emplace 到队列中，notify_one 唤醒一个工作线程执行。
     */
    template<class F>
    void AddTask(F&& task) {
        {
            std::lock_guard<std::mutex> locker(pool_->mtx);
            if(pool_->isClosed) { return; }
            pool_->tasks.emplace(std::forward<F>(task));
        }
        pool_->cond.notify_one();
    }

private:
    /** @brief 线程池内部数据（由 shared_ptr 管理，工作线程共享所有权） */
    struct Pool {
        std::mutex mtx;                           ///< 保护任务队列的互斥锁
        std::condition_variable cond;             ///< 工作线程的条件变量
        bool isClosed = false;                    ///< 关闭标志
        std::queue<std::function<void()>> tasks;  ///< 任务队列
        std::vector<std::thread> threads;         ///< 工作线程
    };

    std::shared_ptr<Pool> pool_;  ///< 线程安全的内部 Pool
};

#endif // THREADPOOL_H
