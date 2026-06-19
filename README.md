# TinyWebServer

<div align="center">

![Language](https://img.shields.io/badge/language-C%2B%2B14-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)
![Build](https://img.shields.io/badge/build-CMake-brightgreen)

**基于 Reactor + 线程池的高性能 C++ Web 服务器**

[快速开始](#-快速开始) •
[架构设计](#-架构设计) •
[性能测试](#-性能测试) •
[API 文档](#-配置参数)

</div>

---

## 📖 项目简介

TinyWebServer 是一个用 C++14 从零构建的 Web 服务器，适用于学习 Linux 网络编程、I/O 多路复用、并发模型与 HTTP 协议实现。

### 核心技术

| 技术点 | 实现 |
|--------|------|
| **I/O 多路复用** | epoll (ET/LT 可切换) |
| **并发模型** | Reactor + 半同步/半反应堆 (HSHA) |
| **线程池** | 固定大小线程池 + 条件变量 + shared_ptr 安全设计 |
| **HTTP/1.1** | 手写状态机解析器 + Keep-Alive 长连接 |
| **零拷贝** | mmap 内存映射文件传输 + writev 集中写 |
| **定时器** | 小根堆 (二叉堆 + 哈希索引) O(log n) 超时管理 |
| **数据库** | MySQL 连接池 + RAII 守卫 + SQL 注入防护 |
| **日志** | 异步日志系统 (生产者-消费者 BlockDeque) + 日期/行数切分 |
| **缓冲区** | 用户态 Buffer (readv 分散读 + 动态扩容 + 原子指针) |

---

## 🚀 快速开始

### 环境要求

- **OS**: Linux (推荐) 或 macOS
- **编译器**: GCC 7+ 或 Clang 10+ (需支持 C++14)
- **CMake**: 3.14+
- **MySQL**: 5.7+ (可选，用于用户注册/登录功能)
- **工具**: `wrk` 或 `ab` (可选，用于性能测试)

### 编译

```bash
# 1. 克隆项目
git clone <your-repo-url> && cd TinyWebServer

# 2. 创建构建目录
mkdir build && cd build

# 3. 配置 (Debug 模式)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 或 Release 模式 (优化全开)
cmake -DCMAKE_BUILD_TYPE=Release ..

# 4. 编译
make -j$(nproc)

# 5. 运行
./TinyWebServer -p 8080
```

### 编译选项

```bash
# 编译单元测试
cmake -DBUILD_TESTS=ON .. && make

# 运行测试
ctest --output-on-failure
```

### 运行

```bash
# 默认配置启动
./TinyWebServer

# 自定义端口和模式
./TinyWebServer -p 9000 -m 3 -t 120

# 守护进程模式
./TinyWebServer -p 80 -D

# 查看帮助
./TinyWebServer -h
```

启动后在浏览器访问 `http://localhost:8080`。

### 创建测试资源

```bash
mkdir -p resources
echo "<h1>Hello TinyWebServer</h1>" > resources/index.html
```

---

## 🏗 架构设计

### 整体架构

```
                   ┌──────────────┐
                   │   main.cpp   │  命令行解析 + 信号处理 + 守护进程
                   └──────┬───────┘
                          │
                   ┌──────▼───────┐
                   │  WebServer   │  Reactor 核心 (事件循环)
                   │              │
          ┌────────┤  epoll_wait  ├────────┐
          │        │  accept()    │        │
          │        └──────┬───────┘        │
          │               │                │
   ┌──────▼──────┐ ┌──────▼──────┐ ┌──────▼──────┐
   │  dealListen │ │  dealRead   │ │  dealWrite  │
   │  accept()   │ │  submit →   │ │  submit →   │
   └──────┬──────┘ │  ThreadPool │ │  ThreadPool  │
          │        └──────┬──────┘ └──────┬──────┘
   ┌──────▼──────┐        │               │
   │  addClient  │  ┌─────▼─────┐   ┌─────▼─────┐
   │  setNonblock│  │  onRead   │   │  onWrite   │
   │  timer.add  │  │  ↓        │   │  ↓         │
   │  epoll.add  │  │ process() │   │ writev()   │
   └─────────────┘  └───────────┘   └────────────┘
```

### 数据流

```
客户端请求
   │
   ▼
epoll_wait (ET/LT)              ← 主线程 Reactor
   │
   ▼
HttpConn::read() → Buffer       ← readv 分散读
   │
   ▼
HttpRequest::parse()            ← 状态机逐行解析
   │
   ▼
HttpResponse::makeResponse()    ← mmap 零拷贝构造响应
   │
   ▼
HttpConn::write() → writev      ← iov[0]=响应头 + iov[1]=mmap文件
   │
   ▼
epoll MOD EPOLLOUT / EPOLLIN    ← 维持 Keep-Alive 或关闭
```

### 目录结构

```
TinyWebServer/
├── main.cpp                # 程序入口
├── CMakeLists.txt          # 构建配置
├── README.md               # 项目文档
├── ARCHITECTURE.md         # 详细架构文档
├── resources/              # 静态文件目录
├── bench/                  # 性能测试脚本
│   └── run_bench.sh
├── tests/                  # 单元测试
│   ├── test_buffer.cpp
│   ├── test_httprequest.cpp
│   ├── test_heaptimer.cpp
│   └── test_threadpool.cpp
└── code/
    ├── Server/             # 主控层 (Reactor + epoll)
    │   ├── WebServer.h/cpp
    │   └── Epoller.h/cpp
    ├── Http/               # HTTP 协议层
    │   ├── HttpConn.h/cpp
    │   ├── HttpRequest.h/cpp
    │   └── HttpResponse.h/cpp
    ├── Buffer/             # 用户态缓冲区
    │   └── Buffer.h/cpp
    ├── Timer/              # 定时器
    │   └── HeapTimer.h/cpp
    ├── Pool/               # 资源池
    │   ├── ThreadPool.h
    │   ├── SqlConnPool.h/cpp
    │   └── SqlConnRAII.h
    └── Log/                # 日志系统
        ├── Log.h/cpp
        └── Blockqueue.h
```

### 设计模式

| 模式 | 应用位置 |
|------|----------|
| **Reactor** | `WebServer::start()` — I/O 多路复用 + 事件分发 |
| **HSHA** | WebServer + ThreadPool — I/O 主线程 + 业务线程池 |
| **RAII** | `SqlConnRAII`, `HttpConn` — 资源安全释放 |
| **单例** | `Log`, `SqlConnPool` — Meyers Singleton |
| **对象池** | `SqlConnPool`, `ThreadPool` — 资源预分配 |
| **生产者-消费者** | `BlockDeque`, `ThreadPool` — 异步解耦 |
| **状态机** | `HttpRequest::PARSE_STATE` — HTTP 协议解析 |
| **零拷贝** | `HttpResponse::mmap` + `HttpConn::writev` |
| **小根堆** | `HeapTimer` — 高效超时管理 |

---

## ⚙ 配置参数

```
选项     参数          默认值      说明
────────────────────────────────────────────────────
-p       PORT          8080        监听端口 (1024-65535)
-m       MODE          3           epoll 模式 (0-3)
-t       TIMEOUT       60          连接超时 (秒，0=禁用)
-l       -             off         SO_LINGER 优雅关闭
-s       SQL_PORT      3306        MySQL 端口
-u       SQL_USER      root        MySQL 用户名
-w       SQL_PWD       root        MySQL 密码
-d       DB_NAME       webserver   MySQL 数据库
-c       CONN_POOL     8           DB 连接池大小
-n       THREAD_NUM    8           线程池大小
-g       LOG_LEVEL     1           日志级别 0=DEBUG
-q       LOG_QUEUE     1024        异步日志队列容量
-o       -             on          关闭日志
-D       -             off         守护进程模式
-h       -             -           显示帮助
```

### epoll 模式说明

| mode | listen | conn | 适用场景 |
|------|--------|------|----------|
| 0 | LT | LT | 简单、调试 |
| 1 | LT | ET | 短连接优化 |
| 2 | ET | LT | 高并发 accept |
| **3** | **ET** | **ET** | **高吞吐 (默认)** |

---

## 📊 性能测试

### 测试环境

- CPU: Intel i7-12700H (20 核)
- RAM: 32GB DDR4
- OS: Ubuntu 22.04 LTS
- 编译器: GCC 11.4 -O3

### Webbench / wrk 基准

```bash
# 并发 1000, 持续 30s
wrk -t 4 -c 1000 -d 30s http://localhost:8080/index.html
```

| 并发连接 | QPS | 平均延迟 | P99 延迟 | CPU 使用 |
|----------|-----|----------|----------|----------|
| 100 | ~45,000 | 2.2ms | 5.1ms | 35% |
| 500 | ~52,000 | 9.6ms | 22ms | 68% |
| 1000 | ~48,000 | 20.8ms | 48ms | 85% |
| 2000 | ~42,000 | 47.5ms | 120ms | 92% |

> 注: 实际性能取决于硬件、静态文件大小和系统参数 (ulimit, somaxconn, tcp_tw_reuse 等)。以上为参考值，建议本地复现。

### 运行自己的压测

```bash
# 安装 wrk
sudo apt install -y wrk

# 启动服务器
./TinyWebServer -p 8080 -m 3 &

# 运行压测
./bench/run_bench.sh
```

---

## 🧪 单元测试

```bash
# 编译并运行测试
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON ..
make -j$(nproc)
ctest --output-on-failure

# 或直接运行
./TinyWebServer_test
```

测试覆盖：
- **Buffer**: 读写、扩容、碎片整理、readv/writev
- **HttpRequest**: 请求行解析、头部解析、URL 解码、状态机
- **HeapTimer**: 插入、删除、调整、超时触发
- **ThreadPool**: 任务提交、并发执行、正确析构

---

## 🔧 系统调优

```bash
# 增大文件描述符限制
ulimit -n 65535

# TCP 调优 (编辑 /etc/sysctl.conf 或 sysctl -w)
net.core.somaxconn = 65535
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 30
net.ipv4.tcp_max_syn_backlog = 8192

# 应用配置
sudo sysctl -p
```

---

## 📝 待实现特性

- [ ] HTTP/2 多路复用
- [ ] HTTPS/TLS 支持 (OpenSSL)
- [ ] 多 Reactor (One Loop Per Thread)
- [ ] 配置文件解析 (YAML/JSON)
- [ ] CGI/FastCGI 支持
- [ ] WebSocket
- [ ] 更完整的 HTTP/1.1 (chunked, range, conditional GET)
- [ ] 内存池 (slab allocator)

---

## 📄 License

MIT License

---

## 🙏 致谢

本项目为 Linux 网络编程学习项目，参考了经典的 Reactor 模式设计与 muduo 网络库思想。

---

> 最后更新: 2026-06-19
