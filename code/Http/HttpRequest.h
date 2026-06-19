/**
 * @file    HttpRequest.h
 * @brief   HTTP 请求解析器 —— 状态机驱动
 * @details
 *
 *  解析流程:
 *    REQUEST_LINE → HEADERS → BODY → FINISH
 *
 *  支持的 HTTP 方法: GET, POST
 *  支持的 Content-Type: application/x-www-form-urlencoded
 *
 *  解析通过 std::regex 逐行匹配，从 Buffer 中按 CRLF 分割取出行。
 */

#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../Buffer/Buffer.h"

class HttpRequest
{
public:
    /** @brief HTTP 解析状态机 */
    enum PARSE_STATE {
        REQUEST_LINE,   ///< 正在解析请求行
        HEADERS,        ///< 正在解析请求头
        BODY,           ///< 正在解析请求体
        FINISH,         ///< 解析完成
    };

    /** @brief HTTP 解析结果码 */
    enum HTTP_CODE {
        NO_REQUEST = 0,         ///< 请求不完整，等待更多数据
        GET_REQUEST,            ///< 成功解析 GET 请求
        BAD_REQUEST,            ///< 请求格式错误
        NO_RESOURCE,            ///< 请求的资源不存在
        FORBIDDEN_REQUEST,      ///< 请求的资源无权限访问
        FILE_REQUEST,           ///< 请求的是文件
        INTERNAL_ERROR,         ///< 服务器内部错误
        CLOSED_CONNECTION,      ///< 连接已关闭
    };

    HttpRequest() { init(); }
    ~HttpRequest() = default;

    /** @brief 重置状态机，准备解析下一个请求 */
    void init();

    /**
     * @brief 从 Buffer 中解析 HTTP 请求
     * @param buff 读缓冲区
     * @return true 解析成功（状态到达 FINISH）
     */
    bool parse(Buffer& buff);

    /* ===== Getters ===== */
    std::string path() const;
    std::string& path();
    std::string method() const;
    std::string version() const;

    /** @brief 获取 POST 表单参数 */
    std::string getPost(const std::string& key) const;
    std::string getPost(const char* key) const;

    /** @brief 是否 keep-alive 连接 */
    bool isKeepAlive() const;

private:
    /* ===== 逐行解析回调 ===== */
    bool parseRequestLine(const std::string& line);  ///< 解析 "GET /index.html HTTP/1.1"
    void parseHeader(const std::string& line);       ///< 解析 "Key: value"
    void parseBody(const std::string& line);         ///< 解析 POST body

    void parsePath();            ///< 路径补全（/ → /index.html）
    void parsePost();            ///< 解析 POST 表单
    void parseFromUrlencoded();  ///< 解析 URL-encoded 表单数据

    /* ===== 辅助函数 ===== */
    static int converHex(char ch);  ///< 十六进制字符转数值

    /**
     * @brief 用户登录/注册验证
     *
     *  isLogin=true  → 验证密码
     *  isLogin=false → 注册新用户
     */
    static bool userVerify(const std::string& name, const std::string& pwd, bool isLogin);

    /* ===== 解析状态 ===== */
    PARSE_STATE state_;
    std::string method_;  ///< GET 或 POST
    std::string path_;    ///< 请求路径
    std::string version_; ///< HTTP/1.0 或 HTTP/1.1
    std::string body_;    ///< POST 请求体

    std::unordered_map<std::string, std::string> header_; ///< 请求头 key-value
    std::unordered_map<std::string, std::string> post_;   ///< POST 表单 key-value

    /* ===== 静态常量 ===== */
    static const std::unordered_set<std::string> DEFAULT_HTML;     ///< 默认页面集合
    static const std::unordered_map<std::string, int> DEFAULT_HTML_TAG; ///< 页面标签
};

#endif // HTTPREQUEST_H
