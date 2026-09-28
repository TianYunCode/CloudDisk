#pragma once
// -----------------------------------------------------------------------------
// Log: 极简线程安全日志 (header-only)。写入文件并回显到 stdout, 带时间戳与级别。
//   LOG_INFO("msg " << x); LOG_WARN(...); LOG_ERROR(...);
// -----------------------------------------------------------------------------
#include <string>
#include <fstream>
#include <mutex>
#include <sstream>
#include <iostream>
#include <ctime>
#include <sys/stat.h>

class Log
{
public:
    static Log& instance()
    {
        static Log inst;
        return inst;
    }

    void init(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 确保日志目录存在
        auto pos = path.find_last_of('/');
        if (pos != std::string::npos) {
            std::string dir = path.substr(0, pos);
            ::mkdir(dir.c_str(), 0755);
        }
        m_out.open(path, std::ios::app);
        m_path = path;
    }

    void write(const char* level, const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        char ts[32];
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        std::string line = std::string("[") + ts + "] [" + level + "] " + msg;
        std::cout << line << std::endl;
        if (m_out.is_open()) { m_out << line << "\n"; m_out.flush(); }
    }

private:
    Log() = default;
    std::ofstream m_out;
    std::string m_path;
    std::mutex m_mutex;
};

#define LOG_INFO(x)  do { std::ostringstream _o; _o << x; Log::instance().write("INFO",  _o.str()); } while(0)
#define LOG_WARN(x)  do { std::ostringstream _o; _o << x; Log::instance().write("WARN",  _o.str()); } while(0)
#define LOG_ERROR(x) do { std::ostringstream _o; _o << x; Log::instance().write("ERROR", _o.str()); } while(0)
