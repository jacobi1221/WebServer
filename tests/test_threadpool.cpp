/**
 * @file    test_threadpool.cpp
 * @brief   ThreadPool 线程池单元测试
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <vector>

#include "Pool/ThreadPool.h"

/* ================================================================
 *  基本任务提交
 * ================================================================ */
TEST(ThreadPoolTest, SingleTask) {
    ThreadPool pool(4);
    std::atomic<int> result{0};

    pool.AddTask([&]() { result = 42; });

    // 等待任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(result, 42);
}

TEST(ThreadPoolTest, MultipleTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int kNumTasks = 100;

    for(int i = 0; i < kNumTasks; i++) {
        pool.AddTask([&]() { counter++; });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_EQ(counter, kNumTasks);
}

/* ================================================================
 *  并发正确性
 * ================================================================ */
TEST(ThreadPoolTest, ConcurrentIncrement) {
    ThreadPool pool(8);
    std::atomic<int> counter{0};
    const int kNumTasks = 1000;

    for(int i = 0; i < kNumTasks; i++) {
        pool.AddTask([&]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_EQ(counter.load(), kNumTasks);   // 原子性保证最终一致
}

TEST(ThreadPoolTest, TasksExecuteInParallel) {
    const size_t kThreads = 4;
    ThreadPool pool(kThreads);

    std::atomic<int> maxConcurrent{0};
    std::atomic<int> running{0};

    // 提交阻塞任务，让所有线程都忙
    for(size_t i = 0; i < kThreads * 2; i++) {
        pool.AddTask([&]() {
            int cur = running.fetch_add(1) + 1;
            // 更新最大并发数
            int prev = maxConcurrent.load();
            while(cur > prev &&
                  !maxConcurrent.compare_exchange_weak(prev, cur))
                ;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            running.fetch_sub(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 至少应该有 > 1 的并发（证明多线程在工作）
    EXPECT_GE(maxConcurrent.load(), 2);
}

/* ================================================================
 *  Move 语义
 * ================================================================ */
TEST(ThreadPoolTest, MoveConstructor) {
    ThreadPool pool1(2);
    std::atomic<int> result{0};

    ThreadPool pool2 = std::move(pool1);
    pool2.AddTask([&]() { result = 99; });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(result, 99);
}

/* ================================================================
 *  析构安全
 * ================================================================ */
TEST(ThreadPoolTest, DestructorJoinsAllThreads) {
    // 创建并立即销毁线程池 —— 不应崩溃或 hang
    {
        ThreadPool pool(4);
        for(int i = 0; i < 10; i++) {
            pool.AddTask([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        }
    } // 析构时等待所有任务完成并 join
    SUCCEED();
}

TEST(ThreadPoolTest, DestructorWithPendingTasks) {
    std::atomic<int> completed{0};

    {
        ThreadPool pool(2);
        for(int i = 0; i < 100; i++) {
            pool.AddTask([&]() {
                completed++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        }
        // ~ThreadPool 应等待所有任务完成
    }

    EXPECT_EQ(completed, 100);  // 析构后所有任务应已完成
}

/* ================================================================
 *  性能 / 压力测试
 * ================================================================ */
TEST(ThreadPoolTest, StressTest) {
    ThreadPool pool(8);
    std::atomic<int64_t> sum{0};
    const int kNumTasks = 5000;

    for(int i = 0; i < kNumTasks; i++) {
        pool.AddTask([&sum, i]() {
            sum.fetch_add(i, std::memory_order_relaxed);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 0 + 1 + 2 + ... + 4999 = 5000*4999/2 = 12,497,500
    int64_t expected = static_cast<int64_t>(kNumTasks) * (kNumTasks - 1) / 2;
    EXPECT_EQ(sum.load(), expected);
}
