#pragma once
// -----------------------------------------------------------------------------
// SqlUtil: SQL 注入防护 (header-only)。
// workflow 的 MySQL 任务只接受拼好的 SQL 文本, 因此对所有进入 SQL 的字符串值
// 统一做转义, 对数值/标识做白名单校验。
// -----------------------------------------------------------------------------
#include <string>
#include <cctype>
#include <cstdint>

namespace SqlUtil {

// 转义字符串字面量内容 (不含外层引号)。覆盖 MySQL 需要转义的字符。
inline std::string escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '\0': out += "\\0";  break;
            case '\'': out += "\\'";  break;
            case '\"': out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case 0x1a: out += "\\Z";  break;
            case '\\': out += "\\\\"; break;
            default:   out += c;      break;
        }
    }
    return out;
}

// 转义并加上单引号, 直接用于 SQL 值位置。
inline std::string quote(const std::string& in)
{
    return "'" + escape(in) + "'";
}

// 用户名白名单: 3-32 位, 字母/数字/下划线/中划线。
inline bool valid_username(const std::string& u)
{
    if (u.size() < 3 || u.size() > 32) return false;
    for (unsigned char c : u) {
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

// 密码强度: 6-64 位 (可按需加强)。
inline bool valid_password(const std::string& p)
{
    return p.size() >= 6 && p.size() <= 64;
}

// 十六进制哈希白名单 (sha256 = 64 hex)。
inline bool valid_hash(const std::string& h)
{
    if (h.empty() || h.size() > 128) return false;
    for (unsigned char c : h) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

// base62 令牌白名单 (分享 / 上传会话 id)。仅字母数字, 长度 1..64。
inline bool valid_token(const std::string& t)
{
    if (t.empty() || t.size() > 64) return false;
    for (unsigned char c : t) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}
// 把字符串安全转成非负整数; 失败返回 def。用于 limit/offset 等。
inline long long to_uint(const std::string& s, long long def, long long maxv)
{
    if (s.empty()) return def;
    for (unsigned char c : s) if (!std::isdigit(c)) return def;
    try {
        long long v = std::stoll(s);
        if (v < 0) return def;
        if (v > maxv) return maxv;
        return v;
    } catch (...) { return def; }
}

} // namespace SqlUtil
