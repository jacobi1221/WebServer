/**
 * @file    Log.h
 * @brief   异步日志系统 —— 支持日期切分 + 行数切分
 * @details
 *
 *   日志系统特性:
 *   - 单例模式 (Meyers Singleton)
 *   - 支持同步/异步写入（异步模式使用 BlockDeque + 后台线程）
 *   - 按日期切分: 每天生成新日志文件
 *   - 按行数切分: 每 50000 行生成新日志文件
 *   - 4 个日志级别: DEBUG(0) < INFO(1) < WARN(2) < ERROR(3)
 *
 *   异步模式:
 *   - write() 将格式化后的日志 push 到 BlockDeque
 *   - FlushLogThread 线程从 BlockDeque pop 并 fputs 写入文件
 *
 *   线程安全:
 *   - 写操作通过 std::mutex 保护
 *   - BlockDeque 自身是线程安全的（生产者-消费者）
 *
 *   用法:
 *     LOG_INFO("Port:%d", port);  → [2026-06-19 14:30:00.123456][info ]: Port:8080
 */

#ifndef LOG_H
#define LOG_H

#include <mutex>
#include <thread>
#include <sys/time.h>
#include <stdio.h>
#include "Blockqueue.h"
#include "../Buffer/Buffer.h"

class Log {
public:
    /**
     * @brief 初始化日志系统
     * @param level            日志级别 (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
     * @param path             日志文件路径
     * @param suffix           日志文件后缀
     * @param maxQueueCapacity 异步队列容量 (>0 开启异步, ≤0 同步)
     */
    void init(int level, const char* path = "./log",
              const char* suffix = ".log",
              int maxQueueCapacity = 1024);

    /** @brief 获取单例 */
    static Log* Instance();

    /** @brief 后台写线程的入口（静态，调用 AsyncWrite_） */
    static void FlushLogThread();

    /**
     * @brief 写入一条日志
     * @param level  日志级别
     * @param format printf 格式字符串
     * @param ...    可变参数
     */
    void write(int level, const char* format, ...);

    /** @brief 刷新日志缓冲区到磁盘 */
    void flush();

    int GetLevel();
    void SetLevel(int level);
    bool IsOpen() { return isOpen_; }

private:
    Log();
    virtual ~Log();

    /** @brief 拼接日志级别标签 [debug] / [info] / [warn] / [error] */
    void AppendLogLevelTitle_(int level);

    /** @brief 异步写线程的主循环: 从 BlockDeque pop 日志并写入文件 */
    void AsyncWrite_();

    /* ===== 配置常量 ===== */
    static const int LOG_PATH_LEN = 256;   ///< 日志路径最大长度
    static const int LOG_NAME_LEN = 256;   ///< 日志文件名最大长度
    static const int MAX_LINES    = 50000; ///< 每个日志文件最大行数

    /* ===== 文件信息 ===== */
    const char* path_;      ///< 日志目录
    const char* suffix_;    ///< 日志后缀 (.log)

    int lineCount_;         ///< 当前文件行数计数器
    int toDay_;             ///< 当前日期 (日，用于判断跨天)

    bool isOpen_;           ///< 日志系统是否开启

    Buffer buff_;           ///< 格式化缓冲区
    int level_;             ///< 当前日志级别
    bool isAsync_;          ///< 是否异步模式

    FILE* fp_;              ///< 日志文件句柄

    /** @brief 异步日志使用的阻塞队列 + 后台写线程 */
    std::unique_ptr<BlockDeque<std::string>> deque_;
    std::unique_ptr<std::thread> writeThread_;

    std::mutex mtx_;        ///< 互斥锁（保护缓冲区/文件写操作）
};

/* ========================================================================
 *  日志宏
 * ======================================================================== */

#define LOG_BASE(level, format, ...) \
    do { \
        Log* log = Log::Instance(); \
        if(log->IsOpen() && log->GetLevel() <= level) { \
            log->write(level, format, ##__VA_ARGS__); \
            log->flush(); \
        } \
    } while(0);

#define LOG_DEBUG(format, ...) do { LOG_BASE(0, format, ##__VA_ARGS__) } while(0);
#define LOG_INFO(format, ...)  do { LOG_BASE(1, format, ##__VA_ARGS__) } while(0);
#define LOG_WARN(format, ...)  do { LOG_BASE(2, format, ##__VA_ARGS__) } while(0);
#define LOG_ERROR(format, ...) do { LOG_BASE(3, format, ##__VA_ARGS__) } while(0);

#endif // LOG_H
