// =============================================================================
// Extra 处理器: 收藏 / 搜索 / 版本 / 统计 / 标签 / 活动日志
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
// 收藏夹: 文件 / 文件夹加星, 独立视图列出
// =============================================================================
void CloudiskServer::register_favorite_module()
{
    // 切换收藏: 已收藏则取消, 未收藏则添加 (会校验条目归属)
    m_server.POST("/api/favorite/toggle", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        int type = in.contains("itemType") && in["itemType"].is_number() ? in["itemType"].get<int>() : 0;
        long long id = in.contains("itemId") && in["itemId"].is_number() ? in["itemId"].get<long long>() : 0;
        if ((type != 0 && type != 1) || id <= 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        long long uid = user.id; string su = std::to_string(uid), st = std::to_string(type), si = std::to_string(id);
        string sel = "SELECT id FROM tbl_favorite WHERE uid=" + su + " AND item_type=" + st + " AND item_id=" + si + " LIMIT 1";
        push_mysql(series, sel, [resp, su, st, si, type, id](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "操作失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            bool exists = c.fetch_row(row);
            if (exists) {
                string del = "DELETE FROM tbl_favorite WHERE uid=" + su + " AND item_type=" + st + " AND item_id=" + si;
                push_mysql(series_of(t), del, [resp](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "取消失败"); return; }
                    api::ok(resp, {{"favorited", false}}, "已取消收藏");
                });
                return;
            }
            // 添加前校验条目存在且归属当前用户且未删除
            string tbl = type == 0 ? "tbl_file" : "tbl_folder";
            string chk = "SELECT id FROM " + tbl + " WHERE deleted=0 AND uid=" + su + " AND id=" + si + " LIMIT 1";
            push_mysql(series_of(t), chk, [resp, su, st, si](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "操作失败"); return; }
                MySQLResultCursor c2{ t2->get_resp() }; std::vector<MySQLCell> r2;
                if (!c2.fetch_row(r2)) { api::fail(resp, 404, 404, "条目不存在"); return; }
                string ins = "INSERT IGNORE INTO tbl_favorite (uid, item_type, item_id) VALUES (" + su + ", " + st + ", " + si + ")";
                push_mysql(series_of(t2), ins, [resp](WFMySQLTask* t3) {
                    if (!mysql_ok(t3)) { api::fail(resp, 500, 500, "收藏失败"); return; }
                    api::ok(resp, {{"favorited", true}}, "已收藏");
                });
            });
        });
    });

    // 批量收藏 (多选工具栏使用): INSERT IGNORE, 仅收藏归属自己且未删除的条目
    m_server.POST("/api/favorite/batch", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        std::vector<long long> fileIds, folderIds;
        if (!ids_from_json(in, "fileIds", fileIds) || !ids_from_json(in, "folderIds", folderIds)) {
            api::fail(resp, 400, 400, "参数不合法"); return;
        }
        if (fileIds.empty() && folderIds.empty()) { api::ok(resp, {{"added", 0}}, "无变化"); return; }
        long long uid = user.id; string su = std::to_string(uid);
        string q;
        if (!fileIds.empty())
            q += "INSERT IGNORE INTO tbl_favorite (uid, item_type, item_id) "
                 "SELECT " + su + ", 0, id FROM tbl_file WHERE deleted=0 AND uid=" + su + " AND id IN (" + ids_csv(fileIds) + "); ";
        if (!folderIds.empty())
            q += "INSERT IGNORE INTO tbl_favorite (uid, item_type, item_id) "
                 "SELECT " + su + ", 1, id FROM tbl_folder WHERE deleted=0 AND uid=" + su + " AND id IN (" + ids_csv(folderIds) + "); ";
        push_mysql(series, q, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "收藏失败"); return; }
            api::ok(resp, {}, "已加入收藏");
        });
    });

    // 收藏列表 (文件夹 + 文件, 跳过已删除)
    m_server.GET("/api/favorites", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long uid = user.id; string su = std::to_string(uid);
        string fq = "SELECT d.item_id, o.name, o.parent_id, DATE_FORMAT(d.created_at,'%Y-%m-%d %H:%i:%s') "
                    "FROM tbl_favorite d JOIN tbl_folder o ON o.id=d.item_id AND o.uid=" + su + " "
                    "WHERE d.uid=" + su + " AND d.item_type=1 AND o.deleted=0 ORDER BY d.created_at DESC";
        push_mysql(series, fq, [resp, su](WFMySQLTask* t) {
            nlohmann::json items = nlohmann::json::array();
            if (mysql_ok(t)) {
                MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                while (c.fetch_row(row)) {
                    items.push_back({ {"type", 1}, {"id", cell_ll(row[0])}, {"name", row[1].as_string()},
                                      {"parentId", cell_ll(row[2])}, {"size", 0},
                                      {"favoritedAt", row[3].is_string() ? row[3].as_string() : string()} });
                }
            }
            auto itemsPtr = std::make_shared<nlohmann::json>(items);
            string flq = "SELECT d.item_id, x.filename, x.parent_id, x.size, DATE_FORMAT(d.created_at,'%Y-%m-%d %H:%i:%s') "
                         "FROM tbl_favorite d JOIN tbl_file x ON x.id=d.item_id AND x.uid=" + su + " "
                         "WHERE d.uid=" + su + " AND d.item_type=0 AND x.deleted=0 ORDER BY d.created_at DESC";
            push_mysql(series_of(t), flq, [resp, itemsPtr](WFMySQLTask* t2) {
                if (mysql_ok(t2)) {
                    MySQLResultCursor c{ t2->get_resp() }; std::vector<MySQLCell> row;
                    while (c.fetch_row(row)) {
                        itemsPtr->push_back({ {"type", 0}, {"id", cell_ll(row[0])}, {"name", row[1].as_string()},
                                              {"parentId", cell_ll(row[2])}, {"size", (long long)row[3].as_ulonglong()},
                                              {"favoritedAt", row[4].is_string() ? row[4].as_string() : string()} });
                    }
                }
                api::ok(resp, {{"items", *itemsPtr}}, "ok");
            });
        });
    });
}

// =============================================================================
// 全局搜索: 跨全部目录按名称匹配文件与文件夹
// =============================================================================
void CloudiskServer::register_search_module()
{
    m_server.GET("/api/search", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string q = req->has_query("q") ? req->query("q") : "";
        q = wfrest::CodeUtil::url_decode(q);   // wfrest 不自动解码 query, 需手动 (支持中文)
        // 去除首尾空白
        auto trim = [](string s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            return a == string::npos ? string() : s.substr(a, b - a + 1);
        };
        q = trim(q);
        if (q.empty()) { api::ok(resp, {{"items", nlohmann::json::array()}, {"query", ""}}, "ok"); return; }
        long long limit = SqlUtil::to_uint(req->has_query("limit") ? req->query("limit") : "", 50, 200);
        long long uid = user.id; string su = std::to_string(uid), sl = std::to_string(limit);
        // 转义 LIKE 元字符, 防止 % _ 被解释为通配
        string kw = SqlUtil::escape(q);
        string esc; esc.reserve(kw.size());
        for (char c : kw) { if (c == '%' || c == '_' || c == '\\') esc += '\\'; esc += c; }
        string pat = "'%" + esc + "%' ESCAPE '\\\\'";
        string fq = "SELECT id, name, parent_id FROM tbl_folder WHERE deleted=0 AND uid=" + su +
                    " AND name LIKE " + pat + " ORDER BY name LIMIT " + sl;
        push_mysql(series, fq, [resp, su, pat, sl](WFMySQLTask* t) {
            auto items = std::make_shared<nlohmann::json>(nlohmann::json::array());
            if (mysql_ok(t)) {
                MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                while (c.fetch_row(row))
                    items->push_back({ {"type", 1}, {"id", cell_ll(row[0])}, {"name", row[1].as_string()},
                                       {"parentId", cell_ll(row[2])}, {"size", 0} });
            }
            string flq = "SELECT id, filename, parent_id, size FROM tbl_file WHERE deleted=0 AND uid=" + su +
                         " AND filename LIKE " + pat + " ORDER BY filename LIMIT " + sl;
            push_mysql(series_of(t), flq, [resp, items](WFMySQLTask* t2) {
                if (mysql_ok(t2)) {
                    MySQLResultCursor c{ t2->get_resp() }; std::vector<MySQLCell> row;
                    while (c.fetch_row(row))
                        items->push_back({ {"type", 0}, {"id", cell_ll(row[0])}, {"name", row[1].as_string()},
                                           {"parentId", cell_ll(row[2])}, {"size", (long long)row[3].as_ulonglong()} });
                }
                api::ok(resp, {{"items", *items}, {"count", (long long)items->size()}}, "ok");
            });
        });
    });
}

// =============================================================================
// 文件版本历史: 上传新版本 / 列出 / 恢复 / 下载指定版本
// 说明: 文件内容寻址 (blob=hashcode)。上传新版本或恢复前, 先把“当前”版本的
//       (hashcode,size) 归档进 tbl_file_version, 再更新 tbl_file 指向新 blob。
//       每个文件最多保留最近 50 个历史版本。blob 回收站 GC 已将本表纳入存活判定。
// =============================================================================
void CloudiskServer::register_version_module()
{
    // ---- 上传新版本 (multipart, 取表单第一个文件字段) ----
    m_server.POST("/api/file/version", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long fileId = SqlUtil::to_uint(req->has_query("fileId") ? req->query("fileId") : "", 0, 1000000000000LL);
        if (fileId <= 0) { api::fail(resp, 400, 400, "缺少 fileId"); return; }
        Form& form = req->form();
        const string* cptr = nullptr;
        for (auto& kv : form) { if (!kv.second.first.empty()) { cptr = &kv.second.second; break; } }
        if (!cptr) { api::fail(resp, 400, 400, "没有有效文件"); return; }
        if ((long long)cptr->size() > g_max_file_size) { api::fail(resp, 413, 413, "文件超过大小上限"); return; }
        auto content = std::make_shared<string>(*cptr);
        long long uid = user.id; string su = std::to_string(uid), sfid = std::to_string(fileId);

        string q1 = "SELECT hashcode, size FROM tbl_file WHERE deleted=0 AND uid=" + su + " AND id=" + sfid;
        push_mysql(series, q1, [resp, uid, su, sfid, content](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "文件不存在"); return; }
            auto oldHash = std::make_shared<string>(row[0].as_string());
            long long oldSize = (long long)row[1].as_ulonglong();
            push_mysql(series_of(t), usage_quota_sql(uid), [resp, su, sfid, content, oldHash, oldSize](WFMySQLTask* tq) {
                long long used = 0, quota = g_user_quota;
                if (mysql_ok(tq)) { MySQLResultCursor c2{ tq->get_resp() }; std::vector<MySQLCell> r2;
                    if (c2.fetch_row(r2)) { quota = cell_ll(r2[0]); used = (long long)r2[1].as_ulonglong(); } }
                long long newSize = (long long)content->size();
                long long delta = newSize - oldSize;
                if (delta > 0 && used + delta > quota) { api::fail(resp, 413, 413, "存储空间不足"); return; }
                string newHash = CryptoUtil::generate_hashcode(content->c_str(), content->size());
                if (newHash == *oldHash) { api::ok(resp, {{"changed", false}}, "内容未变化, 未创建新版本"); return; }
                bool isNew = write_blob_if_absent(newHash, *content);
                if (isNew) publish_oss_backup(newHash);
                string up =
                    "INSERT INTO tbl_file_version (file_id, uid, hashcode, size) VALUES ("
                    + sfid + ", " + su + ", " + SqlUtil::quote(*oldHash) + ", " + std::to_string(oldSize) + "); "
                    "UPDATE tbl_file SET hashcode=" + SqlUtil::quote(newHash) + ", size=" + std::to_string(newSize)
                    + " WHERE id=" + sfid + " AND uid=" + su + "; "
                    "DELETE FROM tbl_file_version WHERE file_id=" + sfid + " AND uid=" + su
                    + " AND id NOT IN (SELECT id FROM (SELECT id FROM tbl_file_version WHERE file_id=" + sfid
                    + " ORDER BY id DESC LIMIT 50) z); ";
                push_mysql(series_of(tq), up, [resp, newHash, newSize](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "保存版本失败"); return; }
                    g_metrics.uploads++; g_metrics.upload_bytes += newSize;
                    api::ok(resp, {{"changed", true}, {"hash", newHash}, {"size", newSize}}, "已上传新版本");
                });
            });
        });
    });

    // ---- 列出某文件的当前版本 + 历史版本 (最新在前) ----
    m_server.GET("/api/file/versions", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long fileId = SqlUtil::to_uint(req->has_query("fileId") ? req->query("fileId") : "", 0, 1000000000000LL);
        if (fileId <= 0) { api::fail(resp, 400, 400, "缺少 fileId"); return; }
        string su = std::to_string(user.id), sfid = std::to_string(fileId);
        string q1 = "SELECT filename, hashcode, size, DATE_FORMAT(last_update,'%Y-%m-%d %H:%i:%s') "
                    "FROM tbl_file WHERE deleted=0 AND uid=" + su + " AND id=" + sfid;
        push_mysql(series, q1, [resp, su, sfid](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "文件不存在"); return; }
            auto cur = std::make_shared<nlohmann::json>(nlohmann::json{
                {"filename", row[0].as_string()}, {"hash", row[1].as_string()},
                {"size", (long long)row[2].as_ulonglong()}, {"updatedAt", row[3].as_string()} });
            string q2 = "SELECT id, hashcode, size, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') "
                        "FROM tbl_file_version WHERE uid=" + su + " AND file_id=" + sfid + " ORDER BY id DESC";
            push_mysql(series_of(t), q2, [resp, cur](WFMySQLTask* t2) {
                nlohmann::json versions = nlohmann::json::array();
                if (mysql_ok(t2)) {
                    MySQLResultCursor c2{ t2->get_resp() }; std::vector<MySQLCell> r;
                    while (c2.fetch_row(r))
                        versions.push_back({ {"versionId", cell_ll(r[0])}, {"hash", r[1].as_string()},
                                             {"size", (long long)r[2].as_ulonglong()}, {"createdAt", r[3].as_string()} });
                }
                api::ok(resp, {{"current", *cur}, {"versions", versions}, {"count", (long long)versions.size()}}, "ok");
            });
        });
    });

    // ---- 恢复到指定历史版本 (当前版本会被归档, 可再次回滚) ----
    m_server.POST("/api/file/version/restore", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long fileId = in.contains("fileId") && in["fileId"].is_number() ? in["fileId"].get<long long>() : 0;
        long long verId  = in.contains("versionId") && in["versionId"].is_number() ? in["versionId"].get<long long>() : 0;
        if (fileId <= 0 || verId <= 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        long long uid = user.id; string su = std::to_string(uid), sfid = std::to_string(fileId), svid = std::to_string(verId);
        string q1 = "SELECT f.hashcode, f.size, v.hashcode, v.size FROM tbl_file f "
                    "JOIN tbl_file_version v ON v.file_id=f.id AND v.uid=f.uid "
                    "WHERE f.id=" + sfid + " AND f.uid=" + su + " AND f.deleted=0 AND v.id=" + svid;
        push_mysql(series, q1, [resp, uid, su, sfid](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "文件或版本不存在"); return; }
            auto curHash = std::make_shared<string>(row[0].as_string());
            long long curSize = (long long)row[1].as_ulonglong();
            auto tgtHash = std::make_shared<string>(row[2].as_string());
            long long tgtSize = (long long)row[3].as_ulonglong();
            if (!file_exists(g_blob_dir + "/" + *tgtHash)) { api::fail(resp, 410, 410, "该版本内容已被回收, 无法恢复"); return; }
            push_mysql(series_of(t), usage_quota_sql(uid), [resp, su, sfid, curHash, curSize, tgtHash, tgtSize](WFMySQLTask* tq) {
                long long used = 0, quota = g_user_quota;
                if (mysql_ok(tq)) { MySQLResultCursor c2{ tq->get_resp() }; std::vector<MySQLCell> r2;
                    if (c2.fetch_row(r2)) { quota = cell_ll(r2[0]); used = (long long)r2[1].as_ulonglong(); } }
                long long delta = tgtSize - curSize;
                if (delta > 0 && used + delta > quota) { api::fail(resp, 413, 413, "存储空间不足"); return; }
                if (*curHash == *tgtHash) { api::ok(resp, {{"changed", false}}, "已是该版本内容"); return; }
                string up =
                    "INSERT INTO tbl_file_version (file_id, uid, hashcode, size) VALUES ("
                    + sfid + ", " + su + ", " + SqlUtil::quote(*curHash) + ", " + std::to_string(curSize) + "); "
                    "UPDATE tbl_file SET hashcode=" + SqlUtil::quote(*tgtHash) + ", size=" + std::to_string(tgtSize)
                    + " WHERE id=" + sfid + " AND uid=" + su + "; "
                    "DELETE FROM tbl_file_version WHERE file_id=" + sfid + " AND uid=" + su
                    + " AND id NOT IN (SELECT id FROM (SELECT id FROM tbl_file_version WHERE file_id=" + sfid
                    + " ORDER BY id DESC LIMIT 50) z); ";
                push_mysql(series_of(tq), up, [resp, tgtHash, tgtSize](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "恢复失败"); return; }
                    api::ok(resp, {{"changed", true}, {"hash", *tgtHash}, {"size", tgtSize}}, "已恢复到该版本");
                });
            });
        });
    });

    // ---- 下载指定历史版本 ----
    m_server.GET("/api/file/version/download", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long verId = SqlUtil::to_uint(req->has_query("versionId") ? req->query("versionId") : "", 0, 1000000000000LL);
        if (verId <= 0) { api::fail(resp, 400, 400, "缺少 versionId"); return; }
        string sql = "SELECT v.hashcode, f.filename FROM tbl_file_version v "
                     "JOIN tbl_file f ON f.id=v.file_id "
                     "WHERE v.id=" + std::to_string(verId) + " AND v.uid=" + std::to_string(user.id);
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "下载失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "版本不存在"); return; }
            string blob = g_blob_dir + "/" + row[0].as_string();
            string fname = row[1].as_string();
            if (!file_exists(blob)) { api::fail(resp, 404, 404, "版本内容缺失"); return; }
            g_metrics.downloads++; g_metrics.download_bytes += (long long)file_size_of(blob);
            resp->add_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
            resp->File(blob);
        });
    });
}

// =============================================================================
// 存储分析仪表盘: 概览 KPI + 文件类型分布 + 最大文件 + 近 14 天上传趋势
// 只读聚合, 全部按当前用户 (uid) 维度, 单请求内串行组装为一个 JSON。
// =============================================================================
namespace {
    // 扩展名 -> 展示分类
    string stats_category(const string& extRaw)
    {
        string e; for (char c : extRaw) e += (char)std::tolower((unsigned char)c);
        auto in = [&](std::initializer_list<const char*> xs) {
            for (auto x : xs) if (e == x) return true; return false;
        };
        if (in({"jpg","jpeg","png","gif","bmp","webp","svg","ico","heic","tiff"})) return "image";
        if (in({"mp4","mkv","avi","mov","wmv","flv","webm","m4v","mpeg","mpg"}))    return "video";
        if (in({"mp3","wav","flac","aac","ogg","m4a","wma","opus"}))                return "audio";
        if (in({"pdf","doc","docx","xls","xlsx","ppt","pptx","txt","md","rtf","csv","odt"})) return "document";
        if (in({"zip","rar","7z","tar","gz","bz2","xz","tgz"}))                     return "archive";
        if (in({"c","cpp","h","hpp","js","ts","py","java","go","rs","rb","php","sh","html","css","json","xml","yml","yaml","sql"})) return "code";
        return "other";
    }
}

void CloudiskServer::register_stats_module()
{
    m_server.GET("/api/stats", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string U = std::to_string(user.id);
        string g = std::to_string(g_user_quota);
        auto out = std::make_shared<nlohmann::json>(nlohmann::json::object());

        string q1 =
            "SELECT "
            "(SELECT COUNT(*) FROM tbl_file WHERE deleted=0 AND uid=" + U + "), "
            "(SELECT COUNT(*) FROM tbl_folder WHERE deleted=0 AND uid=" + U + "), "
            "(SELECT CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM tbl_file WHERE deleted=0 AND uid=" + U + "), "
            "(SELECT CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM (SELECT hashcode, MIN(size) size FROM tbl_file WHERE deleted=0 AND uid=" + U + " GROUP BY hashcode) x), "
            "(SELECT COUNT(*) FROM tbl_file WHERE deleted=1 AND uid=" + U + "), "
            "(SELECT IF(quota>0,quota," + g + ") FROM tbl_user WHERE id=" + U + "), "
            "(SELECT COUNT(*) FROM tbl_favorite WHERE uid=" + U + "), "
            "(SELECT COUNT(*) FROM tbl_file_version WHERE uid=" + U + "), "
            "(SELECT COUNT(*) FROM tbl_share WHERE uid=" + U + " AND revoked=0)";
        push_mysql(series, q1, [resp, U, out](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "统计失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (c.fetch_row(row)) {
                (*out)["files"]        = cell_ll(row[0]);
                (*out)["folders"]      = cell_ll(row[1]);
                (*out)["logicalSize"]  = (long long)row[2].as_ulonglong();
                (*out)["physicalSize"] = (long long)row[3].as_ulonglong();
                (*out)["trash"]        = cell_ll(row[4]);
                (*out)["quota"]        = cell_ll(row[5]);
                (*out)["favorites"]    = cell_ll(row[6]);
                (*out)["versions"]     = cell_ll(row[7]);
                (*out)["shares"]       = cell_ll(row[8]);
            }
            string q2 = "SELECT LOWER(SUBSTRING_INDEX(filename,'.',-1)) ext, COUNT(*) c, CAST(COALESCE(SUM(size),0) AS UNSIGNED) s "
                        "FROM tbl_file WHERE deleted=0 AND uid=" + U + " GROUP BY ext";
            push_mysql(series_of(t), q2, [resp, U, out](WFMySQLTask* t2) {
                std::map<string, std::pair<long long, long long>> cats;
                if (mysql_ok(t2)) {
                    MySQLResultCursor c2{ t2->get_resp() }; std::vector<MySQLCell> r;
                    while (c2.fetch_row(r)) {
                        string cat = stats_category(r[0].as_string());
                        cats[cat].first  += cell_ll(r[1]);
                        cats[cat].second += (long long)r[2].as_ulonglong();
                    }
                }
                nlohmann::json arr = nlohmann::json::array();
                for (auto& kv : cats)
                    arr.push_back({ {"category", kv.first}, {"count", kv.second.first}, {"size", kv.second.second} });
                (*out)["fileTypes"] = arr;

                string q3 = "SELECT id, filename, size FROM tbl_file WHERE deleted=0 AND uid=" + U
                          + " ORDER BY size DESC, id DESC LIMIT 8";
                push_mysql(series_of(t2), q3, [resp, U, out](WFMySQLTask* t3) {
                    nlohmann::json arr3 = nlohmann::json::array();
                    if (mysql_ok(t3)) {
                        MySQLResultCursor c3{ t3->get_resp() }; std::vector<MySQLCell> r;
                        while (c3.fetch_row(r))
                            arr3.push_back({ {"id", cell_ll(r[0])}, {"filename", r[1].as_string()},
                                             {"size", (long long)r[2].as_ulonglong()} });
                    }
                    (*out)["largest"] = arr3;

                    string q4 = "SELECT DATE_FORMAT(created_at,'%Y-%m-%d') d, COUNT(*) c "
                                "FROM tbl_file WHERE deleted=0 AND uid=" + U
                              + " AND created_at >= DATE_SUB(CURDATE(), INTERVAL 13 DAY) GROUP BY d ORDER BY d";
                    push_mysql(series_of(t3), q4, [resp, out](WFMySQLTask* t4) {
                        nlohmann::json arr4 = nlohmann::json::array();
                        if (mysql_ok(t4)) {
                            MySQLResultCursor c4{ t4->get_resp() }; std::vector<MySQLCell> r;
                            while (c4.fetch_row(r))
                                arr4.push_back({ {"date", r[0].as_string()}, {"count", cell_ll(r[1])} });
                        }
                        (*out)["timeline"] = arr4;
                        api::ok(resp, *out, "ok");
                    });
                });
            });
        });
    });
}

// =============================================================================
// 文件标签体系: 用户自定义彩色标签 + 多对多关联文件 + 按标签筛选。
// 标签关联在文件被彻底删除时随之清理 (见回收站清空/永久删除)。
// =============================================================================
namespace {
    bool valid_hex_color(const string& c) {
        if (c.size() != 4 && c.size() != 7) return false;
        if (c[0] != '#') return false;
        for (size_t i = 1; i < c.size(); ++i) {
            char ch = c[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
        }
        return true;
    }
}

void CloudiskServer::register_tag_module()
{
    // ---- 列出当前用户的标签 (含每个标签的文件数) ----
    m_server.GET("/api/tags", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string U = std::to_string(user.id);
        string sql = "SELECT t.id, t.name, t.color, "
                     "(SELECT COUNT(*) FROM tbl_file_tag ft WHERE ft.tag_id=t.id) c "
                     "FROM tbl_tag t WHERE t.uid=" + U + " ORDER BY t.name ASC";
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> r;
            nlohmann::json arr = nlohmann::json::array();
            while (c.fetch_row(r))
                arr.push_back({ {"id", cell_ll(r[0])}, {"name", r[1].as_string()},
                                {"color", r[2].as_string()}, {"count", cell_ll(r[3])} });
            api::ok(resp, {{"tags", arr}, {"count", (long long)arr.size()}}, "ok");
        });
    });

    // ---- 创建标签 (名称唯一, 已存在则返回其 id) ----
    m_server.POST("/api/tags", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string name = json_str(in, "name");
        string color = json_str(in, "color");
        // 去首尾空白
        auto trim = [](string s){ size_t a=s.find_first_not_of(" \t\r\n"); if(a==string::npos) return string(); size_t b=s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1); };
        name = trim(name);
        if (name.empty() || name.size() > 64) { api::fail(resp, 400, 400, "标签名不合法(1-64字符)"); return; }
        if (color.empty()) color = "#6366f1";
        if (!valid_hex_color(color)) { api::fail(resp, 400, 400, "颜色格式不合法"); return; }
        string U = std::to_string(user.id);
        string sql = "INSERT INTO tbl_tag (uid, name, color) VALUES (" + U + ", " + SqlUtil::quote(name) + ", "
                   + SqlUtil::quote(color) + ") ON DUPLICATE KEY UPDATE color=VALUES(color), id=LAST_INSERT_ID(id)";
        push_mysql(series, sql, [resp, name, color](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "创建失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            long long id = c.get_insert_id();
            api::ok(resp, {{"id", id}, {"name", name}, {"color", color}}, "已创建");
        });
    });

    // ---- 删除标签 (连带解除所有文件关联) ----
    m_server.POST("/api/tags/delete", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long tagId = in.contains("tagId") && in["tagId"].is_number() ? in["tagId"].get<long long>() : 0;
        if (tagId <= 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        string U = std::to_string(user.id), T = std::to_string(tagId);
        string sql = "DELETE FROM tbl_file_tag WHERE uid=" + U + " AND tag_id=" + T + "; "
                     "DELETE FROM tbl_tag WHERE uid=" + U + " AND id=" + T + "; ";
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "删除失败"); return; }
            api::ok(resp, {}, "已删除");
        });
    });

    // ---- 获取单个文件的标签 id 列表 ----
    m_server.GET("/api/file/tags", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long fileId = SqlUtil::to_uint(req->has_query("fileId") ? req->query("fileId") : "", 0, 1000000000000LL);
        if (fileId <= 0) { api::fail(resp, 400, 400, "缺少 fileId"); return; }
        string sql = "SELECT tag_id FROM tbl_file_tag WHERE uid=" + std::to_string(user.id)
                   + " AND file_id=" + std::to_string(fileId);
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> r;
            nlohmann::json arr = nlohmann::json::array();
            while (c.fetch_row(r)) arr.push_back(cell_ll(r[0]));
            api::ok(resp, {{"tagIds", arr}}, "ok");
        });
    });

    // ---- 覆盖设置某文件的标签集合 ----
    m_server.POST("/api/file/tags/set", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long fileId = in.contains("fileId") && in["fileId"].is_number() ? in["fileId"].get<long long>() : 0;
        if (fileId <= 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        std::vector<long long> tagIds;
        if (in.contains("tagIds") && in["tagIds"].is_array())
            for (auto& v : in["tagIds"]) if (v.is_number()) tagIds.push_back(v.get<long long>());
        long long uid = user.id; string U = std::to_string(uid), F = std::to_string(fileId);
        // 校验文件归属 + 未删除
        string chk = "SELECT id FROM tbl_file WHERE deleted=0 AND uid=" + U + " AND id=" + F;
        push_mysql(series, chk, [resp, U, F, uid, tagIds](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> r;
            if (!c.fetch_row(r)) { api::fail(resp, 404, 404, "文件不存在"); return; }
            // 先清空该文件关联, 再插入合法(属于本人)的标签
            string sql = "DELETE FROM tbl_file_tag WHERE uid=" + U + " AND file_id=" + F + "; ";
            if (!tagIds.empty()) {
                string ids;
                for (auto id : tagIds) { if (!ids.empty()) ids += ","; ids += std::to_string(id); }
                sql += "INSERT IGNORE INTO tbl_file_tag (tag_id, file_id, uid) "
                       "SELECT id, " + F + ", " + U + " FROM tbl_tag WHERE uid=" + U
                     + " AND id IN (" + ids + "); ";
            }
            push_mysql(series_of(t), sql, [resp](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "保存失败"); return; }
                api::ok(resp, {}, "已保存");
            });
        });
    });

    // ---- 列出带某标签的文件 ----
    m_server.GET("/api/files/by-tag", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long tagId = SqlUtil::to_uint(req->has_query("tagId") ? req->query("tagId") : "", 0, 1000000000000LL);
        if (tagId <= 0) { api::fail(resp, 400, 400, "缺少 tagId"); return; }
        string U = std::to_string(user.id);
        string sql = "SELECT f.id, f.filename, f.size, f.parent_id "
                     "FROM tbl_file_tag ft JOIN tbl_file f ON f.id=ft.file_id "
                     "WHERE ft.uid=" + U + " AND ft.tag_id=" + std::to_string(tagId)
                   + " AND f.deleted=0 ORDER BY f.filename ASC LIMIT 500";
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> r;
            nlohmann::json arr = nlohmann::json::array();
            while (c.fetch_row(r))
                arr.push_back({ {"id", cell_ll(r[0])}, {"filename", r[1].as_string()},
                                {"size", (long long)r[2].as_ulonglong()}, {"parentId", cell_ll(r[3])} });
            api::ok(resp, {{"items", arr}, {"count", (long long)arr.size()}}, "ok");
        });
    });
}

// =============================================================================
// 活动日志: 面向当前用户展示其自身的操作记录 (读 tbl_audit, 按 uid 过滤)。
// 与管理员的 /api/admin/audit 互补: 这里仅返回本人 uid 的记录, 无需管理员权限。
// =============================================================================
void CloudiskServer::register_activity_module()
{
    m_server.GET("/api/activity", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long limit  = SqlUtil::to_uint(req->has_query("limit")  ? req->query("limit")  : "", 50, 200);
        long long offset = SqlUtil::to_uint(req->has_query("offset") ? req->query("offset") : "", 0, 100000000LL);
        if (limit <= 0) limit = 50;
        string action = req->has_query("action") ? req->query("action") : "";
        string where = "uid=" + std::to_string(user.id);
        // action 仅允许安全字符 (字母/数字/下划线), 防注入
        if (!action.empty()) {
            bool okc = true;
            for (char ch : action) if (!(isalnum((unsigned char)ch) || ch == '_')) { okc = false; break; }
            if (okc) where += " AND action=" + SqlUtil::quote(action);
        }
        string sql = "SELECT id, action, detail, ip, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') "
                     "FROM tbl_audit WHERE " + where + " ORDER BY id DESC LIMIT "
                   + std::to_string(limit) + " OFFSET " + std::to_string(offset);
        push_mysql(series, sql, [resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> r;
            nlohmann::json arr = nlohmann::json::array();
            while (c.fetch_row(r))
                arr.push_back({ {"id", cell_ll(r[0])}, {"action", r[1].as_string()},
                                {"detail", r[2].as_string()}, {"ip", r[3].as_string()},
                                {"createdAt", r[4].is_string() ? r[4].as_string() : string()} });
            api::ok(resp, {{"logs", arr}, {"count", (long long)arr.size()}}, "ok");
        });
    });
}
