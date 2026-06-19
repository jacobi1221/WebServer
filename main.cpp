/**
 * @file    main.cpp
 * @brief   程序入口 —— 命令行解析 + 信号处理 + 启动服务器
 *
 *   用法:
 *     ./TinyWebServer [options]
 *
 *   选项:
 *     -p PORT         监听端口 (1024-65535, 默认 8080)
 *     -m MODE         epoll 触发模式 (0=LT+LT, 1=ET+LT, 2=LT+ET, 3=ET+ET; 默认 3)
 *     -t TIMEOUT      连接超时秒数 (默认 60s, 0=禁用)
 *     -l              启用 SO_LINGER 优雅关闭
 *     -s SQL_PORT     MySQL 端口 (默认 3306)
 *     -u SQL_USER     MySQL 用户名 (默认 "root")
 *     -w SQL_PWD      MySQL 密码 (默认 "root")
 *     -d DB_NAME      MySQL 数据库名 (默认 "webserver")
 *     -c CONN_POOL    数据库连接池大小 (默认 8)
 *     -n THREAD_NUM   线程池大小 (默认 8)
 *     -g LOG_LEVEL    日志级别 (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR; 默认 1)
 *     -q LOG_QUEUE    异步日志队列容量 (默认 1024, 0=同步)
 *     -o              关闭日志
 *     -D              守护进程模式
 *     -h              显示帮助
 */

#include "Server/WebServer.h"

#include <iostream>
#include <string>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

// 全局指针，供信号处理使用
static WebServer* g_server = nullptr;

static void signalHandler(int sig) {
    if(g_server) {
        std::cout << "\n[INFO] Received signal " << sig << ", shutting down..."
                  << std::endl;
        // 可以在这里设置 isClose_，让事件循环优雅退出
    }
    exit(0);
}

static void setupSignals() {
    signal(SIGINT,  signalHandler);   // Ctrl+C
    signal(SIGTERM, signalHandler);   // kill
    signal(SIGPIPE, SIG_IGN);         // 忽略 SIGPIPE（send 会返回 -1）
}

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  -p PORT         监听端口 (1024-65535, 默认 8080)\n"
              << "  -m MODE         epoll 触发模式:\n"
              << "                    0 = LT listen + LT conn\n"
              << "                    1 = LT listen + ET conn\n"
              << "                    2 = ET listen + LT conn\n"
              << "                    3 = ET listen + ET conn (默认)\n"
              << "  -t TIMEOUT      连接超时秒数 (默认 60, 0=禁用超时)\n"
              << "  -l              启用 SO_LINGER 优雅关闭\n"
              << "  -s SQL_PORT     MySQL 端口 (默认 3306)\n"
              << "  -u SQL_USER     MySQL 用户名 (默认 \"root\")\n"
              << "  -w SQL_PWD      MySQL 密码 (默认 \"root\")\n"
              << "  -d DB_NAME      数据库名 (默认 \"webserver\")\n"
              << "  -c CONN_POOL    DB 连接池大小 (默认 8)\n"
              << "  -n THREAD_NUM   线程池大小 (默认 8)\n"
              << "  -g LOG_LEVEL    日志级别 0=DEBUG,1=INFO,2=WARN,3=ERROR (默认 1)\n"
              << "  -q LOG_QUEUE    异步日志队列容量 (默认 1024, 0=同步)\n"
              << "  -o              关闭日志\n"
              << "  -D              守护进程模式 (后台运行)\n"
              << "  -h              显示此帮助\n"
              << std::endl;
}

/**
 * @brief 进入守护进程模式
 *  1. fork 并退出父进程
 *  2. setsid 创建新会话
 *  3. 重定向 stdin/stdout/stderr 到 /dev/null
 *  4. 切换工作目录到 /
 */
static bool daemonize() {
    pid_t pid = fork();
    if(pid < 0)  { return false; }
    if(pid > 0)  { _exit(0); }           // 父进程退出

    setsid();                              // 创建新会话，脱离终端

    // 重定向标准 I/O 到 /dev/null
    int fd = open("/dev/null", O_RDWR);
    if(fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if(fd > STDERR_FILENO) close(fd);
    }

    chdir("/");                            // 避免占用文件系统挂载点
    umask(0);                              // 重置文件权限掩码
    return true;
}

int main(int argc, char* argv[])
{
    /* ===== 默认参数 ===== */
    int    port        = 8080;
    int    trigMode    = 3;       // 默认 ET + ET
    int    timeoutMS   = 60000;   // 60 秒
    bool   optLinger   = false;
    int    sqlPort     = 3306;
    const char* sqlUser   = "root";
    const char* sqlPwd    = "root";
    const char* dbName    = "webserver";
    int    connPoolNum = 8;
    int    threadNum   = 8;
    bool   openLog     = true;
    int    logLevel    = 1;       // INFO
    int    logQueueSize = 1024;
    bool   daemon      = false;

    /* ===== 命令行解析 ===== */
    int opt;
    while((opt = getopt(argc, argv, "p:m:t:ls:u:w:d:c:n:g:q:oDh")) != -1)
    {
        switch(opt)
        {
            case 'p': port       = std::stoi(optarg);   break;
            case 'm': trigMode   = std::stoi(optarg);   break;
            case 't': timeoutMS  = std::stoi(optarg) * 1000; break;  // 秒 → 毫秒
            case 'l': optLinger  = true;                 break;
            case 's': sqlPort    = std::stoi(optarg);   break;
            case 'u': sqlUser    = optarg;               break;
            case 'w': sqlPwd     = optarg;               break;
            case 'd': dbName     = optarg;               break;
            case 'c': connPoolNum = std::stoi(optarg);  break;
            case 'n': threadNum  = std::stoi(optarg);   break;
            case 'g': logLevel   = std::stoi(optarg);   break;
            case 'q': logQueueSize = std::stoi(optarg); break;
            case 'o': openLog    = false;                break;
            case 'D': daemon     = true;                 break;
            case 'h':
            default:
                printUsage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    /* ===== 参数校验 ===== */
    if(port < 1024 || port > 65535) {
        std::cerr << "[ERROR] Port must be in range [1024, 65535]" << std::endl;
        return 1;
    }
    if(trigMode < 0 || trigMode > 3) {
        std::cerr << "[ERROR] trigMode must be 0-3" << std::endl;
        return 1;
    }
    if(threadNum < 1) {
        std::cerr << "[ERROR] threadNum must be >= 1" << std::endl;
        return 1;
    }

    /* ===== 守护进程模式 ===== */
    if(daemon) {
        if(!daemonize()) {
            std::cerr << "[ERROR] Failed to daemonize" << std::endl;
            return 1;
        }
    }

    /* ===== 信号处理 ===== */
    setupSignals();

    /* ===== 启动服务器 ===== */
    std::cout << "TinyWebServer starting on port " << port << "..." << std::endl;

    WebServer server(port, trigMode, timeoutMS, optLinger,
                     sqlPort, sqlUser, sqlPwd, dbName,
                     connPoolNum, threadNum,
                     openLog, logLevel, logQueueSize);

    g_server = &server;

    server.start();

    return 0;
}
