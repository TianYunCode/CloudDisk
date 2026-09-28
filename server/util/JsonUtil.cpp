// =============================================================================
// JsonUtil.cpp —— JSON 提取 / id 列表处理的实现 (单一真源)
// =============================================================================
#include "JsonUtil.h"

std::string json_str(const nlohmann::json& j, const char* key)
{
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return std::string();
}

// 从 JSON 读取一组正整数 id (支持数字或数字字符串)。全部合法返回 true; 缺省视为空数组。
bool ids_from_json(const nlohmann::json& j, const char* key, std::vector<long long>& out)
{
    out.clear();
    if (!j.contains(key) || j[key].is_null()) return true;
    if (!j[key].is_array()) return false;
    for (auto& v : j[key]) {
        if (v.is_number_integer() || v.is_number_unsigned()) {
            long long n = v.get<long long>();
            if (n <= 0) return false;
            out.push_back(n);
        } else if (v.is_string()) {
            const std::string s = v.get<std::string>();
            if (s.empty()) return false;
            for (char c : s) if (c < '0' || c > '9') return false;
            out.push_back(std::stoll(s));
        } else return false;
    }
    return true;
}

// 把 id 列表拼成安全的 "1,2,3" (仅数字, 无注入风险)。
std::string ids_csv(const std::vector<long long>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
    return s;
}
