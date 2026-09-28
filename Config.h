#pragma once
// -----------------------------------------------------------------------------
// Config: 进程级配置单例 (header-only)。
// 优先级: 环境变量 > config.json > 内置默认值。
// 敏感信息 (数据库口令 / JWT 密钥 / OSS 密钥) 不再硬编码在源码里。
// -----------------------------------------------------------------------------
#include <string>
#include <fstream>
#include <mutex>
#include <cstdlib>
#include <nlohmann/json.hpp>

class Config
{
public:
    static Config& instance()
    {
        static Config cfg;
        return cfg;
    }

    // 从指定路径加载 config.json (只需在 main 启动时调用一次; 找不到文件则用默认值)
    void load(const std::string& path = "config.json")
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ifstream in(path);
        if (in.good()) {
            try {
                in >> m_json;
            } catch (const std::exception& e) {
                m_json = nlohmann::json::object();
            }
        }
        m_loaded = true;
    }

    std::string mysql_url()    { return get_str("mysql_url",    "MYSQL_URL",    "mysql://root:1234@localhost/test"); }
    std::string rabbitmq_url() { return get_str("rabbitmq_url", "RABBITMQ_URL", "amqp://guest:guest@localhost:5672/%2f"); }
    std::string consul_url()   { return get_str("consul_url",   "CONSUL_URL",   "http://127.0.0.1:8500"); }
    std::string jwt_secret()   { return get_str("jwt_secret",   "JWT_SECRET",   "$Rv&O98@"); }
    // /metrics 抓取令牌: 非空时, 抓取需带 ?token= 或 Bearer 匹配 (为空则开放, 建议内网/反代)
    std::string metrics_token(){ return get_str("metrics_token","METRICS_TOKEN",""); }
    std::string storage_dir()  { return get_str("storage_dir",  "STORAGE_DIR",  "storage"); }
    std::string log_file()     { return get_str("log_file",     "LOG_FILE",     "logs/clouddisk.log"); }
    std::string oss_bucket()   { return get_str("oss_bucket",   "OSS_BUCKET",   ""); }
    int http_port()            { return get_int("http_port",    "HTTP_PORT",    8888); }
    int srpc_port()            { return get_int("srpc_port",    "SRPC_PORT",    1414); }
    // 单文件大小上限 (字节), 默认 100 MB
    long long max_file_size()  { return (long long)get_int("max_file_size", "MAX_FILE_SIZE", 104857600); }
    // 每个用户的存储配额 (字节), 默认 1 GB
    long long user_quota()     { return (long long)get_int("user_quota", "USER_QUOTA", 1073741824); }

private:
    Config() { load(); }

    std::string get_str(const char* key, const char* env, const char* def)
    {
        const char* e = std::getenv(env);
        if (e && e[0]) return std::string(e);
        if (m_json.contains(key) && m_json[key].is_string())
            return m_json[key].get<std::string>();
        return std::string(def);
    }
    long long get_int(const char* key, const char* env, long long def)
    {
        const char* e = std::getenv(env);
        if (e && e[0]) { try { return std::stoll(e); } catch (...) {} }
        if (m_json.contains(key) && m_json[key].is_number())
            return m_json[key].get<long long>();
        return def;
    }

    nlohmann::json m_json = nlohmann::json::object();
    bool m_loaded = false;
    std::mutex m_mutex;
};
