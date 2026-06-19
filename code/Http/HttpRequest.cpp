/**
 * @file    HttpRequest.cpp
 * @brief   HttpRequest 实现 —— 正则匹配 + 状态机解析 + 用户认证
 */

#include "HttpRequest.h"

#include <regex>
#include <mysql/mysql.h>

#include "../Log/Log.h"
#include "../Pool/SqlConnPool.h"
#include "../Pool/SqlConnRAII.h"

/* ========================================================================
 *  静态常量
 * ======================================================================== */

// 默认 HTML 页面集合（不带 .html 后缀的路径会自动补全）
const std::unordered_set<std::string> HttpRequest::DEFAULT_HTML{
    "/index", "/register", "/login",
    "/welcome", "/video", "/picture",
};

// 需要用户认证的页面标签: 0=注册页, 1=登录页
const std::unordered_map<std::string, int> HttpRequest::DEFAULT_HTML_TAG {
    {"/register.html", 0},
    {"/login.html",    1},
};

/* ========================================================================
 *  状态机初始化
 * ======================================================================== */

void HttpRequest::init()
{
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;   // 从请求行开始解析
    header_.clear();
    post_.clear();
}

/* ========================================================================
 *  Connection 判断
 * ======================================================================== */

bool HttpRequest::isKeepAlive() const
{
    auto it = header_.find("Connection");
    if(it != header_.end())
    {
        return it->second == "keep-alive";
    }
    // HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close
    return version_ == "1.1";
}

/* ========================================================================
 *  主解析函数 —— 状态机
 * ======================================================================== */

/**
 * @brief 从 Buffer 逐行解析 HTTP 请求
 *
 *  算法:
 *   1. 在 Buffer 中搜索 CRLF (\r\n) 找到行边界
 *   2. 根据当前状态 state_ 分发到对应解析函数
 *   3. retrieveUntil(lineEnd+2) 消耗已解析的数据
 *   4. 循环直到 state_==FINISH 或 Buffer 数据耗尽
 *
 *  状态转移:
 *    REQUEST_LINE → HEADERS → BODY → FINISH
 *
 *  @param buff 读缓冲区
 *  @return true 解析成功
 */
bool HttpRequest::parse(Buffer& buff)
{
    const char CRLF[] = "\r\n";

    if(buff.readableBytes() <= 0) { return false; }

    // 逐行解析，直到状态到达 FINISH 或缓冲区数据耗尽
    while(buff.readableBytes() && state_ != FINISH)
    {
        // 搜索下一行的 CRLF 分割符
        const char* lineEnd = std::search(buff.peek(), buff.beginWriteConst(),
                                          CRLF, CRLF + 2);
        std::string line(buff.peek(), lineEnd);

        switch(state_)
        {
            case REQUEST_LINE:
                if(!parseRequestLine(line)) { return false; }
                parsePath();              // 补全路径后缀
                break;

            case HEADERS:
                parseHeader(line);

                // 遇到空行 (只剩 CRLF) 且之前没有 BODY → 直接完成
                if(buff.readableBytes() <= 2)
                {
                    state_ = FINISH;
                }
                break;

            case BODY:
                parseBody(line);
                break;

            default:
                break;
        }

        // 如果搜索不到 CRLF（行不完整），退出循环等待更多数据
        if(lineEnd == buff.beginWrite())
        {
            break;
        }

        // 消耗已解析的行（包括 CRLF）
        buff.retrieveUntil(lineEnd + 2);
    }

    // POST 请求且 body 非空 → 解析表单数据
    if(method_ == "POST" && !body_.empty()) {
        parsePost();
    }

    LOG_DEBUG("[%s],[%s],[%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return state_ == FINISH;
}

/* ========================================================================
 *  路径处理
 * ======================================================================== */

/**
 * @brief 路径补全
 *
 *  "/"           → "/index.html"
 *  "/index"      → "/index.html"   (在 DEFAULT_HTML 中)
 *  "/register"   → "/register.html"
 */
void HttpRequest::parsePath()
{
    if(path_ == "/")
    {
        path_ = "/index.html";
    }
    else
    {
        for(auto& item : DEFAULT_HTML)
        {
            if(item == path_)
            {
                path_ += ".html";
                break;
            }
        }
    }
}

/* ========================================================================
 *  请求行解析: regex 匹配 "METHOD PATH HTTP/VERSION"
 * ======================================================================== */

bool HttpRequest::parseRequestLine(const std::string& line)
{
    // static const: 正则只编译一次，避免每次请求重复构造
    static const std::regex pattern("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    std::smatch subMatch;

    if(regex_match(line, subMatch, pattern))
    {
        method_ = subMatch[1];     // GET 或 POST
        path_ = subMatch[2];       // /index.html
        version_ = subMatch[3];    // 1.0 或 1.1
        state_ = HEADERS;          // → 下一状态
        return true;
    }

    LOG_ERROR("RequestLine Error");
    return false;
}

/* ========================================================================
 *  请求头解析: regex 匹配 "Key: value"
 * ======================================================================== */

void HttpRequest::parseHeader(const std::string& line)
{
    static const std::regex pattern("^([^:]*): ?(.*)$");
    std::smatch subMatch;

    if(regex_match(line, subMatch, pattern))
    {
        header_[subMatch[1]] = subMatch[2];
    }
    else
    {
        // 不是头部行 → 进入 BODY 状态
        state_ = BODY;
    }
}

/* ========================================================================
 *  请求体解析: URL-encoded 表单
 * ======================================================================== */

void HttpRequest::parseBody(const std::string& line)
{
    // 多行 body 用 & 连接
    if(!body_.empty()) { body_ += "&"; }
    body_ += line;
}

void HttpRequest::parsePost()
{
    auto ctIt = header_.find("Content-Type");
    if(method_ == "POST"
       && ctIt != header_.end()
       && ctIt->second == "application/x-www-form-urlencoded")
    {
        parseFromUrlencoded();

        // 检查是否需要用户认证
        auto tagIt = DEFAULT_HTML_TAG.find(path_);
        if(tagIt != DEFAULT_HTML_TAG.end())
        {
            int tag = tagIt->second;
            LOG_DEBUG("Tag:%d", tag);

            if(tag == 0 || tag == 1)
            {
                bool isLogin = (tag == 1);  // tag=0→注册, tag=1→登录
                if(userVerify(post_["username"], post_["password"], isLogin))
                {
                    path_ = "/welcome.html";
                }
                else
                {
                    path_ = "/error.html";
                }
            }
        }
    }
}

/* ========================================================================
 *  URL 解码 + 键值对解析
 * ======================================================================== */

int HttpRequest::converHex(char ch) {
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

/**
 * @brief 解析 URL-encoded 表单数据
 *
 *  两步处理（in-place）:
 *  1. URL 解码: %XX → 字符, + → 空格
 *  2. 键值对分割: name=value&name=value
 */
void HttpRequest::parseFromUrlencoded()
{
    if(body_.size() == 0) { return; }

    /* ---- Step 1: URL-decode in-place ---- */
    size_t writePos = 0;
    for(size_t i = 0; i < body_.size(); i++)
    {
        if(body_[i] == '%' && i + 2 < body_.size())
        {
            int high = converHex(body_[i + 1]);
            int low  = converHex(body_[i + 2]);
            if(high >= 0 && low >= 0)
            {
                body_[writePos++] = static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        else if(body_[i] == '+')
        {
            body_[writePos++] = ' ';    // + → 空格
            continue;
        }
        body_[writePos++] = body_[i];
    }
    body_.resize(writePos);

    /* ---- Step 2: 解析 key=value&key=value 对 ---- */
    std::string key, value;
    size_t j = 0;
    for(size_t i = 0; i < body_.size(); i++)
    {
        char ch = body_[i];
        if(ch == '=')
        {
            key = body_.substr(j, i - j);
            j = i + 1;
        }
        else if(ch == '&')
        {
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
        }
    }
    // 最后一个 key-value（尾部没有 &）
    if(j < body_.size())
    {
        value = body_.substr(j);
        post_[key] = value;
    }
}

/* ========================================================================
 *  用户认证 (MySQL)
 * ======================================================================== */

/**
 * @brief 用户登录 / 注册验证
 *
 *  使用 mysql_real_escape_string 防止 SQL 注入。
 *
 *  @param name    用户名
 *  @param pwd     密码
 *  @param isLogin true=登录, false=注册
 *  @return true 认证成功
 */
bool HttpRequest::userVerify(const std::string& name, const std::string& pwd, bool isLogin)
{
    if(name == "" || pwd == "") { return false; }
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());

    // RAII 获取数据库连接
    MYSQL* sql = nullptr;
    SqlConnRAII sqlRAII(&sql, SqlConnPool::instance());
    assert(sql);

    /* ---- SQL 注入防护: 转义用户输入 ---- */
    char escapedName[128] = {0};
    char escapedPwd[128]  = {0};
    mysql_real_escape_string(sql, escapedName, name.c_str(), name.length());
    mysql_real_escape_string(sql, escapedPwd,  pwd.c_str(),  pwd.length());

    bool flag = false;
    char order[512] = {0};
    MYSQL_RES* res = nullptr;

    // 注册时默认允许，除非发现用户名已被占用
    if(!isLogin) { flag = true; }

    /* ---- Step 1: 查询用户名 ---- */
    snprintf(order, sizeof(order),
             "SELECT username, password FROM user WHERE username='%s' LIMIT 1",
             escapedName);
    LOG_DEBUG("%s", order);

    if(mysql_query(sql, order))
    {
        mysql_free_result(res);
        return false;
    }

    res = mysql_store_result(sql);
    mysql_num_fields(res);       // 触发字段元数据加载
    mysql_fetch_fields(res);

    while(MYSQL_ROW row = mysql_fetch_row(res))
    {
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        std::string password(row[1]);

        if(isLogin)
        {
            // 登录: 验证密码
            if(pwd == password) { flag = true; }
            else
            {
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        }
        else
        {
            // 注册: 用户名已被占用
            flag = false;
            LOG_DEBUG("user used!");
        }
    }
    mysql_free_result(res);

    /* ---- Step 2: 注册时插入新用户 ---- */
    if(!isLogin && flag == true)
    {
        LOG_DEBUG("register!");
        memset(order, 0, sizeof(order));
        snprintf(order, sizeof(order),
                 "INSERT INTO user(username, password) VALUES('%s','%s')",
                 escapedName, escapedPwd);
        LOG_DEBUG("%s", order);

        if(mysql_query(sql, order))
        {
            LOG_DEBUG("Insert error!");
            flag = false;
        }
    }

    LOG_DEBUG("UserVerify success!!");
    return flag;
}

/* ========================================================================
 *  Getters
 * ======================================================================== */

std::string HttpRequest::path() const     { return path_; }
std::string& HttpRequest::path()          { return path_; }
std::string HttpRequest::method() const   { return method_; }
std::string HttpRequest::version() const  { return version_; }

std::string HttpRequest::getPost(const std::string& key) const {
    assert(key != "");
    auto it = post_.find(key);
    return (it != post_.end()) ? it->second : "";
}

std::string HttpRequest::getPost(const char* key) const {
    assert(key != nullptr);
    auto it = post_.find(key);
    return (it != post_.end()) ? it->second : "";
}
