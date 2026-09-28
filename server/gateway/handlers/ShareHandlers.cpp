// =============================================================================
// Share 处理器: 公开分享链接 / 提取码 / 转存
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

// ----- 分享: 公开链接 / 提取码 / 过期 / 下载限次 / 转存 --------------------------
namespace {
struct ShareRow {
    long long id = 0, uid = 0, file_id = 0, folder_id = 0;
    int is_folder = 0; long long max_downloads = 0, downloads = 0; int revoked = 0; bool expired = false;
    string code, name;
};

// 加载并校验分享 (存在/未取消/未过期/提取码)。校验通过后回调, 否则已写好错误响应。
void with_valid_share(SeriesWork* series, HttpResp* resp, const string& token,
                      const string& code, bool needAccess,
                      std::function<void(SeriesWork*, const ShareRow&)> cb)
{
    if (token.empty() || token.size() > 32) { api::fail(resp, 400, 400, "无效的分享链接"); return; }
    string sql = "SELECT id,uid,code,file_id,folder_id,is_folder,name,max_downloads,downloads,revoked,"
                 "IF(expire_at IS NOT NULL AND expire_at<NOW(),1,0) FROM tbl_share WHERE token=" + SqlUtil::quote(token);
    push_mysql(series, sql, [resp, code, needAccess, cb](WFMySQLTask* t) {
        if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
        MySQLResultCursor c{ t->get_resp() };
        std::vector<MySQLCell> r;
        if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "分享不存在或已失效"); return; }
        ShareRow s;
        s.id = cell_ll(r[0]); s.uid = cell_ll(r[1]); s.code = r[2].as_string();
        s.file_id = cell_ll(r[3]); s.folder_id = cell_ll(r[4]); s.is_folder = (int)cell_ll(r[5]);
        s.name = r[6].as_string(); s.max_downloads = cell_ll(r[7]); s.downloads = cell_ll(r[8]);
        s.revoked = (int)cell_ll(r[9]); s.expired = cell_ll(r[10]) != 0;
        if (s.revoked) { api::fail(resp, 404, 404, "分享已被取消"); return; }
        if (s.expired) { api::fail(resp, 410, 410, "分享已过期"); return; }
        if (needAccess && !s.code.empty() && code != s.code) { api::fail(resp, 403, 403, "提取码错误"); return; }
        cb(series_of(t), s);
    });
}
} // namespace

void CloudiskServer::register_share_module()
{
    // -------- 创建分享 (鉴权) --------
    m_server.POST("/api/share/create", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long fileId   = in.contains("fileId")   && in["fileId"].is_number()   ? in["fileId"].get<long long>()   : 0;
        long long folderId = in.contains("folderId") && in["folderId"].is_number() ? in["folderId"].get<long long>() : 0;
        if ((fileId > 0) == (folderId > 0)) { api::fail(resp, 400, 400, "需指定一个文件或文件夹"); return; }
        long long expireDays   = in.contains("expireDays")   && in["expireDays"].is_number()   ? in["expireDays"].get<long long>()   : 0;
        long long maxDownloads = in.contains("maxDownloads") && in["maxDownloads"].is_number() ? in["maxDownloads"].get<long long>() : 0;
        if (expireDays < 0 || expireDays > 3650 || maxDownloads < 0 || maxDownloads > 1000000) { api::fail(resp, 400, 400, "参数超出范围"); return; }
        bool autoCode = in.contains("autoCode") && in["autoCode"].is_boolean() && in["autoCode"].get<bool>();
        string code = json_str(in, "code");
        if (code.empty() && autoCode) code = gen_token(4);
        if (code.size() > 16) { api::fail(resp, 400, 400, "提取码过长"); return; }
        for (char ch : code) if (!isalnum((unsigned char)ch)) { api::fail(resp, 400, 400, "提取码仅限字母数字"); return; }

        long long uid = user.id;
        bool isFolder = folderId > 0;
        string username = user.username, ip = client_ip(req);
        string qName = isFolder
            ? "SELECT name FROM tbl_folder WHERE id=" + std::to_string(folderId) + " AND uid=" + std::to_string(uid) + " AND deleted=0"
            : "SELECT filename FROM tbl_file WHERE id=" + std::to_string(fileId) + " AND uid=" + std::to_string(uid) + " AND deleted=0";
        push_mysql(series, qName, [resp, uid, username, ip, fileId, folderId, isFolder, code, expireDays, maxDownloads](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "创建失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "要分享的对象不存在"); return; }
            string name = row[0].as_string();
            string token = gen_token(22);
            string expireExpr = expireDays > 0 ? ("DATE_ADD(NOW(), INTERVAL " + std::to_string(expireDays) + " DAY)") : "NULL";
            string ins = "INSERT INTO tbl_share (uid, token, code, file_id, folder_id, is_folder, name, expire_at, max_downloads) VALUES ("
                + std::to_string(uid) + ", " + SqlUtil::quote(token) + ", " + SqlUtil::quote(code) + ", "
                + std::to_string(fileId) + ", " + std::to_string(folderId) + ", " + (isFolder ? "1" : "0") + ", "
                + SqlUtil::quote(name) + ", " + expireExpr + ", " + std::to_string(maxDownloads) + ")";
            push_mysql(series_of(t), ins, [resp, uid, username, ip, token, code, isFolder, name, expireDays, maxDownloads](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "创建失败"); return; }
                g_metrics.shares_created++;
                audit_log(uid, username, "share_create", (isFolder ? "文件夹: " : "文件: ") + name, ip);
                api::ok(resp, {
                    {"token", token}, {"code", code}, {"isFolder", isFolder},
                    {"name", name}, {"expireDays", expireDays}, {"maxDownloads", maxDownloads},
                    {"path", "/share.html?t=" + token},
                }, "分享已创建");
            });
        });
    });

    // -------- 我的分享列表 (鉴权) --------
    m_server.GET("/api/share/mine", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string sql = "SELECT id, token, code, is_folder, name, max_downloads, downloads, views, created_at, "
                     "IFNULL(DATE_FORMAT(expire_at,'%Y-%m-%d %H:%i:%s'),''), IF(expire_at IS NOT NULL AND expire_at<NOW(),1,0) "
                     "FROM tbl_share WHERE uid=" + std::to_string(user.id) + " AND revoked=0 ORDER BY created_at DESC";
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> r;
            nlohmann::json arr = nlohmann::json::array();
            while (c.fetch_row(r)) {
                arr.push_back({
                    {"id", cell_ll(r[0])}, {"token", r[1].as_string()}, {"code", r[2].as_string()},
                    {"isFolder", cell_ll(r[3]) != 0}, {"name", r[4].as_string()},
                    {"maxDownloads", cell_ll(r[5])}, {"downloads", cell_ll(r[6])}, {"views", cell_ll(r[7])},
                    {"createdAt", r[8].as_datetime()}, {"expireAt", r[9].as_string()}, {"expired", cell_ll(r[10]) != 0},
                });
            }
            api::ok(resp, {{"shares", arr}});
        });
    });

    // -------- 取消分享 (鉴权) --------
    m_server.POST("/api/share/cancel", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long id = in.contains("id") && in["id"].is_number() ? in["id"].get<long long>() : 0;
        if (id <= 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        string sql = "UPDATE tbl_share SET revoked=1 WHERE id=" + std::to_string(id) + " AND uid=" + std::to_string(user.id);
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "取消失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            if (c.get_affected_rows() == 0) { api::fail(resp, 404, 404, "分享不存在"); return; }
            api::ok(resp, {}, "已取消分享");
        });
    });

    // -------- 分享信息 (公开) --------
    m_server.GET("/api/share/info", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        string token = req->has_query("token") ? req->query("token") : "";
        with_valid_share(series, resp, token, "", false, [resp](SeriesWork* s, const ShareRow& sh) {
            string up = "UPDATE tbl_share SET views=views+1 WHERE id=" + std::to_string(sh.id);
            push_mysql(s, up, [resp, sh](WFMySQLTask*) {
                api::ok(resp, {
                    {"name", sh.name}, {"isFolder", sh.is_folder != 0},
                    {"needCode", !sh.code.empty()},
                    {"downloads", sh.downloads}, {"maxDownloads", sh.max_downloads},
                });
            });
        });
    });

    // -------- 浏览分享内容 (公开, 需提取码) --------
    m_server.GET("/api/share/browse", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        string token = req->has_query("token") ? req->query("token") : "";
        string code  = req->has_query("code")  ? req->query("code")  : "";
        long long reqFolder = SqlUtil::to_uint(req->has_query("folderId") ? req->query("folderId") : "", 0, 1000000000000LL);
        with_valid_share(series, resp, token, code, true, [resp, reqFolder](SeriesWork* s, const ShareRow& sh) {
            long long owner = sh.uid;
            // 文件分享: 直接返回单文件
            if (!sh.is_folder) {
                string q = "SELECT id, filename, size, hashcode FROM tbl_file WHERE id=" + std::to_string(sh.file_id)
                         + " AND uid=" + std::to_string(owner) + " AND deleted=0";
                push_mysql(s, q, [resp, sh](WFMySQLTask* t) {
                    if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
                    MySQLResultCursor c{ t->get_resp() };
                    std::vector<MySQLCell> r;
                    nlohmann::json files = nlohmann::json::array();
                    if (c.fetch_row(r))
                        files.push_back({ {"id", r[0].as_ulonglong()}, {"filename", r[1].as_string()},
                                          {"size", r[2].as_ulonglong()}, {"hash", r[3].as_string()} });
                    api::ok(resp, { {"isFolder", false}, {"rootName", sh.name},
                                    {"breadcrumb", nlohmann::json::array()}, {"folders", nlohmann::json::array()}, {"files", files} });
                });
                return;
            }
            // 文件夹分享: 先取分享根路径
            string qroot = "SELECT path FROM tbl_folder WHERE id=" + std::to_string(sh.folder_id)
                         + " AND uid=" + std::to_string(owner) + " AND deleted=0";
            push_mysql(s, qroot, [resp, sh, reqFolder, owner](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> r;
                if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "分享内容已不存在"); return; }
                string rootPath = r[0].as_string();
                long long fid = (reqFolder == 0 || reqFolder == sh.folder_id) ? sh.folder_id : reqFolder;

                auto listChildren = [resp, sh, owner, rootPath](SeriesWork* s2, const string& curPath, long long curId) {
                    string qf = "SELECT id, name FROM tbl_folder WHERE uid=" + std::to_string(owner)
                              + " AND deleted=0 AND parent_id=" + std::to_string(curId) + " ORDER BY name ASC";
                    push_mysql(s2, qf, [resp, sh, owner, rootPath, curPath, curId](WFMySQLTask* tf) {
                        if (!mysql_ok(tf)) { api::fail(resp, 500, 500, "查询失败"); return; }
                        auto folders = std::make_shared<nlohmann::json>(nlohmann::json::array());
                        MySQLResultCursor cf{ tf->get_resp() };
                        std::vector<MySQLCell> rf;
                        while (cf.fetch_row(rf)) folders->push_back({ {"id", rf[0].as_ulonglong()}, {"name", rf[1].as_string()} });
                        string ql = "SELECT id, filename, size, hashcode FROM tbl_file WHERE uid=" + std::to_string(owner)
                                  + " AND deleted=0 AND parent_id=" + std::to_string(curId) + " ORDER BY filename ASC";
                        push_mysql(series_of(tf), ql, [resp, sh, folders, rootPath, curPath, owner, curId](WFMySQLTask* tl) {
                            if (!mysql_ok(tl)) { api::fail(resp, 500, 500, "查询失败"); return; }
                            nlohmann::json files = nlohmann::json::array();
                            MySQLResultCursor cl{ tl->get_resp() };
                            std::vector<MySQLCell> rl;
                            while (cl.fetch_row(rl))
                                files.push_back({ {"id", rl[0].as_ulonglong()}, {"filename", rl[1].as_string()},
                                                  {"size", rl[2].as_ulonglong()}, {"hash", rl[3].as_string()} });
                            // 面包屑: 解析 curPath 中从分享根开始的 id 段
                            std::vector<long long> segs;
                            { string cur; for (char ch : curPath) { if (ch == '/') { if (!cur.empty()) { segs.push_back(std::stoll(cur)); cur.clear(); } } else cur += ch; } }
                            std::vector<long long> rootSegs;
                            { string cur; for (char ch : rootPath) { if (ch == '/') { if (!cur.empty()) { rootSegs.push_back(std::stoll(cur)); cur.clear(); } } else cur += ch; } }
                            std::vector<long long> bcIds;
                            for (size_t i = rootSegs.size() ? rootSegs.size() - 1 : 0; i < segs.size(); ++i) bcIds.push_back(segs[i]);
                            auto respond = [resp, sh, folders, files](const nlohmann::json& bc) {
                                api::ok(resp, { {"isFolder", true}, {"rootName", sh.name}, {"folderId", 0},
                                                {"breadcrumb", bc}, {"folders", *folders}, {"files", files} });
                            };
                            if (bcIds.size() <= 1) { respond(nlohmann::json::array()); return; }
                            string bsql = "SELECT id,name FROM tbl_folder WHERE uid=" + std::to_string(owner)
                                        + " AND id IN (" + ids_csv(bcIds) + ") ORDER BY FIELD(id," + ids_csv(bcIds) + ")";
                            push_mysql(series_of(tl), bsql, [resp, respond](WFMySQLTask* tb) {
                                nlohmann::json bc = nlohmann::json::array();
                                if (mysql_ok(tb)) {
                                    MySQLResultCursor cb2{ tb->get_resp() };
                                    std::vector<MySQLCell> rb;
                                    while (cb2.fetch_row(rb)) bc.push_back({ {"id", rb[0].as_ulonglong()}, {"name", rb[1].as_string()} });
                                }
                                respond(bc);
                            });
                        });
                    });
                };

                if (fid == sh.folder_id) { listChildren(series_of(t), rootPath, fid); return; }
                // 校验请求目录在分享子树内
                string qcur = "SELECT path FROM tbl_folder WHERE id=" + std::to_string(fid) + " AND uid=" + std::to_string(owner)
                            + " AND deleted=0 AND path LIKE " + SqlUtil::quote(rootPath + "%");
                push_mysql(series_of(t), qcur, [resp, listChildren, fid](WFMySQLTask* tc) {
                    if (!mysql_ok(tc)) { api::fail(resp, 500, 500, "查询失败"); return; }
                    MySQLResultCursor cc{ tc->get_resp() };
                    std::vector<MySQLCell> rc;
                    if (!cc.fetch_row(rc)) { api::fail(resp, 403, 403, "非法目录"); return; }
                    listChildren(series_of(tc), rc[0].as_string(), fid);
                });
            });
        });
    });

    // -------- 下载分享中的文件 (公开, 需提取码) --------
    m_server.GET("/api/share/download", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        string token = req->has_query("token") ? req->query("token") : "";
        string code  = req->has_query("code")  ? req->query("code")  : "";
        long long id = SqlUtil::to_uint(req->has_query("id") ? req->query("id") : "", 0, 1000000000000LL);
        with_valid_share(series, resp, token, code, true, [resp, id](SeriesWork* s, const ShareRow& sh) {
            if (sh.max_downloads > 0 && sh.downloads >= sh.max_downloads) { api::fail(resp, 403, 403, "下载次数已用尽"); return; }
            long long owner = sh.uid;
            long long fileId = sh.is_folder ? id : sh.file_id;
            if (fileId <= 0) { api::fail(resp, 400, 400, "缺少文件 id"); return; }

            auto stream = [resp, sh](SeriesWork* s2, const string& fname, const string& hash) {
                string blob = g_blob_dir + "/" + hash;
                if (!file_exists(blob)) { api::fail(resp, 404, 404, "文件内容缺失"); return; }
                string up = "UPDATE tbl_share SET downloads=downloads+1 WHERE id=" + std::to_string(sh.id);
                push_mysql(s2, up, [resp, fname, blob](WFMySQLTask*) {
                    resp->add_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
                    resp->File(blob);
                });
            };

            if (!sh.is_folder) {
                string q = "SELECT filename, hashcode FROM tbl_file WHERE id=" + std::to_string(fileId)
                         + " AND uid=" + std::to_string(owner) + " AND deleted=0";
                push_mysql(s, q, [resp, stream](WFMySQLTask* t) {
                    if (!mysql_ok(t)) { api::fail(resp, 500, 500, "下载失败"); return; }
                    MySQLResultCursor c{ t->get_resp() };
                    std::vector<MySQLCell> r;
                    if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "文件不存在"); return; }
                    stream(series_of(t), r[0].as_string(), r[1].as_string());
                });
                return;
            }
            // 文件夹分享: 校验目标文件在子树内
            string qroot = "SELECT path FROM tbl_folder WHERE id=" + std::to_string(sh.folder_id)
                         + " AND uid=" + std::to_string(owner) + " AND deleted=0";
            push_mysql(s, qroot, [resp, owner, fileId, stream](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "下载失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> r;
                if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "分享内容已不存在"); return; }
                string rootPath = r[0].as_string();
                string q = "SELECT filename, hashcode FROM tbl_file WHERE id=" + std::to_string(fileId)
                         + " AND uid=" + std::to_string(owner) + " AND deleted=0 AND parent_id IN ("
                         + "SELECT id FROM tbl_folder WHERE uid=" + std::to_string(owner) + " AND path LIKE " + SqlUtil::quote(rootPath + "%") + ")";
                push_mysql(series_of(t), q, [resp, stream](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "下载失败"); return; }
                    MySQLResultCursor c2{ t2->get_resp() };
                    std::vector<MySQLCell> r2;
                    if (!c2.fetch_row(r2)) { api::fail(resp, 403, 403, "文件不在分享范围内"); return; }
                    stream(series_of(t2), r2[0].as_string(), r2[1].as_string());
                });
            });
        });
    });

    // -------- 转存到我的网盘 (鉴权 + 需提取码) --------
    m_server.POST("/api/share/save", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string token = json_str(in, "token");
        string code  = json_str(in, "code");
        long long targetId = in.contains("targetId") && in["targetId"].is_number() ? in["targetId"].get<long long>() : 0;
        long long me = user.id;

        with_valid_share(series, resp, token, code, true, [resp, me, targetId](SeriesWork* s, const ShareRow& sh) {
            long long owner = sh.uid;
            // 目标目录路径 (我的)
            auto withTargetPath = [resp, me, targetId, sh, owner](SeriesWork* s2, const string& targetPath) {
                if (!sh.is_folder) {
                    // 单文件转存: 读原文件 -> 配额校验 -> 插入
                    string q = "SELECT filename, hashcode, size FROM tbl_file WHERE id=" + std::to_string(sh.file_id)
                             + " AND uid=" + std::to_string(owner) + " AND deleted=0";
                    push_mysql(s2, q, [resp, me, targetId](WFMySQLTask* t) {
                        if (!mysql_ok(t)) { api::fail(resp, 500, 500, "转存失败"); return; }
                        MySQLResultCursor c{ t->get_resp() };
                        std::vector<MySQLCell> r;
                        if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "文件不存在"); return; }
                        auto fname = std::make_shared<string>(r[0].as_string());
                        auto hash  = std::make_shared<string>(r[1].as_string());
                        long long size = r[2].as_ulonglong();
                        string usage = usage_quota_sql(me);
                        push_mysql(series_of(t), usage, [resp, me, targetId, fname, hash, size](WFMySQLTask* tu) {
                            long long used = 0, quota = g_user_quota;
                            if (mysql_ok(tu)) { MySQLResultCursor cu{ tu->get_resp() }; std::vector<MySQLCell> ru; if (cu.fetch_row(ru)) { quota = cell_ll(ru[0]); used = (long long)ru[1].as_ulonglong(); } }
                            if (used + size > quota) { api::fail(resp, 413, 413, "存储空间不足"); return; }
                            string ins = "INSERT INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                                + std::to_string(me) + ", " + std::to_string(targetId) + ", " + SqlUtil::quote(*fname) + ", "
                                + SqlUtil::quote(*hash) + ", " + std::to_string(size) + ")";
                            push_mysql(series_of(tu), ins, [resp](WFMySQLTask* ti) {
                                if (!mysql_ok(ti)) { api::fail(resp, 409, 409, "目标目录已存在同名文件"); return; }
                                api::ok(resp, {{"files", 1}, {"folders", 0}}, "已转存到我的网盘");
                            });
                        });
                    });
                    return;
                }
                // 文件夹转存: 复制整棵子树
                string qroot = "SELECT path FROM tbl_folder WHERE id=" + std::to_string(sh.folder_id)
                             + " AND uid=" + std::to_string(owner) + " AND deleted=0";
                push_mysql(s2, qroot, [resp, me, targetId, owner, targetPath](WFMySQLTask* t) {
                    if (!mysql_ok(t)) { api::fail(resp, 500, 500, "转存失败"); return; }
                    MySQLResultCursor c{ t->get_resp() };
                    std::vector<MySQLCell> r;
                    if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "分享内容已不存在"); return; }
                    string rootPath = r[0].as_string();
                    // 取子树全部文件夹 (按路径长度升序: 父先于子)
                    string qf = "SELECT id, name, parent_id FROM tbl_folder WHERE uid=" + std::to_string(owner)
                              + " AND deleted=0 AND path LIKE " + SqlUtil::quote(rootPath + "%") + " ORDER BY CHAR_LENGTH(path) ASC";
                    push_mysql(series_of(t), qf, [resp, me, targetId, owner, targetPath, rootPath](WFMySQLTask* tf) {
                        if (!mysql_ok(tf)) { api::fail(resp, 500, 500, "转存失败"); return; }
                        auto folders = std::make_shared<std::vector<std::array<long long,2>>>(); // {oldId, oldParent}
                        auto fnames  = std::make_shared<std::vector<string>>();
                        MySQLResultCursor cf{ tf->get_resp() };
                        std::vector<MySQLCell> rf;
                        while (cf.fetch_row(rf)) { folders->push_back({ (long long)rf[0].as_ulonglong(), (long long)rf[2].as_ulonglong() }); fnames->push_back(rf[1].as_string()); }
                        if (folders->empty()) { api::fail(resp, 404, 404, "分享内容已不存在"); return; }
                        long long rootOldId = (*folders)[0][0];

                        // 取子树全部文件
                        string qfile = "SELECT parent_id, filename, hashcode, size FROM tbl_file WHERE uid=" + std::to_string(owner)
                                     + " AND deleted=0 AND parent_id IN (SELECT id FROM tbl_folder WHERE uid=" + std::to_string(owner)
                                     + " AND path LIKE " + SqlUtil::quote(rootPath + "%") + ")";
                        push_mysql(series_of(tf), qfile, [resp, me, targetId, targetPath, folders, fnames, rootOldId](WFMySQLTask* tfl) {
                            if (!mysql_ok(tfl)) { api::fail(resp, 500, 500, "转存失败"); return; }
                            struct FileRow { long long parent; string name, hash; long long size; };
                            auto files = std::make_shared<std::vector<FileRow>>();
                            long long total = 0;
                            MySQLResultCursor cl{ tfl->get_resp() };
                            std::vector<MySQLCell> rl;
                            while (cl.fetch_row(rl)) { FileRow fr{ (long long)rl[0].as_ulonglong(), rl[1].as_string(), rl[2].as_string(), (long long)rl[3].as_ulonglong() }; total += fr.size; files->push_back(fr); }

                            // 配额校验
                            string usage = usage_quota_sql(me);
                            push_mysql(series_of(tfl), usage, [resp, me, targetId, targetPath, folders, fnames, files, rootOldId, total](WFMySQLTask* tu) {
                                long long used = 0, quota = g_user_quota;
                                if (mysql_ok(tu)) { MySQLResultCursor cu{ tu->get_resp() }; std::vector<MySQLCell> ru; if (cu.fetch_row(ru)) { quota = cell_ll(ru[0]); used = (long long)ru[1].as_ulonglong(); } }
                                if (used + total > quota) { api::fail(resp, 413, 413, "存储空间不足"); return; }

                                auto idMap   = std::make_shared<std::map<long long,long long>>(); // old->new folder id
                                auto pathMap = std::make_shared<std::map<long long,string>>();     // old->new folder path
                                auto idx = std::make_shared<size_t>(0);
                                auto step = std::make_shared<std::function<void(SeriesWork*)>>();

                                *step = [=](SeriesWork* sw) {
                                    if (*idx >= folders->size()) {
                                        // 插入文件 (多语句)
                                        if (files->empty()) { api::ok(resp, {{"folders", (long long)folders->size()}, {"files", 0}}, "已转存到我的网盘"); return; }
                                        string q;
                                        for (auto& fr : *files) {
                                            auto it = idMap->find(fr.parent);
                                            if (it == idMap->end()) continue;
                                            q += "INSERT INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                                               + std::to_string(me) + ", " + std::to_string(it->second) + ", " + SqlUtil::quote(fr.name) + ", "
                                               + SqlUtil::quote(fr.hash) + ", " + std::to_string(fr.size) + "); ";
                                        }
                                        int fcount = (int)folders->size();
                                        push_mysql(sw, q, [resp, fcount](WFMySQLTask* ti) {
                                            if (!mysql_ok(ti)) { api::fail(resp, 500, 500, "转存文件失败"); return; }
                                            api::ok(resp, {{"folders", fcount}}, "已转存到我的网盘");
                                        });
                                        return;
                                    }
                                    size_t i = (*idx)++;
                                    long long oldId = (*folders)[i][0], oldParent = (*folders)[i][1];
                                    const string& nm = (*fnames)[i];
                                    long long newParent = (oldId == rootOldId) ? targetId : (*idMap)[oldParent];
                                    string newParentPath = (oldId == rootOldId) ? targetPath : (*pathMap)[oldParent];
                                    string ins = "INSERT INTO tbl_folder (uid, name, parent_id, path) VALUES ("
                                        + std::to_string(me) + ", " + SqlUtil::quote(nm) + ", " + std::to_string(newParent) + ", '/')";
                                    push_mysql(sw, ins, [=](WFMySQLTask* ti) {
                                        if (!mysql_ok(ti)) { api::fail(resp, 409, 409, "目标目录已存在同名文件夹"); return; }
                                        MySQLResultCursor ci{ ti->get_resp() };
                                        long long newId = ci.get_insert_id();
                                        string newPath = newParentPath + std::to_string(newId) + "/";
                                        (*idMap)[oldId] = newId; (*pathMap)[oldId] = newPath;
                                        string up = "UPDATE tbl_folder SET path=" + SqlUtil::quote(newPath) + " WHERE id=" + std::to_string(newId);
                                        push_mysql(series_of(ti), up, [=](WFMySQLTask* tup) { (*step)(series_of(tup)); });
                                    });
                                };
                                (*step)(series_of(tu));
                            });
                        });
                    });
                });
            };

            if (targetId == 0) { withTargetPath(s, "/"); return; }
            string qt = "SELECT path FROM tbl_folder WHERE id=" + std::to_string(targetId) + " AND uid=" + std::to_string(me) + " AND deleted=0";
            push_mysql(s, qt, [resp, withTargetPath](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "转存失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> r;
                if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "目标文件夹不存在"); return; }
                withTargetPath(series_of(t), r[0].as_string());
            });
        });
    });
}
