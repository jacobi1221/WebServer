/**
 * @file    Log.cpp
 * @brief   Log 实现 —— 格式化 + 日期切分 + 异步刷盘
 */

#include "Log.h"
#include <assert.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <string>

using namespace std;

Log::Log() {
    lineCount_ = 0;
    isAsync_ = false;
    writeThread_ = nullptr;
    deque_ = nullptr;
    toDay_ = 0;
    fp_ = nullptr;
    level_ = 0;
    isOpen_ = false;
}

Log::~Log() {
    // 先等待异步队列排空再关闭
    if(writeThread_ && writeThread_->joinable())
    {
        while(!deque_->empty())
        {
            deque_->flush();           // 唤醒消费者线程处理积压日志
        };
        deque_->Close();               // 通知消费者退出
        writeThread_->join();
    }

    if(fp_)
    {
        lock_guard<mutex> locker(mtx_);
        flush();
        fclose(fp_);
    }
}

/* ===== 日志级别 ===== */

int Log::GetLevel() {
    lock_guard<mutex> locker(mtx_);
    return level_;
}

void Log::SetLevel(int level) {
    lock_guard<mutex> locker(mtx_);
    level_ = level;
}

/* ========================================================================
 *  初始化
 * ======================================================================== */

/**
 * @brief 初始化日志系统
 *
 *  如果 maxQueueSize > 0，开启异步模式:
 *    - 创建 BlockDeque（生产者-消费者队列）
 *    - 启动 FlushLogThread 后台线程
 *
 *  文件名格式: path_/YYYY_MM_DDsuffix_
 */
void Log::init(int level, const char* path, const char* suffix,
    int maxQueueSize)
{
    isOpen_ = true;
    level_ = level;

    if(maxQueueSize > 0)
    {
        // ---- 异步模式 ----
        isAsync_ = true;
        if(!deque_)
        {
            unique_ptr<BlockDeque<std::string>> newDeque(new BlockDeque<std::string>);
            deque_ = move(newDeque);

            std::unique_ptr<std::thread> NewThread(new thread(FlushLogThread));
            writeThread_ = move(NewThread);
        }
    }
    else
    {
        isAsync_ = false;
    }

    lineCount_ = 0;

    // 计算今天的日志文件名
    time_t timer = time(nullptr);
    struct tm* sysTime = localtime(&timer);
    struct tm t = *sysTime;

    path_ = path;
    suffix_ = suffix;
    char fileName[LOG_NAME_LEN] = {0};
    snprintf(fileName, LOG_NAME_LEN - 1, "%s/%04d_%02d_%02d%s",
            path_, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, suffix_);
    toDay_ = t.tm_mday;

    {
        lock_guard<mutex> locker(mtx_);
        buff_.retrieveAll();

        // 如果之前有打开的文件，先关闭
        if(fp_) {
            flush();
            fclose(fp_);
        }

        // 打开（或创建）日志文件
        fp_ = fopen(fileName, "a");
        if(fp_ == nullptr)
        {
            mkdir(path_, 0755);            // 安全权限
            fp_ = fopen(fileName, "a");
        }
        assert(fp_ != nullptr);
    }
}

/* ========================================================================
 *  写日志
 * ======================================================================== */

/**
 * @brief 格式化并写入一条日志
 *
 *  流程:
 *   1. 日期切分检查（跨天 or 超行）
 *   2. 拼接时间戳前缀
 *   3. 拼接日志级别标签
 *   4. vsnprintf 格式化用户消息
 *   5. isAsync → push 到队列（异步）
 *      !isAsync → fwrite 直接写文件（同步）
 *
 *  @param level  日志级别
 *  @param format printf 格式字符串
 *  @param ...    可变参数列表
 */
void Log::write(int level, const char* format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr);
    time_t tSec = now.tv_sec;
    struct tm* sysTime = localtime(&tSec);
    struct tm t = *sysTime;
    va_list vaList;

    /* ---- 日期 / 行数切分检查 ---- */
    if(toDay_ != t.tm_mday || (lineCount_ && (lineCount_ % MAX_LINES == 0)))
    {
        char newFile[LOG_NAME_LEN];
        char tail[36] = {0};
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        if(toDay_ != t.tm_mday)
        {
            // 跨天: 创建新日期文件
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s%s", path_, tail, suffix_);
        }
        else
        {
            // 达到行数上限: 创建带序号的切分文件
            snprintf(newFile, LOG_NAME_LEN - 72, "%s/%s-%d%s", path_, tail, (lineCount_ / MAX_LINES), suffix_);
        }

        {
            lock_guard<mutex> locker(mtx_);
            if(toDay_ != t.tm_mday) {
                toDay_ = t.tm_mday;
                lineCount_ = 0;
            }
            flush();
            fclose(fp_);
            fp_ = fopen(newFile, "a");
            assert(fp_ != nullptr);
        }
    }

    /* ---- 格式化日志内容 ---- */
    {
        unique_lock<mutex> locker(mtx_);
        lineCount_++;

        // 时间戳: YYYY-MM-DD HH:MM:SS.μs
        int n = snprintf(buff_.beginWrite(), buff_.writableBytes(),
                    "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);

        buff_.hasWritten(n);
        AppendLogLevelTitle_(level);          // [debug]: / [info] : / ...

        // 用户消息（vsnprintf 格式化）
        va_start(vaList, format);
        int m = vsnprintf(buff_.beginWrite(), buff_.writableBytes(), format, vaList);
        va_end(vaList);

        buff_.hasWritten(m);
        buff_.append("\n", 1);

        if(isAsync_ && deque_ && !deque_->full())
        {
            // 异步模式: push 到队列，由后台线程异步刷盘
            deque_->push_back(buff_.retrieveAllToStr());
        }
        else
        {
            // 同步模式: 直接写文件
            fwrite(buff_.peek(), 1, buff_.readableBytes(), fp_);
            buff_.retrieveAll();
        }
    }
}

/**
 * @brief 拼接日志级别标签
 *
 *  [debug]:  /  [info] :  /  [warn] :  /  [error]:
 */
void Log::AppendLogLevelTitle_(int level) {
    switch(level) {
    case 0:
        buff_.append("[debug]: ", 9);
        break;
    case 1:
        buff_.append("[info] : ", 9);
        break;
    case 2:
        buff_.append("[warn] : ", 9);
        break;
    case 3:
        buff_.append("[error]: ", 9);
        break;
    default:
        buff_.append("[info] : ", 9);
        break;
    }
}

/* ========================================================================
 *  刷新 & 异步写入
 * ======================================================================== */

void Log::flush() {
    if(isAsync_) {
        deque_->flush();         // 唤醒消费者线程
    }
    fflush(fp_);                 // 将 FILE* 缓冲区刷到内核
}

/**
 * @brief 异步写线程的主循环
 *
 *  从 BlockDeque pop 日志字符串，fputs 写入文件。
 *  BlockDeque::pop 是阻塞操作，队列空时线程 sleep。
 */
void Log::AsyncWrite_() {
    string str = "";
    while(deque_->pop(str))      // 阻塞等待日志
    {
        lock_guard<mutex> locker(mtx_);
        fputs(str.c_str(), fp_);
    }
}

/* ===== 单例 ===== */

Log* Log::Instance() {
    static Log inst;             // C++11 magic static: 线程安全懒初始化
    return &inst;
}

void Log::FlushLogThread() {
    Log::Instance()->AsyncWrite_();
}
