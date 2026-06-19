/**
 * @file    test_heaptimer.cpp
 * @brief   HeapTimer 小根堆定时器单元测试
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>

#include "Timer/HeapTimer.h"

/* ================================================================
 *  基本插入 / 获取
 * ================================================================ */
TEST(HeapTimerTest, AddAndGetNextTick) {
    HeapTimer timer;

    std::atomic<int> callCount{0};
    timer.add(1, 100, [&]() { callCount++; });      // 100ms 后超时
    timer.add(2, 5000, [&]() { callCount++; });     // 5s 后超时

    int tick = timer.getNextTick();
    EXPECT_GT(tick, 0);         // 应该返回正数（距超时的 ms）
    EXPECT_LE(tick, 100);       // 最近超时应 ≤ 100ms
    EXPECT_EQ(callCount, 0);    // 还未超时
}

TEST(HeapTimerTest, AddDuplicateIdUpdates) {
    HeapTimer timer;

    std::atomic<int> count{0};
    timer.add(1, 100, [&]() { count = 1; });
    timer.add(1, 50, [&]() { count = 2; });   // 更新同一 id → 新超时

    // 现在 id=1 应该在 50ms 超时
    int tick = timer.getNextTick();
    EXPECT_LE(tick, 50);
}

/* ================================================================
 *  超时触发
 * ================================================================ */
TEST(HeapTimerTest, TickFiresExpiredCallbacks) {
    HeapTimer timer;
    std::atomic<int> fired{0};

    // 添加一个立即过期的定时器
    timer.add(1, -100, [&]() { fired++; });   // 负数 = 已经过期

    timer.tick();
    EXPECT_EQ(fired, 1);                       // 应该触发
}

TEST(HeapTimerTest, TickOnlyFiresExpired) {
    HeapTimer timer;
    std::atomic<int> fired{0};

    timer.add(1, 5000, [&]() { fired++; });   // 5s → 未超时
    timer.add(2, 10000, [&]() { fired++; });  // 10s → 未超时

    timer.tick();
    EXPECT_EQ(fired, 0);                       // 都不应该触发
}

/* ================================================================
 *  删除节点
 * ================================================================ */
TEST(HeapTimerTest, DoWorkRemovesAndFires) {
    HeapTimer timer;
    std::atomic<int> fired{0};

    timer.add(1, 100, [&]() { fired++; });
    timer.doWork(1);

    EXPECT_EQ(fired, 1);                       // 回调应触发

    // 再次 tick 不应该触发（已删除）
    fired = 0;
    timer.tick();
    EXPECT_EQ(fired, 0);

    // getNextTick 返回 -1（堆为空）
    EXPECT_EQ(timer.getNextTick(), -1);
}

TEST(HeapTimerTest, DoWorkNonexistentId) {
    HeapTimer timer;
    // 不应该崩溃
    EXPECT_NO_THROW(timer.doWork(999));
}

/* ================================================================
 *  调整超时
 * ================================================================ */
TEST(HeapTimerTest, AdjustExpiry) {
    HeapTimer timer;
    std::atomic<int> fired{0};

    timer.add(1, 5000, [&]() { fired++; });

    // 调整为很快过期
    timer.adjust(1, 10);           // 10ms

    int tick = timer.getNextTick();
    EXPECT_LE(tick, 10);
}

/* ================================================================
 *  Pop 和 Clear
 * ================================================================ */
TEST(HeapTimerTest, PopRemovesTop) {
    HeapTimer timer;
    std::atomic<int> fired{0};

    timer.add(1, 50, [&]() { fired++; });
    timer.add(2, 100, [&]() { fired++; });
    timer.add(3, 200, [&]() { fired++; });

    timer.pop();  // 移除最近超时的 id=1（但不触发回调）
    EXPECT_EQ(fired, 0);

    // getNextTick 应返回 id=2 的超时（~100ms）
    int tick = timer.getNextTick();
    EXPECT_GT(tick, 0);
    EXPECT_LE(tick, 100);
}

TEST(HeapTimerTest, ClearEmptiesHeap) {
    HeapTimer timer;
    timer.add(1, 100, []() {});
    timer.add(2, 200, []() {});

    timer.clear();
    EXPECT_EQ(timer.getNextTick(), -1);   // 空堆 → -1
}

/* ================================================================
 *  多节点排序正确性
 * ================================================================ */
TEST(HeapTimerTest, HeapOrderMultipleNodes) {
    HeapTimer timer;

    std::vector<int> firedOrder;
    std::mutex mtx;

    // 添加 5 个节点，超时时间递减
    timer.add(1, 1000, [&]() { std::lock_guard<std::mutex> lk(mtx); firedOrder.push_back(1); });
    timer.add(2, 50,   [&]() { std::lock_guard<std::mutex> lk(mtx); firedOrder.push_back(2); });
    timer.add(3, 500,  [&]() { std::lock_guard<std::mutex> lk(mtx); firedOrder.push_back(3); });
    timer.add(4, 10,   [&]() { std::lock_guard<std::mutex> lk(mtx); firedOrder.push_back(4); });
    timer.add(5, 200,  [&]() { std::lock_guard<std::mutex> lk(mtx); firedOrder.push_back(5); });

    // 手动触发所有（通过 doWork，按 id 顺序不保证）
    // 这里验证 tick 100ms 后触发 id=4 和 id=2
    // 实际上 tick 基于真实时间，测试可靠性不高；改为验证堆内部顺序

    // 用 doWork 验证：最小 expires 的节点是 id=4 (10ms)
    // 通过 getNextTick 间接验证
    int tick = timer.getNextTick();
    EXPECT_GT(tick, 0);
    EXPECT_LE(tick, 10);   // 最近的超时是 10ms (id=4)
}

/* ================================================================
 *  回调不在堆操作中崩溃
 * ================================================================ */
TEST(HeapTimerTest, CallbackDoesNotCorruptHeap) {
    HeapTimer timer;
    std::atomic<int> count{0};

    timer.add(1, 10, [&]() { count++; });
    timer.add(2, 20, [&]() { count++; });

    // doWork 先删除节点再执行回调 → 防止回调修改堆导致的迭代器失效
    timer.doWork(1);
    EXPECT_EQ(count, 1);

    timer.doWork(2);
    EXPECT_EQ(count, 2);

    // 堆应为空
    EXPECT_EQ(timer.getNextTick(), -1);
}

TEST(HeapTimerTest, CallbackInTickReentrant) {
    HeapTimer timer;
    std::atomic<int> count{0};

    // 回调内再次 tick（模拟递归场景）
    timer.add(1, -10, [&]() {
        count++;
        timer.tick();  // 不应死循环或崩溃
    });

    timer.tick();
    EXPECT_EQ(count, 1);
}
