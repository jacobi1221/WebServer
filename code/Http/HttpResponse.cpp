/**
 * @file    HttpResponse.cpp
 * @brief   HttpResponse 实现 —— mmap 零拷贝 + HTTP 响应构造
 */

#include "HttpResponse.h"

using namespace std;

/* ========================================================================
 *  静态映射表
 * ======================================================================== */

// 文件后缀 → MIME 类型
const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
    { ".html",  "text/html" },
    { ".xml",   "text/xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".txt",   "text/plain" },
    { ".rtf",   "application/rtf" },
    { ".pdf",   "application/pdf" },
    { ".word",  "application/nsword" },
    { ".png",   "image/png" },
    { ".gif",   "image/gif" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".au",    "audio/basic" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".avi",   "video/x-msvideo" },
    { ".gz",    "application/x-gzip" },
    { ".tar",   "application/x-tar" },
    { ".css",   "text/css" },
    { ".js",    "text/javascript" },
};

// HTTP 状态码 → 状态描述
const unordered_map<int, string> HttpResponse::CODE_STATUS = {
    { 200, "OK" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
};

// HTTP 状态码 → 错误页面路径
const unordered_map<int, string> HttpResponse::CODE_PATH = {
    { 400, "/400.html" },
    { 403, "/403.html" },
    { 404, "/404.html" },
};

/* ========================================================================
 *  构造 / 析构
 * ======================================================================== */

HttpResponse::HttpResponse() {
    code_ = -1;
    path_ = srcDir_ = "";
    isKeepAlive_ = false;
    mmFile_ = nullptr;
    mmFileStat_ = { 0 };
}

HttpResponse::~HttpResponse() {
    unmapFile();
}

/* ========================================================================
 *  初始化与构造响应
 * ======================================================================== */

void HttpResponse::init(const string& srcDir, string& path, bool isKeepAlive, int code)
{
    assert(srcDir != "");
    if(mmFile_) { unmapFile(); }     // 释放上一次的 mmap

    code_ = code;
    isKeepAlive_ = isKeepAlive;
    path_ = path;
    srcDir_ = srcDir;
    mmFile_ = nullptr;
    mmFileStat_ = { 0 };
}

/**
 * @brief 构造完整的 HTTP 响应
 *
 *  流程:
 *   1. stat() 检查文件     → 不存在 → 404
 *                        → 无权限 → 403
 *                        → 正常   → 200
 *   2. errorHtml_(): 如果是错误码，替换为错误页面路径
 *   3. addStateLine_(): 写入状态行
 *   4. addHeader_():    写入 Connection + Content-Type
 *   5. addContent_():   写入 Content-length + mmap 文件
 *
 *  @note mmap 创建的映射在 addContent_() 中实现，写入 Buffer 的只有响应头。
 *        文件数据由 iov[1] 指向 mmFile_，通过 writev 直接发送。
 */
void HttpResponse::makeResponse(Buffer& buff)
{
    // 1. 检查文件状态
    if(stat((srcDir_ + path_).data(), &mmFileStat_) < 0 || S_ISDIR(mmFileStat_.st_mode))
    {
        code_ = 404;         // 文件不存在或路径是目录
    }
    else if(!(mmFileStat_.st_mode & S_IROTH))
    {
        code_ = 403;         // 文件不可读（others 无读权限）
    }
    else if(code_ == -1)
    {
        code_ = 200;         // 无预置错误码 → 200 OK
    }

    // 2. 错误码 → 替换为错误页面
    errorHtml_();

    // 3-5. 构造 HTTP 响应
    addStateLine_(buff);
    addHeader_(buff);
    addContent_(buff);       // mmap 在这里创建
}

/* ========================================================================
 *  mmap 文件访问
 * ======================================================================== */

char* HttpResponse::file()     { return mmFile_; }
size_t HttpResponse::fileLen() const { return mmFileStat_.st_size; }

/**
 * @brief 解除 mmap 映射
 *
 *  close() 时调用，或下一次 init() 覆盖前调用。
 */
void HttpResponse::unmapFile()
{
    if(mmFile_)
    {
        munmap(mmFile_, mmFileStat_.st_size);
        mmFile_ = nullptr;
    }
}

/* ========================================================================
 *  响应构成部分
 * ======================================================================== */

/**
 * @brief 错误码 → 错误页面路径
 *
 *  如 code_=404 → path_="/404.html"，重新 stat 获取错误页面信息。
 */
void HttpResponse::errorHtml_()
{
    auto it = CODE_PATH.find(code_);
    if(it != CODE_PATH.end())
    {
        path_ = it->second;
        stat((srcDir_ + path_).data(), &mmFileStat_);
    }
}

/**
 * @brief 添加响应状态行
 *
 *  格式: "HTTP/1.1 200 OK\r\n"
 */
void HttpResponse::addStateLine_(Buffer& buff)
{
    string status;
    auto it = CODE_STATUS.find(code_);
    if(it != CODE_STATUS.end())
    {
        status = it->second;
    }
    else
    {
        code_ = 400;                            // 未知状态码 → 400
        status = CODE_STATUS.at(400);
    }
    buff.append("HTTP/1.1 " + to_string(code_) + " " + status + "\r\n");
}

/**
 * @brief 添加响应头
 *
 *  Connection: keep-alive / close
 *  Content-Type: text/html / image/png / ...
 */
void HttpResponse::addHeader_(Buffer& buff)
{
    buff.append("Connection: ");
    if(isKeepAlive_)
    {
        buff.append("keep-alive\r\n");
        buff.append("Keep-Alive: max=6, timeout=120\r\n");
    }
    else
    {
        buff.append("close\r\n");
    }
    buff.append("Content-Type: " + getFileType_() + "\r\n");
}

/**
 * @brief 添加响应内容 —— mmap 文件映射 + Content-length 头
 *
 *  mmap 流程:
 *   1. open()  打开文件获取 fd
 *   2. mmap()  将文件内容映射到进程地址空间 (MAP_PRIVATE, 写时拷贝)
 *   3. close() 关闭文件 fd（映射仍然有效，由内核页缓存维护）
 *
 *  文件数据不拷贝到 Buffer，而是由 mmFile_ 指针直接指向，
 *  后续通过 writev 从内核页缓存直接发送（零拷贝）。
 */
void HttpResponse::addContent_(Buffer& buff)
{
    int srcFd = open((srcDir_ + path_).data(), O_RDONLY);
    if(srcFd < 0)
    {
        errorContent(buff, "File NotFound!");
        return;
    }

    LOG_DEBUG("file path %s", (srcDir_ + path_).data());

    // MAP_PRIVATE: 写入时拷贝的私有映射（我们只读，实际不会触发拷贝）
    void* mmRet = mmap(nullptr, mmFileStat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
    if(mmRet == MAP_FAILED)
    {
        errorContent(buff, "File NotFound!");
        close(srcFd);
        return;
    }

    mmFile_ = static_cast<char*>(mmRet);
    close(srcFd);   // mmap 后文件 fd 可立即关闭

    buff.append("Content-length: " + to_string(mmFileStat_.st_size) + "\r\n\r\n");
}

/* ========================================================================
 *  MIME 类型判断
 * ======================================================================== */

string HttpResponse::getFileType_()
{
    string::size_type idx = path_.find_last_of('.');
    if(idx == string::npos)
    {
        return "text/plain";    // 无后缀 → 纯文本
    }

    string suffix = path_.substr(idx);
    auto it = SUFFIX_TYPE.find(suffix);
    return (it != SUFFIX_TYPE.end()) ? it->second : "text/plain";
}

/* ========================================================================
 *  错误页面 HTML 生成（fallback: 当 mmap 失败时）
 * ======================================================================== */

void HttpResponse::errorContent(Buffer& buff, string message)
{
    string body;
    auto it = CODE_STATUS.find(code_);
    string status = (it != CODE_STATUS.end()) ? it->second : "Bad Request";

    body += "<html><title>Error</title>";
    body += "<body bgcolor=\"ffffff\">";
    body += to_string(code_) + " : " + status + "\n";
    body += "<p>" + message + "</p>";
    body += "<hr><em>TinyWebServer</em></body></html>";

    buff.append("Content-Type: text/html\r\n");
    buff.append("Content-length: " + to_string(body.size()) + "\r\n\r\n");
    buff.append(body);
}
