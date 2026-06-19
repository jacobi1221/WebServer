/**
 * @file    SqlConnRAII.h
 * @brief   MySQL 连接 RAII 守卫 —— 自动获取和归还连接
 * @details
 *
 *   使用 RAII (Resource Acquisition Is Initialization) 惯用法:
 *   - 构造时从连接池获取一个 MySQL 连接
 *   - 析构时自动归还连接到池中
 *
 *   确保无论正常返回还是异常抛出，连接都能正确归还。
 *
 *   典型用法:
 *   @code
 *     MYSQL* sql = nullptr;
 *     SqlConnRAII guard(&sql, SqlConnPool::instance());
 *     // ... 使用 sql 进行数据库操作 ...
 *   @endcode
 *   // guard 析构 → freeConn(sql)
 */

#ifndef SQLCONNRAII_H
#define SQLCONNRAII_H

#include "SqlConnPool.h"

class SqlConnRAII {
public:
    /**
     * @brief 从连接池获取连接
     * @param sql      传出参数: 连接指针
     * @param connpool 连接池（通常为 SqlConnPool::instance()）
     */
    SqlConnRAII(MYSQL** sql, SqlConnPool* connpool) {
        assert(connpool);
        *sql = connpool->getConn();
        sql_ = *sql;
        connpool_ = connpool;
    }

    /** @brief 归还连接到池中 */
    ~SqlConnRAII() {
        if(sql_ && connpool_) {
            connpool_->freeConn(sql_);
        }
    }

    /* ===== 禁止拷贝（连接所有权唯一） ===== */
    SqlConnRAII(const SqlConnRAII&) = delete;
    SqlConnRAII& operator=(const SqlConnRAII&) = delete;

private:
    MYSQL* sql_;                 ///< 持有的连接
    SqlConnPool* connpool_;      ///< 所属连接池
};

#endif // SQLCONNRAII_H
