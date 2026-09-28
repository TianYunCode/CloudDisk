#pragma once
// -----------------------------------------------------------------------------
// ApiResp: 统一 JSON 响应封装 + Bearer/Token 鉴权 (header-only)。
// 响应格式: {"code":0,"message":"ok","data":...}  (code==0 表示成功)
// -----------------------------------------------------------------------------
#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <wfrest/HttpServer.h>

#include "CryptoUtil.h"
#include "User.h"

namespace api {

// 可选的 API Token 解析器: 由宿主 (CloudiskServer) 在启动时注入。
// 返回 true 表示 token 命中一个有效的个人访问令牌, 并已填充 user。
inline std::function<bool(const std::string&, User&)>& token_resolver()
{
    static std::function<bool(const std::string&, User&)> resolver;
    return resolver;
}

inline void write_json(wfrest::HttpResp* resp, int http_status, const nlohmann::json& body)
{
    resp->set_status(http_status);
    resp->add_header("Content-Type", "application/json; charset=utf-8");
    resp->String(body.dump());
}

inline void ok(wfrest::HttpResp* resp, const nlohmann::json& data = nlohmann::json::object(),
               const std::string& message = "ok")
{
    nlohmann::json body;
    body["code"] = 0;
    body["message"] = message;
    body["data"] = data;
    write_json(resp, 200, body);
}

inline void fail(wfrest::HttpResp* resp, int http_status, int code, const std::string& message)
{
    nlohmann::json body;
    body["code"] = code;
    body["message"] = message;
    body["data"] = nullptr;
    write_json(resp, http_status, body);
}

// 从请求中取出 token: 优先 Authorization: Bearer <token>, 兼容 ?token= 查询参数。
inline std::string extract_token(const wfrest::HttpReq* req)
{
    if (req->has_header("Authorization")) {
        const std::string& h = req->header("Authorization");
        const std::string prefix = "Bearer ";
        if (h.size() > prefix.size() && h.compare(0, prefix.size(), prefix) == 0)
            return h.substr(prefix.size());
    }
    if (req->has_query("token")) return req->query("token");
    return std::string();
}

// 鉴权: 成功返回 true 并填充 user; 失败直接写 401 响应并返回 false。
// 优先校验 JWT; 未命中则尝试 API Token 解析器 (个人访问令牌)。
inline bool require_auth(const wfrest::HttpReq* req, wfrest::HttpResp* resp, User& user)
{
    std::string token = extract_token(req);
    if (!token.empty()) {
        if (CryptoUtil::verify_token(token, user)) return true;
        auto& tr = token_resolver();
        if (tr && tr(token, user)) return true;
    }
    fail(resp, 401, 401, "未登录或登录已过期");
    return false;
}

} // namespace api
