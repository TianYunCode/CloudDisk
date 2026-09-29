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

// UTF-8 码点数 (非字节数)。统计非延续字节 (0b10xxxxxx) 即得码点数;
// 对畸形序列也给出稳定计数, 不会崩溃或产生未定义行为。
// 用途: 密码长度按"字符数"而非"字节数"衡量, 与前端 [...p].length 语义一致。
inline size_t utf8_length(const std::string& s)
{
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

// 密码强度: 6-64 个**字符** (Unicode 码点), 可按需加强。
// 注意: 必须按码点而非字节计数 —— 否则 2 个汉字 (6 字节) 就能通过"6 位"下限,
// 而 33 个汉字 (99 字节) 又会被上限误拒, 且与前端判定相反。
// 另设 256 字节的原始长度上限, 防止超长输入造成的无谓开销。
inline bool valid_password(const std::string& p)
{
    if (p.size() > 256) return false;
    const size_t chars = utf8_length(p);
    return chars >= 6 && chars <= 64;
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
