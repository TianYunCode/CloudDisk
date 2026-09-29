// =============================================================================
// Auth 处理器: 注册 / 登录 / 用户信息 / 账户 / API 令牌
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

// ----- 注册 --------------------------------------------------------------------
void CloudiskServer::register_signup_module()
{
    m_server.POST("/api/auth/register", [](const HttpReq* req, HttpResp* resp) {
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string username = json_str(in, "username");
        string password = json_str(in, "password");

        if (!SqlUtil::valid_username(username)) { api::fail(resp, 400, 400, "用户名需为 3-32 位字母/数字/下划线/中划线"); return; }
        if (!SqlUtil::valid_password(password)) { api::fail(resp, 400, 400, "密码长度需为 6-64 个字符"); return; }

        string ip; unsigned short port = 0;
        if (!discover_userservice(ip, port)) { api::fail(resp, 503, 503, "用户服务不可用"); return; }

        UserService::SRPCClient client{ ip.c_str(), port };
        UserRequest request; request.set_username(username); request.set_password(password);
        UserResponse response; srpc::RPCSyncContext ctx;
        client.sign_up(&request, &response, &ctx);

        if (ctx.success && response.success()) {
            LOG_INFO("用户注册成功: " << username);
            // 引导管理员: 若系统尚无管理员, 将最小 id 用户设为管理员 (fire-and-forget)
            string boot = "UPDATE tbl_user t JOIN (SELECT MIN(id) AS mid FROM tbl_user) m ON t.id=m.mid "
                          "SET t.role=1 WHERE NOT EXISTS (SELECT 1 FROM (SELECT id FROM tbl_user WHERE role=1 LIMIT 1) e)";
            WFMySQLTask* bt = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
            bt->get_req()->set_query(boot); bt->start();
            audit_log(0, username, AuditAction::Register, "", client_ip(req));
            api::ok(resp, {{"username", username}}, "注册成功");
        } else {
            api::fail(resp, 409, 409, "用户名已存在");
        }
    });
}

// ----- 登录 (网关侧校验: 支持两步验证 / 登录限速 / 禁用检测 / 审计) -----------------
void CloudiskServer::register_signin_module()
{
    m_server.POST("/api/auth/login", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string username = json_str(in, "username");
        string password = json_str(in, "password");
        string code     = json_str(in, "code");   // 可选两步验证码
        if (username.empty() || password.empty()) { api::fail(resp, 400, 400, "用户名或密码不能为空"); return; }

        string ip = client_ip(req);
        string rlkey = username + "|" + ip;
        int wait = rl_blocked(rlkey);
        if (wait > 0) {
            audit_log(0, username, AuditAction::LoginBlocked, "剩余冷却 " + std::to_string(wait) + "s", ip);
            api::fail(resp, 429, 429, "尝试过于频繁, 请 " + std::to_string(wait) + " 秒后再试");
            return;
        }

        string sql = "SELECT id, username, password, salt, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s'), "
                     "disabled, totp_enabled, totp_secret, role FROM tbl_user WHERE username="
                   + SqlUtil::quote(username) + " AND tomb=0";
        push_mysql(series, sql, [resp, username, password, code, ip, rlkey](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "登录失败"); return; }
            MySQLResultCursor cur{ task->get_resp() };
            std::vector<MySQLCell> row;
            auto failLogin = [&](const string& why) {
                rl_on_fail(rlkey);
                g_metrics.login_fails++;
                audit_log(0, username, AuditAction::LoginFail, why, ip);
                api::fail(resp, 401, 401, "用户名或密码错误");
            };
            if (!cur.fetch_row(row)) { failLogin("no such user"); return; }
            User u;
            u.id = (int)cell_ll(row[0]); u.username = row[1].as_string();
            string hash = row[2].as_string(); u.salt = row[3].as_string();
            u.createdAt = row[4].is_string() ? row[4].as_string() : string();
            int disabled = (int)cell_ll(row[5]); int totpEnabled = (int)cell_ll(row[6]);
            string totpSecret = row[7].as_string(); int role = (int)cell_ll(row[8]);

            if (disabled) { audit_log(u.id, username, AuditAction::LoginDisabled, "", ip); api::fail(resp, 403, 403, "账户已被禁用"); return; }
            if (CryptoUtil::hash_password(password, u.salt) != hash) { failLogin("bad password"); return; }
            if (totpEnabled) {
                if (code.empty()) { api::fail(resp, 401, 4012, "需要两步验证码"); return; }
                if (!totp_verify(totpSecret, code)) { rl_on_fail(rlkey); audit_log(u.id, username, AuditAction::Login2faFail, "", ip); api::fail(resp, 401, 4012, "两步验证码错误"); return; }
            }
            rl_on_success(rlkey);
            g_metrics.logins++;
            string token = CryptoUtil::generate_token(u);
            audit_log(u.id, username, AuditAction::Login, "", ip);
            api::ok(resp, {
                {"token", token}, {"username", u.username}, {"createdAt", u.createdAt},
                {"role", role}, {"need2fa", false},
            }, "登录成功");
        });
    });
}

// ----- 用户信息 + 用量统计 ------------------------------------------------------
void CloudiskServer::register_userinfo_module()
{
    m_server.GET("/api/user/info", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;

        string g = std::to_string(g_user_quota);
        string uid = std::to_string(user.id);
        string sql = "SELECT u.nickname, u.email, u.avatar_hash, u.role, u.totp_enabled, IF(u.quota>0,u.quota," + g + "), "
                     "(SELECT COUNT(*) FROM tbl_file WHERE deleted=0 AND uid=u.id), "
                     "(SELECT CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM tbl_file WHERE deleted=0 AND uid=u.id) "
                     "FROM tbl_user u WHERE u.id=" + uid;
        string username = user.username, createdAt = user.createdAt;
        int myid = user.id;

        push_mysql(series, sql, [resp, username, createdAt, myid](WFMySQLTask* task) {
            string nickname, email, avatar; long long role = 0, totp = 0, quota = g_user_quota, cnt = 0, total = 0;
            if (mysql_ok(task)) {
                MySQLResultCursor cursor{ task->get_resp() };
                std::vector<MySQLCell> row;
                if (cursor.fetch_row(row)) {
                    nickname = row[0].as_string(); email = row[1].as_string(); avatar = row[2].as_string();
                    role = cell_ll(row[3]); totp = cell_ll(row[4]); quota = cell_ll(row[5]);
                    cnt = cell_ll(row[6]); total = (long long)row[7].as_ulonglong();
                }
            }
            api::ok(resp, {
                {"uid",         myid},
                {"username",    username},
                {"createdAt",   createdAt},
                {"nickname",    nickname},
                {"email",       email},
                {"avatar",      avatar},
                {"hasAvatar",   !avatar.empty()},
                {"role",        role},
                {"twoFactor",   totp != 0},
                {"fileCount",   cnt},
                {"storageUsed", total},
                {"quota",       quota},
            });
        });
    });
}

// =============================================================================
// 账户: 资料 / 头像 / 改密 / 两步验证 (2FA)
// =============================================================================
void CloudiskServer::register_account_module()
{
    // 更新资料 (昵称 / 邮箱)
    m_server.POST("/api/user/profile", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string nickname = json_str(in, "nickname");
        string email    = json_str(in, "email");
        if (nickname.size() > 64) { api::fail(resp, 400, 400, "昵称过长"); return; }
        if (email.size() > 128) { api::fail(resp, 400, 400, "邮箱过长"); return; }
        if (!email.empty()) {
            auto at = email.find('@');
            if (at == string::npos || at == 0 || at + 1 >= email.size() || email.find('.', at) == string::npos) {
                api::fail(resp, 400, 400, "邮箱格式不正确"); return;
            }
        }
        string sql = "UPDATE tbl_user SET nickname=" + SqlUtil::quote(nickname) + ", email=" + SqlUtil::quote(email)
                   + " WHERE id=" + std::to_string(user.id);
        string uname = user.username, ip = client_ip(req); int uid = user.id;
        push_mysql(series, sql, [resp, nickname, email, uid, uname, ip](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "更新失败"); return; }
            audit_log(uid, uname, AuditAction::ProfileUpdate, "", ip);
            api::ok(resp, {{"nickname", nickname}, {"email", email}}, "资料已更新");
        });
    });

    // 修改密码
    m_server.POST("/api/user/password", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string oldPw = json_str(in, "oldPassword");
        string newPw = json_str(in, "newPassword");
        if (!SqlUtil::valid_password(newPw)) { api::fail(resp, 400, 400, "新密码长度需为 6-64 个字符"); return; }
        int uid = user.id; string uname = user.username, ip = client_ip(req);
        string sel = "SELECT password, salt FROM tbl_user WHERE id=" + std::to_string(uid);
        push_mysql(series, sel, [resp, oldPw, newPw, uid, uname, ip](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "校验失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "用户不存在"); return; }
            string hash = row[0].as_string(), salt = row[1].as_string();
            if (CryptoUtil::hash_password(oldPw, salt) != hash) {
                audit_log(uid, uname, AuditAction::PasswordChangeFail, "", ip);
                api::fail(resp, 401, 401, "原密码错误"); return;
            }
            string newSalt = CryptoUtil::generate_salt();
            string newHash = CryptoUtil::hash_password(newPw, newSalt);
            string upd = "UPDATE tbl_user SET password=" + SqlUtil::quote(newHash) + ", salt=" + SqlUtil::quote(newSalt)
                       + " WHERE id=" + std::to_string(uid);
            push_mysql(series_of(t), upd, [resp, uid, uname, ip](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "更新失败"); return; }
                audit_log(uid, uname, AuditAction::PasswordChange, "", ip);
                api::ok(resp, {}, "密码已修改, 请重新登录");
            });
        });
    });

    // 上传头像 (裁剪为 256px JPEG)
    m_server.POST("/api/user/avatar", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        if (req->content_type() != MULTIPART_FORM_DATA) { api::fail(resp, 400, 400, "需要 multipart/form-data"); return; }
        Form& form = req->form();
        const string* content = nullptr; string origName;
        for (auto& [_, file] : form) { if (!file.first.empty()) { origName = file.first; content = &file.second; break; } }
        if (!content || content->empty()) { api::fail(resp, 400, 400, "没有图片"); return; }
        if (!is_thumbnailable(origName)) { api::fail(resp, 400, 400, "仅支持图片文件"); return; }
        if ((long long)content->size() > 10 * 1024 * 1024) { api::fail(resp, 413, 413, "头像不能超过 10MB"); return; }
        int uid = user.id; string uname = user.username, ip = client_ip(req);
        string tmp = g_storage_dir + "/avatars/tmp_" + std::to_string(uid) + "_" + gen_token(6);
        string out = g_storage_dir + "/avatars/" + std::to_string(uid) + ".jpg";
        if (!write_file_all(tmp, content->data(), content->size())) { api::fail(resp, 500, 500, "写入失败"); return; }
        bool okThumb = thumb::make_thumbnail(tmp, out, 256);
        ::remove(tmp.c_str());
        if (!okThumb) { api::fail(resp, 400, 400, "图片解析失败"); return; }
        string tag = gen_token(8);
        string sql = "UPDATE tbl_user SET avatar_hash=" + SqlUtil::quote(tag) + " WHERE id=" + std::to_string(uid);
        push_mysql(series, sql, [resp, uid, uname, ip, tag](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "更新失败"); return; }
            audit_log(uid, uname, AuditAction::AvatarUpdate, "", ip);
            api::ok(resp, {{"avatar", tag}}, "头像已更新");
        });
    });

    // 读取头像 (公开: 按 uid)
    m_server.GET("/api/user/avatar", [](const HttpReq* req, HttpResp* resp) {
        long long uid = SqlUtil::to_uint(req->has_query("uid") ? req->query("uid") : "", 0, 1000000000LL);
        if (uid <= 0) { api::fail(resp, 400, 400, "缺少 uid"); return; }
        string path = g_storage_dir + "/avatars/" + std::to_string(uid) + ".jpg";
        if (!file_exists(path)) { api::fail(resp, 404, 404, "无头像"); return; }
        resp->add_header("Cache-Control", "public, max-age=86400");
        resp->add_header("Content-Type", "image/jpeg");
        resp->File(path);
        resp->headers["Content-Type"] = "image/jpeg";
    });

    // 2FA: 生成密钥 (未启用)
    m_server.POST("/api/2fa/setup", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        unsigned char raw[20]; RAND_bytes(raw, sizeof(raw));
        string secret = base32_encode(raw, sizeof(raw));
        int uid = user.id; string uname = user.username;
        string sql = "UPDATE tbl_user SET totp_secret=" + SqlUtil::quote(secret) + ", totp_enabled=0 WHERE id=" + std::to_string(uid);
        string otpauth = "otpauth://totp/CloudVault:" + uname + "?secret=" + secret + "&issuer=CloudVault&digits=6&period=30";
        push_mysql(series, sql, [resp, secret, otpauth](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "生成失败"); return; }
            api::ok(resp, {{"secret", secret}, {"otpauth", otpauth}}, "请用验证器扫码并输入验证码以启用");
        });
    });

    // 2FA: 校验验证码并启用
    m_server.POST("/api/2fa/enable", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string code = json_str(in, "code");
        int uid = user.id; string uname = user.username, ip = client_ip(req);
        string sel = "SELECT totp_secret, totp_enabled FROM tbl_user WHERE id=" + std::to_string(uid);
        push_mysql(series, sel, [resp, code, uid, uname, ip](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "校验失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "用户不存在"); return; }
            string secret = row[0].as_string();
            if (secret.empty()) { api::fail(resp, 400, 400, "请先获取密钥"); return; }
            if (!totp_verify(secret, code)) { api::fail(resp, 401, 401, "验证码错误"); return; }
            string upd = "UPDATE tbl_user SET totp_enabled=1 WHERE id=" + std::to_string(uid);
            push_mysql(series_of(t), upd, [resp, uid, uname, ip](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "启用失败"); return; }
                audit_log(uid, uname, AuditAction::TwoFaEnable, "", ip);
                api::ok(resp, {{"twoFactor", true}}, "两步验证已启用");
            });
        });
    });

    // 2FA: 校验验证码并关闭
    m_server.POST("/api/2fa/disable", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string code = json_str(in, "code");
        int uid = user.id; string uname = user.username, ip = client_ip(req);
        string sel = "SELECT totp_secret, totp_enabled FROM tbl_user WHERE id=" + std::to_string(uid);
        push_mysql(series, sel, [resp, code, uid, uname, ip](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "校验失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "用户不存在"); return; }
            string secret = row[0].as_string(); int enabled = (int)cell_ll(row[1]);
            if (!enabled) { api::fail(resp, 400, 400, "尚未启用两步验证"); return; }
            if (!totp_verify(secret, code)) { api::fail(resp, 401, 401, "验证码错误"); return; }
            string upd = "UPDATE tbl_user SET totp_enabled=0, totp_secret='' WHERE id=" + std::to_string(uid);
            push_mysql(series_of(t), upd, [resp, uid, uname, ip](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "关闭失败"); return; }
                audit_log(uid, uname, AuditAction::TwoFaDisable, "", ip);
                api::ok(resp, {{"twoFactor", false}}, "两步验证已关闭");
            });
        });
    });
}

// =============================================================================
// 个人访问令牌 (API Token): 供脚本 / WebDAV / 第三方集成使用
// =============================================================================
void CloudiskServer::register_token_module()
{
    // 注入解析器: require_auth 在 JWT 未命中时回退到此
    api::token_resolver() = [](const std::string& tok, User& u) -> bool {
        if (tok.rfind("cvt_", 0) != 0) return false;
        std::string h; bool persist = false;
        if (!g_tokens.resolve(tok, u, h, persist)) return false;
        g_metrics.token_auth++;
        if (persist) {
            string sql = "UPDATE tbl_token SET last_used_at=NOW() WHERE token_hash=" + SqlUtil::quote(h);
            WFMySQLTask* t = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
            t->get_req()->set_query(sql); t->start();
        }
        return true;
    };

    // 启动时加载所有有效令牌进内存
    {
        string load = "SELECT t.token_hash, t.uid, u.username, "
                      "DATE_FORMAT(u.created_at,'%Y-%m-%d %H:%i:%s'), IFNULL(UNIX_TIMESTAMP(t.expires_at),0) "
                      "FROM tbl_token t JOIN tbl_user u ON u.id=t.uid "
                      "WHERE t.revoked=0 AND (t.expires_at IS NULL OR t.expires_at>NOW())";
        WFMySQLTask* lt = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask* task) {
            if (!mysql_ok(task)) return;
            MySQLResultCursor c{ task->get_resp() }; std::vector<MySQLCell> row; int n = 0;
            while (c.fetch_row(row)) {
                g_tokens.put(row[0].as_string(), (int)cell_ll(row[1]), row[2].as_string(),
                             row[3].is_string() ? row[3].as_string() : string(), cell_ll(row[4]));
                n++;
            }
            LOG_INFO("已加载 " << n << " 个 API 令牌进内存");
        });
        lt->get_req()->set_query(load); lt->start();
    }

    // 创建令牌 (仅 JWT 会话可创建, 明文仅返回一次)
    m_server.POST("/api/tokens", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string name = json_str(in, "name");
        if (name.size() > 64) { api::fail(resp, 400, 400, "名称过长"); return; }
        if (name.empty()) name = "token";
        long long days = in.contains("expiresDays") && in["expiresDays"].is_number() ? in["expiresDays"].get<long long>() : 0;
        if (days < 0 || days > 3650) { api::fail(resp, 400, 400, "有效期不合法"); return; }

        string secret = "cvt_" + gen_token(40);
        string hash   = TokenRegistry::hash_of(secret);
        string prefix = secret.substr(0, 12);
        long long expEpoch = days > 0 ? (long long)time(nullptr) + days * 86400 : 0;
        string expExpr = days > 0 ? ("DATE_ADD(NOW(), INTERVAL " + std::to_string(days) + " DAY)") : string("NULL");

        int uid = user.id; string uname = user.username, createdAt = user.createdAt, ip = client_ip(req);
        string sql = "INSERT INTO tbl_token (uid, name, token_hash, prefix, expires_at) VALUES ("
                   + std::to_string(uid) + ", " + SqlUtil::quote(name) + ", " + SqlUtil::quote(hash) + ", "
                   + SqlUtil::quote(prefix) + ", " + expExpr + ")";
        push_mysql(series, sql, [resp, secret, prefix, name, days, hash, uid, uname, createdAt, expEpoch, ip](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "创建失败"); return; }
            MySQLResultCursor c{ task->get_resp() };
            long long id = (long long)c.get_insert_id();
            g_tokens.put(hash, uid, uname, createdAt, expEpoch);
            audit_log(uid, uname, AuditAction::TokenCreate, "name=" + name, ip);
            api::ok(resp, {
                {"id", id}, {"token", secret}, {"prefix", prefix},
                {"name", name}, {"expiresDays", days},
            }, "令牌已创建, 请立即保存, 仅显示一次");
        });
    });

    // 列出我的令牌 (不含明文)
    m_server.GET("/api/tokens", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string sql = "SELECT id, name, prefix, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s'), "
                     "IFNULL(DATE_FORMAT(last_used_at,'%Y-%m-%d %H:%i:%s'),''), "
                     "IFNULL(DATE_FORMAT(expires_at,'%Y-%m-%d %H:%i:%s'),''), "
                     "IF(expires_at IS NOT NULL AND expires_at<NOW(),1,0) "
                     "FROM tbl_token WHERE uid=" + std::to_string(user.id) + " AND revoked=0 ORDER BY id DESC";
        push_mysql(series, sql, [resp](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ task->get_resp() }; std::vector<MySQLCell> row;
            nlohmann::json arr = nlohmann::json::array();
            while (c.fetch_row(row)) {
                arr.push_back({
                    {"id", cell_ll(row[0])}, {"name", row[1].as_string()}, {"prefix", row[2].as_string()},
                    {"createdAt", row[3].is_string() ? row[3].as_string() : string()},
                    {"lastUsedAt", row[4].is_string() ? row[4].as_string() : string()},
                    {"expiresAt", row[5].is_string() ? row[5].as_string() : string()},
                    {"expired", cell_ll(row[6]) != 0},
                });
            }
            api::ok(resp, {{"tokens", arr}}, "ok");
        });
    });

    // 吊销令牌
    m_server.POST("/api/tokens/revoke", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long id = in.contains("id") && in["id"].is_number() ? in["id"].get<long long>() : 0;
        if (id <= 0) { api::fail(resp, 400, 400, "缺少 id"); return; }
        int uid = user.id; string uname = user.username, ip = client_ip(req);
        string sel = "SELECT token_hash FROM tbl_token WHERE id=" + std::to_string(id)
                   + " AND uid=" + std::to_string(uid) + " AND revoked=0";
        push_mysql(series, sel, [resp, id, uid, uname, ip](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "操作失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "令牌不存在"); return; }
            string hash = row[0].as_string();
            string upd = "UPDATE tbl_token SET revoked=1 WHERE id=" + std::to_string(id) + " AND uid=" + std::to_string(uid);
            push_mysql(series_of(t), upd, [resp, hash, id, uid, uname, ip](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "吊销失败"); return; }
                g_tokens.erase(hash);
                audit_log(uid, uname, AuditAction::TokenRevoke, "id=" + std::to_string(id), ip);
                api::ok(resp, {{"id", id}}, "令牌已吊销");
            });
        });
    });
}
