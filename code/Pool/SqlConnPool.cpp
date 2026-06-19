/**
 * @file    SqlConnPool.cpp
 * @brief   SqlConnPool 实现 —— MySQL 连接池生命周期管理
 */

#include "SqlConnPool.h"

using namespace std;

SqlConnPool::SqlConnPool() {
    MAX_CONN_ = 0;
}

/**
 * @brief Meyers Singleton —— C++11 magic static 保证线程安全的懒初始化
 */
SqlConnPool* SqlConnPool::instance() {
    static SqlConnPool connPool;
    return &connPool;
}

/**
 * @brief 初始化连接池 —— 预创建 connSize 个 MySQL 连接
 *
 *  连接失败时跳过当前连接继续创建（服务降级，不因部分连接失败而崩溃）。
 */
void SqlConnPool::init(const char* host, int port,
            const char* user, const char* pwd, const char* dbName,
            int connSize)
{
    assert(connSize > 0);

    for(int i = 0; i < connSize; i++)
    {
        MYSQL* sql = mysql_init(nullptr);
        if(!sql)
        {
            LOG_ERROR("MySql init error!");
            continue;
        }

        sql = mysql_real_connect(sql, host,
                                 user, pwd,
                                 dbName, port, nullptr, 0);
        if(!sql)
        {
            LOG_ERROR("MySql Connect error!");
            continue;
        }

        connQue_.push(sql);
    }

    MAX_CONN_ = static_cast<int>(connQue_.size());

    // 初始化 POSIX 计数信号量，初始值 = 可用连接数
    sem_init(&semId_, 0, MAX_CONN_);
}

/**
 * @brief 获取连接 —— sem_wait 限流，mutex 保护队列
 *
 *  先 sem_wait（如果没有可用连接则阻塞等待），
 *  再持有 mutex 从队列取出。
 *
 *  @return MySQL 连接指针
 */
MYSQL* SqlConnPool::getConn()
{
    sem_wait(&semId_);               // P 操作: 等待可用连接
    MYSQL* sql = nullptr;
    {
        lock_guard<mutex> locker(mtx_);
        if(!connQue_.empty())
        {
            sql = connQue_.front();
            connQue_.pop();
        }
    }
    return sql;
}

/**
 * @brief 归还连接
 *
 *  push 回队列 + sem_post (V 操作)。
 */
void SqlConnPool::freeConn(MYSQL* sql)
{
    assert(sql);
    lock_guard<mutex> locker(mtx_);
    connQue_.push(sql);
    sem_post(&semId_);               // V 操作: 通知等待的线程
}

/**
 * @brief 关闭连接池 —— 释放所有 MySQL 连接
 */
void SqlConnPool::closePool()
{
    lock_guard<mutex> locker(mtx_);
    while(!connQue_.empty())
    {
        auto item = connQue_.front();
        connQue_.pop();
        mysql_close(item);
    }
    mysql_library_end();             // MySQL 库清理
    sem_destroy(&semId_);            // 销毁信号量
}

int SqlConnPool::getFreeConnCount()
{
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}

SqlConnPool::~SqlConnPool()
{
    closePool();
}
