// =============================================================================
// Admin 处理器: 管理后台 / Prometheus 指标
// (由 CloudiskServer.cpp 按领域拆分而来; 共享支撑见 GatewaySupport.h)
// =============================================================================
#include <workflow/MySQLResult.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/MySQLUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/Workflow.h>
#include <workflow/HttpUtil.h>
#include <wfrest/PathUtil.h>
#include <wfrest/CodeUtil.h>
#include <wfrest/base64.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <array>
#include <cctype>
#include <memory>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <mutex>
#include <unordered_map>
#include <ctime>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <SimpleAmqpClient/SimpleAmqpClient.h>

#include "CloudiskServer.h"
#include "CryptoUtil.h"
#include "UserService.srpc.h"
#include "UserService.pb.h"
#include "Config.h"
#include "Log.h"
#include "ApiResp.h"
#include "SqlUtil.h"
#include "Thumbnailer.h"

using namespace std;
using namespace wfrest;
using namespace protocol;
using namespace std::placeholders;
using namespace AmqpClient;

// 共享支撑层: 运行期配置 + 通用工具 (定义见 GatewaySupport.cpp)
#include "GatewaySupport.h"

// =============================================================================
// 管理后台: 统计 / 用户管理 / 配额 / 角色 / 禁用 / 审计日志
// =============================================================================
void CloudiskServer::register_admin_module()
{
    // 概览统计
    m_server.GET("/api/admin/stats", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        guard_admin(series, user, resp, [resp](SeriesWork* s) {
            string sql =
                "SELECT (SELECT COUNT(*) FROM tbl_user WHERE tomb=0), "
                "(SELECT COUNT(*) FROM tbl_file WHERE deleted=0), "
                "(SELECT CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM tbl_file WHERE deleted=0), "
                "(SELECT COUNT(DISTINCT hashcode) FROM tbl_file WHERE deleted=0), "
                "(SELECT COUNT(*) FROM tbl_share), "
                "(SELECT COUNT(*) FROM tbl_offline), "
                "(SELECT COUNT(*) FROM tbl_user WHERE role=1 AND tomb=0)";
            push_mysql(s, sql, [resp](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "统计失败"); return; }
                MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                if (!c.fetch_row(row)) { api::fail(resp, 500, 500, "无数据"); return; }
                api::ok(resp, {
                    {"users",      cell_ll(row[0])},
                    {"files",      cell_ll(row[1])},
                    {"storage",    (long long)row[2].as_ulonglong()},
                    {"blobs",      cell_ll(row[3])},
                    {"shares",     cell_ll(row[4])},
                    {"offline",    cell_ll(row[5])},
                    {"admins",     cell_ll(row[6])},
                });
            });
        });
    });

    // 用户列表 (含用量)
    m_server.GET("/api/admin/users", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long limit = SqlUtil::to_uint(req->has_query("limit") ? req->query("limit") : "", 50, 200);
        long long offset = SqlUtil::to_uint(req->has_query("offset") ? req->query("offset") : "", 0, 100000000LL);
        if (limit <= 0) limit = 50;
        string g = std::to_string(g_user_quota);
        guard_admin(series, user, resp, [resp, limit, offset, g](SeriesWork* s) {
            string sql =
                "SELECT u.id, u.username, u.nickname, u.role, u.disabled, u.totp_enabled, IF(u.quota>0,u.quota," + g + "), "
                "DATE_FORMAT(u.created_at,'%Y-%m-%d %H:%i:%s'), "
                "(SELECT COUNT(*) FROM tbl_file WHERE deleted=0 AND uid=u.id), "
                "(SELECT CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM tbl_file WHERE deleted=0 AND uid=u.id) "
                "FROM tbl_user u WHERE u.tomb=0 ORDER BY u.id LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
            push_mysql(s, sql, [resp](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
                MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                nlohmann::json arr = nlohmann::json::array();
                while (c.fetch_row(row)) {
                    arr.push_back({
                        {"id", cell_ll(row[0])}, {"username", row[1].as_string()}, {"nickname", row[2].as_string()},
                        {"role", cell_ll(row[3])}, {"disabled", cell_ll(row[4]) != 0}, {"twoFactor", cell_ll(row[5]) != 0},
                        {"quota", cell_ll(row[6])}, {"createdAt", row[7].is_string() ? row[7].as_string() : string()},
                        {"fileCount", cell_ll(row[8])}, {"storageUsed", (long long)row[9].as_ulonglong()},
                    });
                }
                api::ok(resp, {{"users", arr}}, "ok");
            });
        });
    });

    // 设置用户配额 (字节; 0=全局默认)
    m_server.POST("/api/admin/user/quota", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long tuid = in.contains("uid") && in["uid"].is_number() ? in["uid"].get<long long>() : 0;
        long long quota = in.contains("quota") && in["quota"].is_number() ? in["quota"].get<long long>() : -1;
        if (tuid <= 0 || quota < 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        string uname = user.username, ip = client_ip(req); int me = user.id;
        guard_admin(series, user, resp, [resp, tuid, quota, uname, ip, me](SeriesWork* s) {
            string sql = "UPDATE tbl_user SET quota=" + std::to_string(quota) + " WHERE id=" + std::to_string(tuid) + " AND tomb=0";
            push_mysql(s, sql, [resp, tuid, quota, uname, ip, me](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "更新失败"); return; }
                audit_log(me, uname, "admin_set_quota", "uid=" + std::to_string(tuid) + " quota=" + std::to_string(quota), ip);
                api::ok(resp, {{"uid", tuid}, {"quota", quota}}, "配额已更新");
            });
        });
    });

    // 设置用户角色 (0/1)
    m_server.POST("/api/admin/user/role", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long tuid = in.contains("uid") && in["uid"].is_number() ? in["uid"].get<long long>() : 0;
        int role = in.contains("role") && in["role"].is_number() ? in["role"].get<int>() : -1;
        if (tuid <= 0 || (role != 0 && role != 1)) { api::fail(resp, 400, 400, "参数不合法"); return; }
        string uname = user.username, ip = client_ip(req); int me = user.id;
        guard_admin(series, user, resp, [resp, tuid, role, uname, ip, me](SeriesWork* s) {
            // 降级时若这是最后一名管理员则拒绝
            if (role == 0) {
                string chk = "SELECT (SELECT COUNT(*) FROM tbl_user WHERE role=1 AND tomb=0), (SELECT role FROM tbl_user WHERE id=" + std::to_string(tuid) + ")";
                push_mysql(s, chk, [resp, tuid, role, uname, ip, me](WFMySQLTask* tc) {
                    if (!mysql_ok(tc)) { api::fail(resp, 500, 500, "校验失败"); return; }
                    MySQLResultCursor c{ tc->get_resp() }; std::vector<MySQLCell> r;
                    if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "用户不存在"); return; }
                    long long admins = cell_ll(r[0]); long long cur = cell_ll(r[1]);
                    if (admins <= 1 && cur == 1) { api::fail(resp, 400, 400, "不能降级最后一名管理员"); return; }
                    string sql = "UPDATE tbl_user SET role=0 WHERE id=" + std::to_string(tuid) + " AND tomb=0";
                    push_mysql(series_of(tc), sql, [resp, tuid, uname, ip, me](WFMySQLTask* t) {
                        if (!mysql_ok(t)) { api::fail(resp, 500, 500, "更新失败"); return; }
                        audit_log(me, uname, "admin_set_role", "uid=" + std::to_string(tuid) + " role=0", ip);
                        api::ok(resp, {{"uid", tuid}, {"role", 0}}, "角色已更新");
                    });
                });
                return;
            }
            string sql = "UPDATE tbl_user SET role=1 WHERE id=" + std::to_string(tuid) + " AND tomb=0";
            push_mysql(s, sql, [resp, tuid, uname, ip, me](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "更新失败"); return; }
                audit_log(me, uname, "admin_set_role", "uid=" + std::to_string(tuid) + " role=1", ip);
                api::ok(resp, {{"uid", tuid}, {"role", 1}}, "角色已更新");
            });
        });
    });

    // 启用 / 禁用用户
    m_server.POST("/api/admin/user/disable", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long tuid = in.contains("uid") && in["uid"].is_number() ? in["uid"].get<long long>() : 0;
        int disabled = in.contains("disabled") && in["disabled"].is_boolean() ? (in["disabled"].get<bool>() ? 1 : 0)
                     : (in.contains("disabled") && in["disabled"].is_number() ? in["disabled"].get<int>() : -1);
        if (tuid <= 0 || (disabled != 0 && disabled != 1)) { api::fail(resp, 400, 400, "参数不合法"); return; }
        if (tuid == user.id) { api::fail(resp, 400, 400, "不能禁用自己"); return; }
        string uname = user.username, ip = client_ip(req); int me = user.id;
        guard_admin(series, user, resp, [resp, tuid, disabled, uname, ip, me](SeriesWork* s) {
            string sql = "UPDATE tbl_user SET disabled=" + std::to_string(disabled) + " WHERE id=" + std::to_string(tuid) + " AND tomb=0";
            push_mysql(s, sql, [resp, tuid, disabled, uname, ip, me](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "更新失败"); return; }
                audit_log(me, uname, "admin_set_disabled", "uid=" + std::to_string(tuid) + " disabled=" + std::to_string(disabled), ip);
                api::ok(resp, {{"uid", tuid}, {"disabled", disabled != 0}}, "已更新");
            });
        });
    });

    // 审计日志
    m_server.GET("/api/admin/audit", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long limit = SqlUtil::to_uint(req->has_query("limit") ? req->query("limit") : "", 100, 500);
        long long offset = SqlUtil::to_uint(req->has_query("offset") ? req->query("offset") : "", 0, 100000000LL);
        if (limit <= 0) limit = 100;
        string action = req->has_query("action") ? req->query("action") : "";
        string where = "1=1";
        if (!action.empty() && SqlUtil::valid_username(action + "x")) where += " AND action=" + SqlUtil::quote(action);
        guard_admin(series, user, resp, [resp, where, limit, offset](SeriesWork* s) {
            string sql = "SELECT id, uid, username, action, detail, ip, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') "
                         "FROM tbl_audit WHERE " + where + " ORDER BY id DESC LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
            push_mysql(s, sql, [resp](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
                MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                nlohmann::json arr = nlohmann::json::array();
                while (c.fetch_row(row)) {
                    arr.push_back({
                        {"id", cell_ll(row[0])}, {"uid", cell_ll(row[1])}, {"username", row[2].as_string()},
                        {"action", row[3].as_string()}, {"detail", row[4].as_string()}, {"ip", row[5].as_string()},
                        {"createdAt", row[6].is_string() ? row[6].as_string() : string()},
                    });
                }
                api::ok(resp, {{"logs", arr}}, "ok");
            });
        });
    });
}

// =============================================================================
// Prometheus 指标端点 (/metrics, 文本曝露格式)
// =============================================================================
void CloudiskServer::register_metrics_module()
{
    m_server.GET("/metrics", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        // 可选抓取令牌: 配置了 metrics_token 时必须匹配 (?token= 或 Bearer)
        std::string mtok = Config::instance().metrics_token();
        if (!mtok.empty()) {
            std::string t = req->has_query("token") ? req->query("token") : api::extract_token(req);
            if (t != mtok) { resp->set_status(401); resp->add_header("Content-Type", "text/plain"); resp->String("unauthorized\n"); return; }
        }
        // DB 侧 gauge: 用户数 / 文件数 / 存储用量
        string sql = "SELECT (SELECT COUNT(*) FROM tbl_user WHERE tomb=0), "
                     "(SELECT COUNT(*) FROM tbl_file WHERE deleted=0), "
                     "(SELECT CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM tbl_file WHERE deleted=0), "
                     "(SELECT COUNT(*) FROM tbl_share WHERE revoked=0)";
        push_mysql(series, sql, [resp](WFMySQLTask* task) {
            long long users = 0, files = 0, storage = 0, shares = 0;
            if (mysql_ok(task)) {
                MySQLResultCursor c{ task->get_resp() }; std::vector<MySQLCell> row;
                if (c.fetch_row(row)) {
                    users = cell_ll(row[0]); files = cell_ll(row[1]);
                    storage = (long long)row[2].as_ulonglong(); shares = cell_ll(row[3]);
                }
            }
            auto now = std::chrono::steady_clock::now();
            long long uptime = std::chrono::duration_cast<std::chrono::seconds>(now - g_metrics.start).count();
            std::string b;
            auto line = [&](const char* type, const char* name, const char* help, long long v) {
                b += "# HELP "; b += name; b += ' '; b += help; b += '\n';
                b += "# TYPE "; b += name; b += ' '; b += type; b += '\n';
                b += name; b += ' '; b += std::to_string(v); b += '\n';
            };
            b += "# HELP cloudvault_build_info Build/brand info\n# TYPE cloudvault_build_info gauge\n";
            b += "cloudvault_build_info{app=\"CloudVault\",version=\"1.0\"} 1\n";
            line("gauge",   "cloudvault_uptime_seconds",       "Seconds since process start",       uptime);
            line("counter", "cloudvault_logins_total",          "Successful logins",                 g_metrics.logins.load());
            line("counter", "cloudvault_login_failures_total",  "Failed login attempts",             g_metrics.login_fails.load());
            line("counter", "cloudvault_uploads_total",         "Files uploaded",                    g_metrics.uploads.load());
            line("counter", "cloudvault_upload_bytes_total",    "Bytes uploaded",                    g_metrics.upload_bytes.load());
            line("counter", "cloudvault_downloads_total",       "Files downloaded",                  g_metrics.downloads.load());
            line("counter", "cloudvault_download_bytes_total",  "Bytes downloaded",                  g_metrics.download_bytes.load());
            line("counter", "cloudvault_shares_created_total",  "Share links created",               g_metrics.shares_created.load());
            line("counter", "cloudvault_api_token_auth_total",  "Requests authenticated via API token", g_metrics.token_auth.load());
            line("counter", "cloudvault_webdav_requests_total", "WebDAV requests handled",           g_metrics.webdav_requests.load());
            line("gauge",   "cloudvault_users",                 "Registered users",                  users);
            line("gauge",   "cloudvault_files",                 "Active (non-deleted) files",        files);
            line("gauge",   "cloudvault_storage_bytes",         "Logical storage in use",            storage);
            line("gauge",   "cloudvault_shares_active",         "Active share links",                shares);
            resp->add_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
            resp->String(b);
        });
    });
}
