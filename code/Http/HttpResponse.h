/**
 * @file    HttpResponse.h
 * @brief   HTTP 响应构造器 —— mmap 零拷贝文件传输
 * @details
 *
 *   HttpResponse 负责根据请求路径生成 HTTP 响应:
 *    1. 检查文件是否存在 / 可读
 *    2. 用 mmap() 将文件映射到进程地址空间
 *    3. 拼接 HTTP 响应头和 Content-length
 *    4. iov[1] 指向 mmap 映射，由 writev 零拷贝发送
 *
 *   错误页面 (4xx) 通过 errorHtml_() 重定向到对应 HTML 文件。
 */

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "../Buffer/Buffer.h"
#include "../Log/Log.h"

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    /**
     * @brief 初始化响应对象
     * @param srcDir      静态文件根目录
     * @param path        请求路径（可被 errorHtml_ 覆盖）
     * @param isKeepAlive 是否保持连接
     * @param code        HTTP 状态码（-1 表示自动判定）
     */
    void init(const std::string& srcDir, std::string& path, bool isKeepAlive = false, int code = -1);

    /**
     * @brief 构造 HTTP 响应到 Buffer
     *
     *  流程: check file → errorHtml_ → addStateLine_ → addHeader_ → addContent_ (mmap)
     */
    void makeResponse(Buffer& buff);

    /** @brief 解除 mmap 文件映射 */
    void unmapFile();

    /** @brief 返回 mmap 映射的文件地址 */
    char* file();

    /** @brief 返回 mmap 文件大小 */
    size_t fileLen() const;

    /** @brief 构造 HTML 错误页面 */
    void errorContent(Buffer& buff, std::string message);

    int code() const { return code_; }

private:
    void addStateLine_(Buffer& buff);   ///< "HTTP/1.1 200 OK\r\n"
    void addHeader_(Buffer& buff);      ///< Connection + Content-Type
    void addContent_(Buffer& buff);     ///< mmap 文件 + Content-length

    void errorHtml_();                  ///< 错误码 → 错误页面路径
    std::string getFileType_();         ///< 根据文件后缀返回 MIME 类型

    int code_;                          ///< HTTP 状态码
    bool isKeepAlive_;                  ///< 是否保持连接

    std::string path_;                  ///< 请求路径
    std::string srcDir_;                ///< 静态文件根目录

    char* mmFile_;                      ///< mmap 映射的文件内存地址
    struct stat mmFileStat_;            ///< mmap 文件的 stat 信息

    /* ===== 静态映射表 ===== */
    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;  ///< 后缀 → MIME
    static const std::unordered_map<int, std::string> CODE_STATUS;          ///< 状态码 → 描述
    static const std::unordered_map<int, std::string> CODE_PATH;            ///< 状态码 → 错误页面路径
};

#endif // HTTP_RESPONSE_H
