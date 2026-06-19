# TinyWebServer 架构知识框架

## 一、项目概览

```
TinyWebServer — 基于 Reactor + 线程池 + 非阻塞 I/O 的 C++14 高性能 Web 服务器。

吞吐模型：One Loop Per Thread + 半同步/半反应堆（HSHA）
并发模式：epoll (ET/LT) + 线程池 + 数据库连接池
内存管理：mmap 零拷贝文件传输 + 用户态读写缓冲区
连接管理：小根堆定时器踢除空闲连接
```

## 二、目录结构

```
WebServer/code/
├── Server/        主控层：事件循环、监听、分发
│   ├── WebServer.h/cpp    — 服务器主类，Reactor 核心
│   └── Epoller.h/cpp      — epoll 封装
├── Http/          HTTP 协议层
│   ├── HttpConn.h/cpp     — 连接对象（读写缓冲区 + 解析 + 响应）
│   ├── HttpRequest.h/cpp  — HTTP 请求解析器（状态机）
│   └── HttpResponse.h/cpp — HTTP 响应构造器（mmap 静态文件）
├── Buffer/        缓冲区
│   └── Buffer.h/cpp       — 用户态读写缓冲区（readv 分散读 + 动态扩容）
├── Timer/         定时器
│   └── HeapTimer.h/cpp    — 小根堆定时器（O(log n) 插入/删除/调整）
├── Pool/          资源池
│   ├── ThreadPool.h       — 线程池（std::queue + 条件变量）
│   ├── SqlConnPool.h/cpp  — MySQL 连接池（semaphore + mutex）
│   └── SqlConnRAII.h      — 数据库连接 RAII 守卫
└── Log/           日志系统
    ├── Log.h/cpp          — 异步日志（level + 日期切分 + 行数切分）
    └── Blockqueue.h       — 阻塞双端队列（生产者-消费者）
```

## 三、数据流全景

```
                               ┌──────────────────────────────────────────────────┐
                               │                   main()                         │
                               │   WebServer(start)  → 事件循环                   │
                               └──────────┬───────────────────────────────────────┘
                                          │ epoll_wait
                         ┌────────────────┼────────────────┐
                         ▼                ▼                ▼
                    listenFd          EPOLLIN           EPOLLOUT
                   有新连接            可读事件           可写事件
                         │                │                │
                         ▼                ▼                ▼
                   dealListen()      dealRead()        dealWrite()
                    accept()          submit to         submit to
                         │           ThreadPool        ThreadPool
                         ▼                │                │
                   addClient()      ┌─────┴─────┐    ┌─────┴─────┐
                    setNonblock     ▼           ▼    ▼           ▼
                    timer.add()   onRead()    ...  onWrite()   ...
                    epoll.add()      │                │
                                     ▼                ▼
                              HttpConn::read()  HttpConn::write()
                              readv 分散读       writev 集中写
                                     │                │
                                     ▼                ▼
                              onProcess()       toWriteBytes()==0
                              HttpConn::process()
                                     │
                          ┌──────────┴──────────┐
                          ▼                      ▼
                   HttpRequest::parse()   HttpResponse::makeResponse()
                   状态机解析请求行/         mmap 文件 + 构造 HTML
                   请求头/请求体
                          │                      │
                          └──────────┬───────────┘
                                     ▼
                             iov[0] = 响应头 Buffer
                             iov[1] = mmap 文件映射
                                     │
                                     ▼
                            epoll MOD → EPOLLOUT
                                     │
                            (下一轮 epoll_wait)
                                     │
                                     ▼
                              onWrite → writev
```

## 四、核心模块设计

### 4.1 主控制器 — WebServer

```
WebServer 是整个系统的协调者，持有所有子模块的 unique_ptr 所有权：

  WebServer
  ├── std::unique_ptr<HeapTimer>    timer_      — 定时器（连接超时踢除）
  ├── std::unique_ptr<ThreadPool>   threadpool_ — 线程池（处理读写业务）
  ├── std::unique_ptr<Epoller>      epoller_    — epoll 事件多路复用
  ├── std::unordered_map<int,HttpConn> users_  — fd → 连接对象映射
  ├── int                           listenFd_   — 监听 socket
  └── uint32_t listenEvent_ / connEvent_  — epoll 事件配置

关键设计决策：
  - connEvent_ 默认包含 EPOLLONESHOT —— 每个 fd 同一时刻只被一个线程触发，
    处理完后需要 modFd 重新注册，避免惊群。
  - EPOLLRDHUP 用于检测客户端半关闭（shutdown(SHUT_WR)）。
  - 定时器绑定 closeConn → timer 超时即关闭连接。
```

### 4.2 事件循环 — start()

```
while(!isClose_)
    timeout = timer_->getNextTick()   // 最近超时时间
    events  = epoller_->wait(timeout) // 阻塞等待，超时则 tick()

    for each event:
        if fd == listenFd  → dealListen()        // accept 循环
        if EPOLLRDHUP|HUP|ERR → closeConn()      // 对端关闭
        if EPOLLIN         → dealRead()  → 提交线程池
        if EPOLLOUT        → dealWrite() → 提交线程池

关键：
  - ET 模式下 accept 必须循环到 EAGAIN
  - dealRead/dealWrite 先 extentTime(client) 更新超时时间，再提交线程池
```

### 4.3 HTTP 连接 — HttpConn

```
HttpConn 封装了一个 TCP 连接的全生命周期：

  状态机：
    init()    → fd, addr, userCount++
    read()    → Buffer::readFd (readv 分散读)
    process() → HttpRequest::parse (状态机解析)
              → HttpResponse::makeResponse (mmap + 构造头部)
              → 设置 iov[0] (头部) + iov[1] (mmap文件)
    write()   → writev (集中写) + 更新 iov 偏移
    close()   → 解除 mmap, ::close(fd), userCount--

  iovec 双缓冲：
    iov_[0] = {writeBuff_.peek(),   writeBuff_.readableBytes()}   // 响应头
    iov_[1] = {mmFile_,             mmFileStat_.st_size}          // 文件内容
    writev(fd, iov_, iovCnt_) 一次系统调用发送两段数据
```

### 4.4 HTTP 解析 — HttpRequest

```
解析状态机（PARSE_STATE）：

  REQUEST_LINE ──parseRequestLine()──► HEADERS
                                          │
                                   parseHeader() 逐行解析
                                   遇到空行 → FINISH
                                   遇到非header行 → BODY
                                          │
                                          ▼
                                        BODY ──parseBody()──► FINISH
                                          │
                                          ▼
                                    parsePost() → parseFromUrlencoded()
                                    处理 application/x-www-form-urlencoded

每一行通过 std::search 查找 CRLF 分割符，然后从 Buffer 中 retrieve 掉。
```

### 4.5 HTTP 响应 — HttpResponse

```
makeResponse() 流程：
  1. stat() 检查文件是否存在  → 404
  2. 检查文件可读权限         → 403
  3. mmap() 将文件映射到内存   → 零拷贝传输
  4. 构造状态行 + 头部 + Content-length

mmap 零拷贝：
  - open() → mmap(MAP_PRIVATE) → close(fd)  文件内容由内核页缓存直接发送
  - 响应完成后 munmap() 解除映射
  - 优点：减少一次内核空间→用户空间的数据拷贝
```

### 4.6 缓冲区 — Buffer

```
Buffer 设计：
  ┌─────────── readPos_ ─────────── writePos_ ────────── capacity ─┐
  │   prependable   │     readable     │       writable            │
  └─────────────────┴──────────────────┴───────────────────────────┘

  动态扩容策略 (makeSpace_)：
    if writable + prependable < len:
        resize(writePos_ + len + 1)     // 确实不够，扩容
    else:
        将 readable 段前移至 buffer_[0]  // 整理碎片

  分散读 (readFd)：
    iov[0] = {buffer_ + writePos_, writable}     // 优先写入 Buffer
    iov[1] = {stack_buf, 16384}                  // 溢出写入栈缓冲
    readv(fd, iov, 2)
    ET 模式下循环 readv 直到 EAGAIN

  原子读写指针：
    使用 std::atomic<size_t> 的 readPos_/writePos_，
    因为 HTTP 连接可能被不同线程读写（线程池调度）。
```

### 4.7 定时器 — HeapTimer

```
小根堆实现（二叉堆 + 哈希索引）：

  数据结构：
    heap_  = vector<TimerNode{id, expires, callback}>
    ref_   = unordered_map<id → heap_index>

  操作复杂度：
    add(id, timeout, cb)    — O(log n)  插入 + siftup
    adjust(id, newTimeout)  — O(log n)  siftdown 或 siftup
    tick()                  — O(k log n) 弹出所有超时节点并执行回调
    doWork(id)              — O(log n)  删除节点 + 执行回调
    getNextTick()           — O(k log n) tick() + 返回最近的超时时间(ms)

  特殊处理：
    - 先移除节点再执行回调：防止回调中修改堆导致迭代器失效
    - getNextTick 每次调用都会先 tick() 清理超时节点
```

### 4.8 线程池 — ThreadPool

```
ThreadPool 使用了 shared_ptr<Pool> 手法：

  设计：
    - 构造函数启动 N 个 std::thread，每个线程进入 wait-loop
    - AddTask() 向 std::queue 投递任务，notify_one() 唤醒一个工作线程
    - 析构时设置 isClosed=true，notify_all()，join 所有线程

  为什么用 shared_ptr<Pool>？
    如果 ThreadPool 被 move，工作线程的 lambda 捕获 [pool = pool_] 仍持有
    到正确的 Pool 对象，不会出现悬空指针。

  wait-loop 模式：
    while(true) {
        if(tasks not empty) → lock → pop → unlock → execute → relock
        else if(isClosed)   → break
        else                → cond.wait()
    }
    这种「执行时 unlock」的模式允许多个线程并发执行任务。
```

### 4.9 数据库连接池 — SqlConnPool + SqlConnRAII

```
SqlConnPool（单例）：
  - init()   → 预创建 connSize 个 MySQL 连接，推入队列
  - getConn() → sem_wait + mutex → 从队列取出
  - freeConn() → mutex + push + sem_post
  - 使用 POSIX sem_t 做计数信号量（限流），std::mutex 保护队列

SqlConnRAII：
  - 构造时调用 getConn()，析构时调用 freeConn()
  - 确保无论正常返回还是异常都能归还连接
```

### 4.10 日志系统 — Log + BlockDeque

```
Log（单例，异步写入）：
  初始化：
    - isAsync=true → 创建 BlockDeque + 启动 FlushLogThread 线程

  写入流程 (write)：
    1. 获取时间戳，写入前缀 "[YYYY-MM-DD HH:MM:SS.μs][level]:"
    2. vsnprintf 格式化用户消息
    3. isAsync → push_back 到阻塞队列（消费者异步刷盘）
       !isAsync → fwrite 直接同步写入文件

  日志切分：
    - 跨天：创建 YYYY_MM_DD.log
    - 超行：创建 YYYY_MM_DD-N.log（每 50000 行切分）

BlockDeque（生产者-消费者）：
  - 使用条件变量 + 双通知（condConsumer_ / condProducer_）
  - push 时队列满则生产者 wait，pop 时队列空则消费者 wait
  - flush() 唤醒消费者立即处理积压日志
```

## 五、并发模型

```
              ┌─────────────────┐
              │   Main Thread   │
              │  (I/O Reactor)  │
              │                 │
              │  epoll_wait()   │──── 监听事件就绪
              │  accept()       │──── 接受新连接
              │  timer tick()   │──── 清理超时连接
              └───────┬─────────┘
                      │ 投递任务 (dealRead / dealWrite)
         ┌────────────┼────────────┐
         ▼            ▼            ▼
    ┌─────────┐ ┌─────────┐ ┌─────────┐
    │ Worker0 │ │ Worker1 │ │ Worker2 │  ... (N 个工作线程)
    │         │ │         │ │         │
    │ onRead  │ │ onWrite │ │ onRead  │
    │  ↓      │ │  ↓      │ │  ↓      │
    │ process │ │ send    │ │ process │
    └─────────┘ └─────────┘ └─────────┘

同步点：
  1. 主线程访问 users_[fd] 不加锁（ET+ONESHOT 保证同一 fd 不会同时被两个线程处理）
  2. 线程池内部 mutex 保护任务队列
  3. 日志系统 mutex 保护 buffer 写入
  4. 定时器未加锁（只在主线程调用 —— 它是 Reactor 的一部分）

EPOLLONESHOT 的作用：
  - 每个 fd 被触发一次后自动从 epoll 移除
  - 处理完毕后需要 epoller_->modFd() 重新注册
  - 保证同一 fd 不会被多个工作线程同时处理
```

## 六、关键设计模式

| 模式 | 位置 | 说明 |
|------|------|------|
| **Reactor** | WebServer::start() | 主线程 I/O 多路复用 + 事件分发 |
| **HSHA** | WebServer + ThreadPool | I/O 在主线程，业务在线程池（半同步/半反应堆） |
| **RAII** | SqlConnRAII, HttpConn | 资源获取即初始化，保证异常安全 |
| **单例 (Meyers)** | Log::Instance(), SqlConnPool::instance() | C++11 magic static，线程安全 |
| **对象池** | SqlConnPool, ThreadPool | 资源预分配，减少运行时创建开销 |
| **生产者-消费者** | BlockDeque, ThreadPool | 异步解耦，流量削峰 |
| **零拷贝** | HttpResponse::mmap | mmap + writev 减少数据拷贝 |
| **状态机** | HttpRequest::PARSE_STATE | 逐行解析 HTTP 协议，避免大缓冲区 |

## 七、ET vs LT 模式选择

```
trigMode | listen  | conn    | 适用场景
---------|---------|---------|---------------------
   0     |   LT    |   LT    | 简单场景，吞吐量较低
   1     |   LT    |   ET    | 大量短连接（accept 快，read 精准）
   2     |   ET    |   LT    | 大量并发连接（减少 accept 唤醒）
   3     |   ET    |   ET    | 高吞吐场景（需循环读写 + EAGAIN）

LT (Level Triggered):
  - 只要缓冲区有数据，epoll_wait 就持续返回
  - 优点：编程简单，不容易丢事件
  - 缺点：可能频繁唤醒

ET (Edge Triggered):
  - 只在缓冲区状态变化时触发一次
  - 优点：减少 epoll_wait 次数，支持更高并发
  - 缺点：必须循环读写直到 EAGAIN，编程复杂

本项目 ET 模式下的处理：
  - accept() 必须循环到返回 EAGAIN（listenEvent_ & EPOLLET）
  - read() 必须循环 readv 直到 EAGAIN（isET）
  - write() 在 toWriteBytes() > 10240 或 isET 时循环 writev
```

## 八、文件与内存管理

```
静态文件服务流程：
  请求 /index.html
    → path_  = "/index.html"
    → srcDir_ = getcwd() + "/resources/"
    → 完整路径 = srcDir_ + path_

  mmap 流程：
    1. open(srcDir + path, O_RDONLY)
    2. stat() 获取文件大小
    3. mmap(NULL, size, PROT_READ, MAP_PRIVATE, srcFd, 0)
    4. close(srcFd)   ← mmap 后立即关闭文件 fd，映射仍有效
    5. 通过 writev 发送：iov[0]=头部, iov[1]=mmap 文件
    6. munmap(mmFile_, size)

  内存安全：
    - 响应完成后 unmapFile()，或者连接关闭时 close() → unmapFile()
    - mmap 错误时回退到 errorContent() 构造 HTML 错误页面
```

## 九、潜在改进方向

| 方向 | 说明 |
|------|------|
| **多 Reactor** | 改为 One Loop Per Thread + 多 Reactor，主线程只 accept，子线程各自 epoll |
| **无锁队列** | ThreadPool 用 lock-free MPMC queue 替代 mutex + condition_variable |
| **内存池** | Buffer 用 slab allocator 减少频繁的 vector 扩容 |
| **HTTP/2** | 支持多路复用、头部压缩 |
| **TLS** | 集成 OpenSSL / mbedtls 支持 HTTPS |
| **配置文件** | 替换硬编码参数为 JSON/YAML 配置解析 |
| **benchmark** | 添加 wrk/ab 性能基准测试 |

---

> 最后更新：2026-06-19
> 基于 TinyWebServer 源码分析生成
