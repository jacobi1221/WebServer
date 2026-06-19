/**
 * @file    SqlConnPool.h
 * @brief   MySQL 连接池 —— 单例 + 信号量限流
 * @details
 *
 *   设计:
 *   - 单例模式 (Meyers Singleton, C++11 magic static)
 *   - sem_t 计数信号量做并发限流
 *   - std::mutex 保护连接队列
 *   - 预创建固定数量连接，用完归还
 *
 *   使用方式:
 *     SqlConnPool::instance()->getConn()   // 获取连接（可能阻塞）
 *     SqlConnPool::instance()->freeConn()  // 归还连接
 *
 *   推荐通过 SqlConnRAII 使用，自动归还连接。
 */

#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>

#include "../Log/Log.h"

class SqlConnPool {
public:
    /** @brief 获取单例 */
    static SqlConnPool* instance();

    /**
     * @brief 初始化连接池
     * @param host     MySQL 主机
     * @param port     MySQL 端口
     * @param user     用户名
     * @param pwd      密码
     * @param dbName   数据库名
     * @param connSize 连接池大小
     */
    void init(const char* host, int port,
              const char* user, const char* pwd,
              const char* dbName, int connSize);

    /**
     * @brief 获取一个连接（信号量限流，可能阻塞）
     * @return MySQL 连接句柄
     */
    MYSQL* getConn();

    /** @brief 归还连接到池中 */
    void freeConn(MYSQL* conn);

    /** @brief 获取空闲连接数 */
    int getFreeConnCount();

    /** @brief 关闭所有连接，释放资源 */
    void closePool();

private:
    SqlConnPool();
    ~SqlConnPool();

    int MAX_CONN_;               ///< 最大连接数

    std::queue<MYSQL*> connQue_; ///< 空闲连接队列
    std::mutex mtx_;             ///< 保护 connQue_ 的互斥锁
    sem_t semId_;                ///< 计数信号量（当前可用连接数）
};

#endif // SQLCONNPOOL_H
