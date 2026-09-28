// =============================================================================
// JsonUtil.h —— 请求 JSON 解析/提取 与 id 列表处理 (纯逻辑, 仅依赖 nlohmann/json)
//
// 从网关支撑层抽出的纯函数: 与 HTTP/DB 无关, 高内聚、可复用、可独立单元测试。
//   · json_str      : 安全读取字符串字段 (缺省/类型不符返回空串)
//   · ids_from_json : 读取一组正整数 id (数字或数字字符串, 全部合法才返回 true)
//   · ids_csv       : 拼成安全的 "1,2,3" (仅数字, 无注入风险)
// =============================================================================
#ifndef CLOUDDISK_JSON_UTIL_H
#define CLOUDDISK_JSON_UTIL_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

std::string json_str(const nlohmann::json& j, const char* key);
bool ids_from_json(const nlohmann::json& j, const char* key, std::vector<long long>& out);
std::string ids_csv(const std::vector<long long>& v);

#endif // CLOUDDISK_JSON_UTIL_H
