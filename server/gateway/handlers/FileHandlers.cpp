// =============================================================================
// File 处理器: 上传 / 列表 / 下载 / 分片 / 离线 / 预览 / 文件夹
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
#include "FormatUtil.h"   // 配额提示的人类可读字节格式化 (与前端 humanSize 一致)
#include <cstdio>          // remove(): 流式合并失败时清理临时文件

// ----- 上传 (多文件) -----------------------------------------------------------
void CloudiskServer::register_fileupload_module()
{
    m_server.POST("/api/file/upload", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        if (req->content_type() != MULTIPART_FORM_DATA) { api::fail(resp, 400, 400, "需要 multipart/form-data"); return; }

        Form& form = req->form();
        if (form.empty()) { api::fail(resp, 400, 400, "没有文件"); return; }

        long long parentId = SqlUtil::to_uint(req->has_query("parentId") ? req->query("parentId") : "", 0, 1000000000000LL);

        // 预校验单文件大小并统计本次总上传量 (此时尚未写盘, 超限直接拒绝)
        long long incoming = 0;
        for (auto& [_, file] : form) {
            if (file.first.empty()) continue;
            if ((long long)file.second.size() > g_max_file_size) {
                api::fail(resp, 413, 413, "文件超过大小上限: " + file.first);
                return;
            }
            incoming += (long long)file.second.size();
        }
        if (incoming == 0) { api::fail(resp, 400, 400, "没有有效文件"); return; }

        int uid = user.id;
        string username = user.username;
        string ip = client_ip(req);
        // 先查当前用量并校验配额, 通过后再写 blob (避免超配额时产生孤儿文件)
        string usageSql = usage_quota_sql(uid);
        push_mysql(series, usageSql, [req, resp, uid, username, ip, incoming, parentId](WFMySQLTask* task) {
            long long used = 0, quota = g_user_quota;
            if (mysql_ok(task)) {
                MySQLResultCursor cur{ task->get_resp() };
                std::vector<MySQLCell> row;
                if (cur.fetch_row(row)) { quota = cell_ll(row[0]); used = (long long)row[1].as_ulonglong(); }
            }
            if (used + incoming > quota) {
                api::fail(resp, 413, 413, "存储空间不足: 剩余 "
                    + fmtutil::human_size(quota > used ? quota - used : 0)
                    + ", 本次需 " + fmtutil::human_size(incoming));
                return;
            }
            // 配额通过: 写 blob + 构建元数据
            Form& form = req->form();
            nlohmann::json uploaded = nlohmann::json::array();
            vector<string> tuples;
            for (auto& [_, file] : form) {
                const string& rawname = file.first;
                const string& content = file.second;
                if (rawname.empty()) continue;
                string filename = PathUtil::base(rawname);
                string hash = CryptoUtil::generate_hashcode(content.c_str(), content.size());
                bool isNew = write_blob_if_absent(hash, content);
                if (isNew) publish_oss_backup(hash);
                tuples.push_back("(" + std::to_string(uid) + ", " + std::to_string(parentId) + ", "
                    + SqlUtil::quote(filename) + ", " + SqlUtil::quote(hash) + ", "
                    + std::to_string(content.size()) + ")");
                uploaded.push_back({ {"filename", filename}, {"hash", hash},
                                     {"size", (long long)content.size()}, {"instant", !isNew} });
            }
            string sql = "REPLACE INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ";
            for (size_t i = 0; i < tuples.size(); ++i) { if (i) sql += ", "; sql += tuples[i]; }
            SeriesWork* s = series_of(task);
            push_mysql(s, sql, [resp, uploaded, username, uid, ip](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "写入文件元数据失败"); return; }
                long long bytes = 0; for (auto& f : uploaded) bytes += (long long)f.value("size", 0);
                g_metrics.uploads += (long long)uploaded.size();
                g_metrics.upload_bytes += bytes;
                LOG_INFO("用户 " << username << " 上传 " << uploaded.size() << " 个文件");
                audit_log(uid, username, AuditAction::FileUpload,
                          std::to_string(uploaded.size()) + " 个文件, " + std::to_string(bytes) + " 字节", ip);
                api::ok(resp, {{"files", uploaded}}, "上传成功");
            });
        });
    });

    // 秒传: 客户端先算 hash, 命中已有 blob 则无需上传文件体
    m_server.POST("/api/file/instant", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }

        string filename = PathUtil::base(json_str(in, "filename"));
        string hash     = json_str(in, "hash");
        long long size  = in.contains("size") && in["size"].is_number() ? in["size"].get<long long>() : 0;
        long long parentId = in.contains("parentId") && in["parentId"].is_number() ? in["parentId"].get<long long>() : 0;
        if (filename.empty() || !SqlUtil::valid_hash(hash) || size < 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        // 早拒: 与 /api/upload/init 保持一致, 避免客户端为超限文件白算哈希再被拒。
        if (size > g_max_file_size) {
            api::fail(resp, 413, 413, "文件超过大小上限: 上限 "
                + fmtutil::human_size(g_max_file_size) + ", 本次 " + fmtutil::human_size(size));
            return;
        }

        if (!blob_store().exists(hash)) {
            api::ok(resp, {{"instant", false}}, "需要完整上传");
            return;
        }
        int uid = user.id;
        string usageSql = usage_quota_sql(uid);
        push_mysql(series, usageSql, [resp, uid, filename, hash, size, parentId](WFMySQLTask* task) {
            long long used = 0, quota = g_user_quota;
            if (mysql_ok(task)) {
                MySQLResultCursor cur{ task->get_resp() };
                std::vector<MySQLCell> row;
                if (cur.fetch_row(row)) { quota = cell_ll(row[0]); used = (long long)row[1].as_ulonglong(); }
            }
            if (used + size > quota) {
                api::fail(resp, 413, 413, "存储空间不足: 剩余 "
                    + fmtutil::human_size(quota > used ? quota - used : 0));
                return;
            }
            string sql = "REPLACE INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                + std::to_string(uid) + ", " + std::to_string(parentId) + ", " + SqlUtil::quote(filename) + ", "
                + SqlUtil::quote(hash) + ", " + std::to_string(size) + ")";
            SeriesWork* s = series_of(task);
            push_mysql(s, sql, [resp, filename, hash, size](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "写入文件元数据失败"); return; }
                api::ok(resp, {{"instant", true}, {"filename", filename}, {"hash", hash}, {"size", size}}, "秒传成功");
            });
        });
    });
}

// ----- 列表 (文件夹感知: 目录 + 文件 + 面包屑) + 文件重命名 -----------------------
void CloudiskServer::register_filelist_module()
{
    m_server.GET("/api/file/list", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;

        long long uid      = user.id;
        long long parentId = SqlUtil::to_uint(req->has_query("parentId") ? req->query("parentId") : "", 0, 1000000000000LL);
        long long limit    = SqlUtil::to_uint(req->has_query("limit")  ? req->query("limit")  : "", 20, 100);
        long long offset   = SqlUtil::to_uint(req->has_query("offset") ? req->query("offset") : "", 0, 1000000000LL);
        string keyword     = req->has_query("keyword") ? req->query("keyword") : "";
        keyword = wfrest::CodeUtil::url_decode(keyword);   // 支持中文关键字 (wfrest 不自动解码)
        string sortReq = req->has_query("sort") ? req->query("sort") : "created_at";
        string sortCol = "created_at";
        if (sortReq == "filename" || sortReq == "size" || sortReq == "created_at" || sortReq == "last_update")
            sortCol = sortReq;
        string order = (req->has_query("order") && req->query("order") == "asc") ? "ASC" : "DESC";

        string fileWhere   = "deleted=0 AND uid=" + std::to_string(uid) + " AND parent_id=" + std::to_string(parentId);
        string folderWhere = "deleted=0 AND uid=" + std::to_string(uid) + " AND parent_id=" + std::to_string(parentId);
        if (!keyword.empty()) {
            string kw = SqlUtil::escape(keyword);
            fileWhere   += " AND filename LIKE '%" + kw + "%'";
            folderWhere += " AND name LIKE '%" + kw + "%'";
        }

        auto out   = std::make_shared<nlohmann::json>(nlohmann::json::object());
        auto total = std::make_shared<long long>(0);
        (*out)["parentId"]   = parentId;
        (*out)["breadcrumb"] = nlohmann::json::array();
        (*out)["folders"]    = nlohmann::json::array();

        // --- 阶段4: 文件列表, 收尾响应 ---
        auto step_list = [resp, out, total, fileWhere, sortCol, order, limit, offset](SeriesWork* s) {
            string sql = "SELECT id, filename, hashcode, size, created_at, last_update, "
                         "CAST((SELECT GROUP_CONCAT(tag_id) FROM tbl_file_tag WHERE file_id=tbl_file.id) AS CHAR) AS tags "
                         "FROM tbl_file WHERE "
                       + fileWhere + " ORDER BY " + sortCol + " " + order
                       + " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
            push_mysql(s, sql, [resp, out, total](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> rec;
                nlohmann::json items = nlohmann::json::array();
                while (c.fetch_row(rec)) {
                    nlohmann::json tagIds = nlohmann::json::array();
                    if (!rec[6].is_null()) {
                        string csv = rec[6].as_string(); string cur;
                        for (char ch : csv) {
                            if (ch == ',') { if (!cur.empty()) { tagIds.push_back(std::stoll(cur)); cur.clear(); } }
                            else cur += ch;
                        }
                        if (!cur.empty()) tagIds.push_back(std::stoll(cur));
                    }
                    items.push_back({
                        {"id",         rec[0].as_ulonglong()},
                        {"filename",   rec[1].as_string()},
                        {"hash",       rec[2].as_string()},
                        {"size",       rec[3].as_ulonglong()},
                        {"createdAt",  rec[4].as_datetime()},
                        {"lastUpdate", rec[5].as_datetime()},
                        {"tagIds",     tagIds},
                        // 权威缩略图可用性: 与 /api/file/thumb 内部使用同一个
                        // is_thumbnailable(), 前端据此决定是否请求缩略图。
                        // 由后端下发而非前端按扩展名自行猜测, 避免"浏览器能预览
                        // 为图片"与"后端能生成缩略图"两个概念混淆 —— 后者不含
                        // webp/svg/ico, 前端猜错就会白发一次 404 并出现回退闪烁。
                        {"hasThumb",   is_thumbnailable(rec[1].as_string())},
                    });
                }
                (*out)["total"] = *total;
                (*out)["items"] = items;
                api::ok(resp, *out);
            });
        };
        // --- 阶段3: 文件计数 ---
        auto step_count = [resp, total, fileWhere, step_list](SeriesWork* s) {
            string sql = "SELECT COUNT(*) FROM tbl_file WHERE " + fileWhere;
            push_mysql(s, sql, [resp, total, step_list](WFMySQLTask* t) {
                if (mysql_ok(t)) {
                    MySQLResultCursor c{ t->get_resp() };
                    std::vector<MySQLCell> row;
                    if (c.fetch_row(row)) *total = row[0].as_ulonglong();
                }
                step_list(series_of(t));
            });
        };
        // --- 阶段2: 子文件夹 ---
        auto step_folders = [resp, out, folderWhere, step_count](SeriesWork* s) {
            string sql = "SELECT id, name, created_at FROM tbl_folder WHERE " + folderWhere + " ORDER BY name ASC";
            push_mysql(s, sql, [resp, out, step_count](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> rec;
                nlohmann::json folders = nlohmann::json::array();
                while (c.fetch_row(rec)) {
                    folders.push_back({
                        {"id",        rec[0].as_ulonglong()},
                        {"name",      rec[1].as_string()},
                        {"createdAt", rec[2].as_datetime()},
                    });
                }
                (*out)["folders"] = folders;
                step_count(series_of(t));
            });
        };
        // --- 阶段1: 面包屑 (根据当前文件夹的 path 解析祖先链) ---
        if (parentId == 0) {
            step_folders(series);
        } else {
            string psql = "SELECT path FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(uid)
                        + " AND id=" + std::to_string(parentId);
            push_mysql(series, psql, [resp, out, uid, step_folders](WFMySQLTask* task) {
                std::vector<long long> ids;
                if (mysql_ok(task)) {
                    MySQLResultCursor c{ task->get_resp() };
                    std::vector<MySQLCell> row;
                    if (c.fetch_row(row)) {
                        string path = row[0].as_string();
                        string cur;
                        for (char ch : path) {
                            if (ch == '/') { if (!cur.empty()) { ids.push_back(std::stoll(cur)); cur.clear(); } }
                            else cur += ch;
                        }
                    }
                }
                if (ids.empty()) { step_folders(series_of(task)); return; }
                string bsql = "SELECT id, name FROM tbl_folder WHERE uid=" + std::to_string(uid)
                            + " AND id IN (" + ids_csv(ids) + ") ORDER BY FIELD(id," + ids_csv(ids) + ")";
                push_mysql(series_of(task), bsql, [resp, out, step_folders](WFMySQLTask* t2) {
                    if (mysql_ok(t2)) {
                        MySQLResultCursor c{ t2->get_resp() };
                        std::vector<MySQLCell> rec;
                        nlohmann::json bc = nlohmann::json::array();
                        while (c.fetch_row(rec))
                            bc.push_back({ {"id", rec[0].as_ulonglong()}, {"name", rec[1].as_string()} });
                        (*out)["breadcrumb"] = bc;
                    }
                    step_folders(series_of(t2));
                });
            });
        }
    });

    // 文件重命名 (按 id)
    m_server.POST("/api/file/rename", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long id  = in.contains("id") && in["id"].is_number() ? in["id"].get<long long>() : 0;
        string newname = PathUtil::base(json_str(in, "newname"));
        if (id <= 0 || newname.empty()) { api::fail(resp, 400, 400, "参数不合法"); return; }

        string uname = user.username, cip = client_ip(req);
        long long ruid = user.id;
        string sql = "UPDATE tbl_file SET filename=" + SqlUtil::quote(newname)
                   + " WHERE deleted=0 AND uid=" + std::to_string(user.id)
                   + " AND id=" + std::to_string(id);
        push_mysql(series, sql, [resp, newname, ruid, uname, cip](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 409, 409, "重命名失败(该目录下可能已存在同名文件)"); return; }
            MySQLResultCursor c{ task->get_resp() };
            if (c.get_affected_rows() == 0) { api::fail(resp, 404, 404, "文件不存在"); return; }
            audit_log(ruid, uname, AuditAction::FileRename, "重命名为 " + newname, cip);
            api::ok(resp, {{"filename", newname}}, "已重命名");
        });
    });
}

// ----- 下载 (按 id) ------------------------------------------------------------
void CloudiskServer::register_filedownload_module()
{
    m_server.GET("/api/file/download", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long id = SqlUtil::to_uint(req->has_query("id") ? req->query("id") : "", 0, 1000000000000LL);
        if (id <= 0) { api::fail(resp, 400, 400, "缺少 id"); return; }

        string sql = "SELECT filename, hashcode FROM tbl_file WHERE deleted=0 AND uid=" + std::to_string(user.id)
                   + " AND id=" + std::to_string(id);
        push_mysql(series, sql, [resp](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "下载失败"); return; }
            MySQLResultCursor cursor{ task->get_resp() };
            std::vector<MySQLCell> row;
            if (!cursor.fetch_row(row)) { api::fail(resp, 404, 404, "文件不存在"); return; }
            string fname = row[0].as_string();
            string blob  = blob_path(row[1].as_string());
            if (!file_exists(blob)) { api::fail(resp, 404, 404, "文件内容缺失"); return; }
            g_metrics.downloads++;
            g_metrics.download_bytes += (long long)file_size_of(blob);
            resp->add_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
            resp->File(blob);
        });
    });
}

// ----- 分片 / 断点续传上传 ------------------------------------------------------
void CloudiskServer::register_chunk_upload_module()
{
    // 初始化上传会话: 秒传命中直接建记录; 否则返回 uploadId 与已上传分片(用于断点续传)
    m_server.POST("/api/upload/init", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string filename = PathUtil::base(json_str(in, "filename"));
        string hash     = json_str(in, "hash");
        long long size  = in.contains("size") && in["size"].is_number() ? in["size"].get<long long>() : 0;
        long long parentId = in.contains("parentId") && in["parentId"].is_number() ? in["parentId"].get<long long>() : 0;
        long long chunkSize = in.contains("chunkSize") && in["chunkSize"].is_number() ? in["chunkSize"].get<long long>() : 0;
        int totalChunks = in.contains("totalChunks") && in["totalChunks"].is_number() ? in["totalChunks"].get<int>() : 0;
        if (filename.empty() || !SqlUtil::valid_hash(hash) || size < 0 || chunkSize <= 0 || totalChunks <= 0 || totalChunks > 100000) {
            api::fail(resp, 400, 400, "参数不合法"); return;
        }
        // 早拒: 单文件超上限必须在 init 就告知, 否则客户端会把整个文件的所有分片
        // 传完 (可能十几 GB 的带宽与磁盘), 直到 complete 才被拒 —— 白等且白占空间。
        if (size > g_max_file_size) {
            api::fail(resp, 413, 413, "文件超过大小上限: 上限 "
                + fmtutil::human_size(g_max_file_size) + ", 本次 " + fmtutil::human_size(size));
            return;
        }
        int uid = user.id;
        string username = user.username;      // 供上传链路日志使用
        bool blobExists = blob_store().exists(hash);

        // 配额校验(以完整大小计)
        string usageSql = usage_quota_sql(uid);
        push_mysql(series, usageSql,
            [resp, uid, username, filename, hash, size, parentId, chunkSize, totalChunks, blobExists](WFMySQLTask* task) {
            long long used = 0, quota = g_user_quota;
            if (mysql_ok(task)) { MySQLResultCursor cur{ task->get_resp() }; std::vector<MySQLCell> row; if (cur.fetch_row(row)) { quota = cell_ll(row[0]); used = (long long)row[1].as_ulonglong(); } }
            if (used + size > quota) {
                api::fail(resp, 413, 413, "存储空间不足: 剩余 " + fmtutil::human_size(quota > used ? quota - used : 0)); return;
            }
            SeriesWork* s = series_of(task);
            if (blobExists) {
                // 秒传
                string ins = "REPLACE INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                    + std::to_string(uid) + ", " + std::to_string(parentId) + ", " + SqlUtil::quote(filename) + ", "
                    + SqlUtil::quote(hash) + ", " + std::to_string(size) + ")";
                push_mysql(s, ins, [resp, filename, hash, size](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "写入元数据失败"); return; }
                    api::ok(resp, {{"instant", true}, {"filename", filename}, {"hash", hash}, {"size", size}}, "秒传成功");
                });
                return;
            }
            // 查找可复用的进行中会话(断点续传), 否则新建。
            //
            // 复用条件**不能**包含 parent_id: 文件的内容身份是 (uid, hashcode),
            // "这些字节是否已经在服务器上" 与 "最终要放进哪个目录" 完全无关。
            // 原先把 parent_id 也作为匹配条件, 于是用户只要换个目标目录 —— 甚至只是
            // 把旧目录删掉再重建一个同名目录 (parent_id 变了) —— 之前已上传的几十 MB
            // 分片就全部作废, 必须从第 0 片重传。实测某用户 143.8 MiB 的视频两次上传
            // 分别停在第 13 / 19 片, 第二次完全没有复用第一次的进度。
            // 反之必须匹配 size / chunk_size / total_chunks: 分片几何不一致时复用旧分片
            // 会拼出损坏的文件。
            string q = "SELECT upload_id, parent_id FROM tbl_upload WHERE status=0 AND uid=" + std::to_string(uid)
                     + " AND hashcode=" + SqlUtil::quote(hash)
                     + " AND size=" + std::to_string(size)
                     + " AND chunk_size=" + std::to_string(chunkSize)
                     + " AND total_chunks=" + std::to_string(totalChunks)
                     + " ORDER BY id DESC LIMIT 1";
            push_mysql(s, q, [resp, uid, username, filename, hash, size, parentId, chunkSize, totalChunks](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "会话查询失败"); return; }
                MySQLResultCursor cur{ t2->get_resp() };
                std::vector<MySQLCell> row;
                string uploadId;
                long long oldParent = -1;
                if (cur.fetch_row(row)) { uploadId = row[0].as_string(); oldParent = cell_ll(row[1]); }
                SeriesWork* s2 = series_of(t2);
                auto reply = [resp, chunkSize, totalChunks](const string& uid_) {
                    string dir = g_upload_dir + "/" + uid_;
                    auto have = list_uploaded_chunks(dir);
                    nlohmann::json arr = nlohmann::json::array();
                    for (int idx : have) arr.push_back(idx);
                    api::ok(resp, {{"instant", false}, {"uploadId", uid_}, {"uploaded", arr},
                                   {"chunkSize", chunkSize}, {"totalChunks", totalChunks},
                                   {"resumed", (long long)have.size()}}, "就绪");
                };
                if (!uploadId.empty()) {
                    mkdir_p(g_upload_dir + "/" + uploadId);
                    // 可观测性: 续传命中必须留痕。排查"上传莫名中断"时, 若没有这条记录
                    // 就无法区分"从未开始""重新开始""成功续传"三种情况。
                    LOG_INFO("用户 " << username << " 续传 " << filename << " ("
                             << (size / 1048576) << " MiB, " << totalChunks << " 片), 会话 " << uploadId
                             << ", 已有 " << list_uploaded_chunks(g_upload_dir + "/" + uploadId).size() << " 片"
                             << (oldParent != parentId ? ", 目标目录由 " + std::to_string(oldParent)
                                                         + " 改为 " + std::to_string(parentId) : ""));
                    // 目标目录变了: 把会话改指到新目录, 这样 complete 落库时用的就是
                    // 用户当前选择的位置, 同时已传分片得以保留。
                    if (oldParent != parentId) {
                        string mv = "UPDATE tbl_upload SET parent_id=" + std::to_string(parentId)
                                  + " WHERE upload_id=" + SqlUtil::quote(uploadId);
                        push_mysql(s2, mv, [reply, uploadId](WFMySQLTask*) { reply(uploadId); });
                        return;
                    }
                    reply(uploadId);
                    return;
                }
                string newId = gen_token(16);
                mkdir_p(g_upload_dir + "/" + newId);
                LOG_INFO("用户 " << username << " 新建上传会话 " << newId << ": " << filename
                         << " (" << (size / 1048576) << " MiB, " << totalChunks << " 片, 目录 "
                         << parentId << ")");
                string ins = "INSERT INTO tbl_upload (upload_id, uid, hashcode, filename, size, parent_id, chunk_size, total_chunks, status) VALUES ("
                    + SqlUtil::quote(newId) + ", " + std::to_string(uid) + ", " + SqlUtil::quote(hash) + ", "
                    + SqlUtil::quote(filename) + ", " + std::to_string(size) + ", " + std::to_string(parentId) + ", "
                    + std::to_string(chunkSize) + ", " + std::to_string(totalChunks) + ", 0)";
                push_mysql(s2, ins, [resp, newId, reply](WFMySQLTask* t3) {
                    if (!mysql_ok(t3)) { api::fail(resp, 500, 500, "创建会话失败"); return; }
                    reply(newId);
                });
            });
        });
    });

    // 上传单个分片: body 为原始二进制, uploadId/index 走 query
    m_server.POST("/api/upload/chunk", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string uploadId = req->has_query("uploadId") ? req->query("uploadId") : "";
        int index = (int)SqlUtil::to_uint(req->has_query("index") ? req->query("index") : "", -1, 1000000);
        if (uploadId.empty() || !SqlUtil::valid_token(uploadId) || index < 0) { api::fail(resp, 400, 400, "参数不合法"); return; }
        const string& body = req->body();
        if (body.empty()) { api::fail(resp, 400, 400, "空分片"); return; }
        int uid = user.id;
        // 校验会话归属
        string q = "SELECT total_chunks FROM tbl_upload WHERE status=0 AND uid=" + std::to_string(uid)
                 + " AND upload_id=" + SqlUtil::quote(uploadId);
        // body 通过指针捕获(请求生命周期内有效)
        push_mysql(series, q, [resp, uploadId, index, &body](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "会话校验失败"); return; }
            MySQLResultCursor cur{ task->get_resp() };
            std::vector<MySQLCell> row;
            if (!cur.fetch_row(row)) { api::fail(resp, 404, 404, "会话不存在或已完成"); return; }
            long long total = cell_ll(row[0]);
            if (index >= total) { api::fail(resp, 400, 400, "分片序号越界"); return; }
            string part = g_upload_dir + "/" + uploadId + "/" + std::to_string(index) + ".part";
            if (!write_file_all(part, body.data(), body.size())) { api::fail(resp, 500, 500, "分片写入失败"); return; }
            api::ok(resp, {{"index", index}, {"received", (long long)body.size()}}, "ok");
        });
    });

    // 合并分片, 校验完整 hash, 落 blob 并建记录
    m_server.POST("/api/upload/complete", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string uploadId = json_str(in, "uploadId");
        if (uploadId.empty() || !SqlUtil::valid_token(uploadId)) { api::fail(resp, 400, 400, "参数不合法"); return; }
        int uid = user.id;
        string username = user.username;      // 供上传链路日志使用
        string q = "SELECT hashcode, filename, size, parent_id, total_chunks FROM tbl_upload WHERE status=0 AND uid="
                 + std::to_string(uid) + " AND upload_id=" + SqlUtil::quote(uploadId);
        push_mysql(series, q, [resp, uid, username, uploadId](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "会话查询失败"); return; }
            MySQLResultCursor cur{ task->get_resp() };
            std::vector<MySQLCell> row;
            if (!cur.fetch_row(row)) { api::fail(resp, 404, 404, "会话不存在或已完成"); return; }
            string hash = row[0].as_string();
            string filename = row[1].as_string();
            long long size = cell_ll(row[2]);
            long long parentId = cell_ll(row[3]);
            int total = (int)cell_ll(row[4]);
            string dir = g_upload_dir + "/" + uploadId;

            // 早拒: 声明大小超过单文件上限就不必再消耗任何磁盘 I/O
            if (size > g_max_file_size) {
                api::fail(resp, 413, 413, "文件超过大小上限: 上限 "
                    + fmtutil::human_size(g_max_file_size) + ", 本次 " + fmtutil::human_size(size));
                return;
            }

            SeriesWork* s = series_of(task);
            string usageSql = usage_quota_sql(uid);
            push_mysql(s, usageSql, [resp, uid, username, filename, hash, size, parentId, uploadId, dir, total](WFMySQLTask* t2) {
                long long used = 0, quota = g_user_quota;
                if (mysql_ok(t2)) { MySQLResultCursor c{ t2->get_resp() }; std::vector<MySQLCell> r; if (c.fetch_row(r)) { quota = cell_ll(r[0]); used = (long long)r[1].as_ulonglong(); } }
                // 合并前先按声明大小判一次配额: 避免为注定失败的上传白白读写几十 GB
                if (used + size > quota) {
                    LOG_WARN("用户 " << username << " 合并被拒(配额): " << filename
                             << " 需 " << (size / 1048576) << " MiB, 剩余 "
                             << ((quota > used ? quota - used : 0) / 1048576) << " MiB");
                    api::fail(resp, 413, 413, "存储空间不足: 剩余 "
                        + fmtutil::human_size(quota > used ? quota - used : 0)
                        + ", 本次需 " + fmtutil::human_size(size));
                    return;
                }

                // 流式合并: 逐片 read -> 追加 write -> 增量算 SHA-256。
                // 峰值内存约 4 MiB, 与文件总大小无关。原实现把所有分片拼进一个
                // std::string 再整体哈希, 峰值内存 = 文件大小; 单文件上限提到
                // 10 GiB 后, 一次合并就要 10 GiB 内存, 会 OOM 拖垮整个服务进程。
                string tmp = blob_tmp_path(uploadId);
                string realHash, merr;
                long long realSize = 0;
                const long long t0 = (long long)time(nullptr);
                if (!merge_chunks_streaming(dir, total, tmp, realHash, realSize, merr)) {
                    // 可观测性: 合并失败必须留痕 (缺哪一片、什么 errno), 否则线上
                    // 只能看到客户端一个笼统的"上传失败"。
                    LOG_ERROR("用户 " << username << " 合并失败: " << filename
                              << " 会话 " << uploadId << " — " << merr);
                    api::fail(resp, 400, 400, merr.empty() ? "分片合并失败" : merr);
                    return;
                }
                if (realHash != hash) {
                    LOG_ERROR("用户 " << username << " 校验失败: " << filename
                              << " 声明 " << hash.substr(0, 12) << "… 实际 " << realHash.substr(0, 12) << "…");
                    remove(tmp.c_str());
                    api::fail(resp, 400, 400, "文件校验失败(hash 不一致)");
                    return;
                }
                // 合并后按真实字节数复核一次配额 (客户端声明的 size 不可信)
                if (used + realSize > quota) {
                    remove(tmp.c_str());
                    api::fail(resp, 413, 413, "存储空间不足: 剩余 "
                        + fmtutil::human_size(quota > used ? quota - used : 0)
                        + ", 本次需 " + fmtutil::human_size(realSize));
                    return;
                }
                // 原子纳入 blob 存储 (rename); 同哈希已存在则丢弃临时文件 = 去重命中
                string aerr;
                bool isNew = adopt_blob_file(hash, tmp, aerr);
                if (!aerr.empty()) {
                    LOG_ERROR("用户 " << username << " 存储写入失败: " << filename << " — " << aerr);
                    api::fail(resp, 500, 500, "存储写入失败: " + aerr); return;
                }
                if (isNew) publish_oss_backup(hash);
                SeriesWork* s2 = series_of(t2);
                string ins = "REPLACE INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                    + std::to_string(uid) + ", " + std::to_string(parentId) + ", " + SqlUtil::quote(filename) + ", "
                    + SqlUtil::quote(hash) + ", " + std::to_string(realSize) + ")";
                push_mysql(s2, ins, [resp, username, uploadId, dir, filename, hash, realSize, isNew, t0](WFMySQLTask* t3) {
                    if (!mysql_ok(t3)) { api::fail(resp, 500, 500, "写入元数据失败"); return; }
                    SeriesWork* s3 = series_of(t3);
                    string upd = "UPDATE tbl_upload SET status=1 WHERE upload_id=" + SqlUtil::quote(uploadId);
                    push_mysql(s3, upd, [resp, username, dir, filename, hash, realSize, isNew, t0](WFMySQLTask*) {
                        remove_upload_dir(dir);
                        // 成功也要留痕: 有了 大小 + 合并耗时 + 是否去重命中, 才能事后
                        // 判断"慢"是慢在网络、磁盘还是哈希, 而不是只能靠猜。
                        LOG_INFO("用户 " << username << " 上传完成: " << filename << " ("
                                 << (realSize / 1048576) << " MiB), 合并耗时 "
                                 << ((long long)time(nullptr) - t0) << "s, "
                                 << (isNew ? "新写入 blob" : "去重命中(秒传)"));
                        api::ok(resp, {{"filename", filename}, {"hash", hash}, {"size", realSize}, {"instant", !isNew}}, "上传成功");
                    });
                });
            });
        });
    });
}

// ----- URL 离线下载 ------------------------------------------------------------
void CloudiskServer::register_offline_module()
{
    // 创建离线下载任务, 立即返回, 后台异步抓取
    m_server.POST("/api/offline/create", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string url = json_str(in, "url");
        long long parentId = in.contains("parentId") && in["parentId"].is_number() ? in["parentId"].get<long long>() : 0;
        string rawname = json_str(in, "filename");
        // 仅允许 http/https
        if (url.size() < 8 || (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) || url.size() > 2048) {
            api::fail(resp, 400, 400, "仅支持 http/https 链接"); return;
        }
        string filename;
        if (!rawname.empty()) {
            filename = PathUtil::base(rawname);
        } else {
            // 从 URL 推断文件名
            string u = url; auto qm = u.find('?'); if (qm != string::npos) u = u.substr(0, qm);
            auto slash = u.rfind('/'); filename = (slash != string::npos && slash + 1 < u.size()) ? u.substr(slash + 1) : "";
        }
        if (filename.empty() || filename == "/" || filename.size() > 255) filename = "download";
        int uid = user.id;
        string ins = "INSERT INTO tbl_offline (uid, url, filename, parent_id, status) VALUES ("
            + std::to_string(uid) + ", " + SqlUtil::quote(url) + ", " + SqlUtil::quote(filename) + ", "
            + std::to_string(parentId) + ", 0)";
        push_mysql(series, ins, [resp, uid, url, filename, parentId](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "创建任务失败"); return; }
            MySQLResultCursor c{ task->get_resp() };
            long long offlineId = c.get_insert_id();

            // 后台抓取(与响应 series 解耦)
            WFHttpTask* http = WFTaskFactory::create_http_task(url, 4, 2,
                [offlineId, uid, filename, parentId](WFHttpTask* t) {
                    auto finishError = [offlineId](const string& msg) {
                        string sql = "UPDATE tbl_offline SET status=2, message=" + SqlUtil::quote(msg)
                                   + " WHERE id=" + std::to_string(offlineId);
                        WFMySQLTask* mt = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
                        mt->get_req()->set_query(sql); mt->start();
                    };
                    if (t->get_state() != WFT_STATE_SUCCESS) { finishError("网络错误"); return; }
                    protocol::HttpResponse* r = t->get_resp();
                    const char* code = r->get_status_code();
                    if (!code || string(code).substr(0,1) != "2") { finishError(string("HTTP ") + (code ? code : "?")); return; }
                    const void* body = nullptr; size_t len = 0;
                    if (!r->get_parsed_body(&body, &len) || len == 0) { finishError("空响应"); return; }
                    if ((long long)len > g_max_file_size) { finishError("文件超过大小上限"); return; }
                    string content((const char*)body, len);
                    string hash = CryptoUtil::generate_hashcode(content.c_str(), content.size());
                    // 配额校验后落库(串到一个新 series)
                    string usageSql = usage_quota_sql(uid);
                    auto cptr = std::make_shared<string>(std::move(content));
                    WFMySQLTask* mt = WFTaskFactory::create_mysql_task(g_mysql_url, 2,
                        [offlineId, uid, filename, parentId, hash, cptr](WFMySQLTask* mq) {
                            long long used = 0, quota = g_user_quota;
                            if (mq->get_state() == WFT_STATE_SUCCESS) {
                                MySQLResultCursor cur{ mq->get_resp() }; std::vector<MySQLCell> row;
                                if (cur.fetch_row(row)) { quota = cell_ll(row[0]); used = (long long)row[1].as_ulonglong(); }
                            }
                            long long sz = (long long)cptr->size();
                            if (used + sz > quota) {
                                // human_size 只产出 "1.5 MB" 这类字符, 不含单引号,
                                // 直接拼进 SQL 字面量是安全的。
                                string msg = "存储空间不足: 剩余 "
                                    + fmtutil::human_size(quota > used ? quota - used : 0)
                                    + ", 本次需 " + fmtutil::human_size(sz);
                                string sql = "UPDATE tbl_offline SET status=2, message=" + SqlUtil::quote(msg)
                                           + " WHERE id=" + std::to_string(offlineId);
                                WFMySQLTask* et = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
                                et->get_req()->set_query(sql); et->start();
                                return;
                            }
                            bool isNew = write_blob_if_absent(hash, *cptr);
                            if (isNew) publish_oss_backup(hash);
                            string fsql = "INSERT INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                                + std::to_string(uid) + ", " + std::to_string(parentId) + ", " + SqlUtil::quote(filename) + ", "
                                + SqlUtil::quote(hash) + ", " + std::to_string(sz) + ")";
                            WFMySQLTask* ft = WFTaskFactory::create_mysql_task(g_mysql_url, 2,
                                [offlineId, sz](WFMySQLTask* fq) {
                                    long long fileId = 0;
                                    if (fq->get_state() == WFT_STATE_SUCCESS) { MySQLResultCursor cc{ fq->get_resp() }; fileId = cc.get_insert_id(); }
                                    string usql = "UPDATE tbl_offline SET status=1, size=" + std::to_string(sz)
                                                + ", file_id=" + std::to_string(fileId) + ", message='' WHERE id=" + std::to_string(offlineId);
                                    WFMySQLTask* ut = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
                                    ut->get_req()->set_query(usql); ut->start();
                                });
                            ft->get_req()->set_query(fsql); ft->start();
                        });
                    mt->get_req()->set_query(usageSql); mt->start();
                });
            http->start();

            api::ok(resp, {{"id", offlineId}, {"filename", filename}, {"status", 0}}, "已加入离线下载队列");
        });
    });

    // 离线任务列表
    m_server.GET("/api/offline/list", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string sql = "SELECT id, url, filename, status, size, message, file_id, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') "
                     "FROM tbl_offline WHERE uid=" + std::to_string(user.id) + " ORDER BY id DESC LIMIT 100";
        push_mysql(series, sql, [resp](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor cur{ task->get_resp() };
            std::vector<MySQLCell> row;
            nlohmann::json arr = nlohmann::json::array();
            while (cur.fetch_row(row)) {
                arr.push_back({
                    {"id", cell_ll(row[0])}, {"url", row[1].as_string()}, {"filename", row[2].as_string()},
                    {"status", (int)cell_ll(row[3])}, {"size", cell_ll(row[4])}, {"message", row[5].as_string()},
                    {"fileId", cell_ll(row[6])}, {"createdAt", row[7].is_string() ? row[7].as_string() : string()}
                });
            }
            api::ok(resp, {{"tasks", arr}}, "ok");
        });
    });
}

// ----- 在线预览: 内联内容(支持 Range) / 缩略图 ---------------------------------
void CloudiskServer::register_preview_module()
{
    m_server.GET("/api/file/content", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long id = SqlUtil::to_uint(req->has_query("id") ? req->query("id") : "", 0, 1000000000000LL);
        if (id <= 0) { api::fail(resp, 400, 400, "缺少 id"); return; }

        bool hasRange = req->has_header("Range");
        string rangeHdr = hasRange ? req->header("Range") : "";

        string sql = "SELECT filename, hashcode FROM tbl_file WHERE deleted=0 AND uid=" + std::to_string(user.id)
                   + " AND id=" + std::to_string(id);
        push_mysql(series, sql, [resp, hasRange, rangeHdr](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "读取失败"); return; }
            MySQLResultCursor cursor{ task->get_resp() };
            std::vector<MySQLCell> row;
            if (!cursor.fetch_row(row)) { api::fail(resp, 404, 404, "文件不存在"); return; }
            string fname = row[0].as_string();
            string blob  = blob_path(row[1].as_string());
            long long total = file_size_of(blob);
            if (total < 0) { api::fail(resp, 404, 404, "文件内容缺失"); return; }
            string mime = mime_of(fname);

            resp->add_header("Accept-Ranges", "bytes");
            resp->add_header("Content-Disposition", "inline; filename=\"" + fname + "\"");

            // 解析 Range: bytes=start-end / bytes=start- / bytes=-suffix
            long long start = 0, end = total - 1;
            bool partial = false;
            if (hasRange && total > 0) {
                auto eq = rangeHdr.find('=');
                string spec = (eq == string::npos) ? "" : rangeHdr.substr(eq + 1);
                auto dash = spec.find('-');
                if (dash != string::npos) {
                    string s = spec.substr(0, dash), e = spec.substr(dash + 1);
                    try {
                        if (s.empty() && !e.empty()) { long long n = std::stoll(e); start = n >= total ? 0 : total - n; end = total - 1; }
                        else { start = s.empty() ? 0 : std::stoll(s); end = e.empty() ? total - 1 : std::stoll(e); }
                        if (end >= total) end = total - 1;
                        if (start < 0) start = 0;
                        if (start <= end && start < total) partial = true;
                    } catch (...) { partial = false; }
                }
            }

            if (partial) {
                resp->File(blob, (size_t)start, (size_t)(end + 1));   // wfrest end 为开区间
                resp->set_status(206);
                resp->headers["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total);
                resp->headers["Content-Type"]  = mime;                 // 覆盖无扩展名 blob 的默认类型
            } else {
                resp->File(blob);
                resp->headers.erase("Content-Range");                  // 200 不应携带 Content-Range
                resp->headers["Content-Type"] = mime;
            }
        });
    });

    // 图片缩略图 (JPEG, 长边默认 256px), 首次生成后落盘缓存
    m_server.GET("/api/file/thumb", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        long long id = SqlUtil::to_uint(req->has_query("id") ? req->query("id") : "", 0, 1000000000000LL);
        if (id <= 0) { api::fail(resp, 400, 400, "缺少 id"); return; }
        int size = (int)SqlUtil::to_uint(req->has_query("size") ? req->query("size") : "", 256, 512);
        if (size < 32) size = 32; if (size > 512) size = 512;

        string sql = "SELECT filename, hashcode FROM tbl_file WHERE deleted=0 AND uid=" + std::to_string(user.id)
                   + " AND id=" + std::to_string(id);
        push_mysql(series, sql, [resp, size](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 500, 500, "读取失败"); return; }
            MySQLResultCursor cursor{ task->get_resp() };
            std::vector<MySQLCell> row;
            if (!cursor.fetch_row(row)) { api::fail(resp, 404, 404, "文件不存在"); return; }
            string fname = row[0].as_string();
            string hash  = row[1].as_string();
            if (!is_thumbnailable(fname)) { api::fail(resp, 404, 404, "该类型无缩略图"); return; }
            string blob  = blob_path(hash);
            if (!file_exists(blob)) { api::fail(resp, 404, 404, "文件内容缺失"); return; }
            string thumbPath = g_thumb_dir + "/" + hash + "_" + std::to_string(size) + ".jpg";
            if (!file_exists(thumbPath)) {
                if (!thumb::make_thumbnail(blob, thumbPath, size)) { api::fail(resp, 404, 404, "缩略图生成失败"); return; }
            }
            resp->add_header("Cache-Control", "public, max-age=604800");
            resp->add_header("Content-Type", "image/jpeg");
            resp->File(thumbPath);
            resp->headers["Content-Type"] = "image/jpeg";
        });
    });
}

// ----- 文件夹: 新建 / 重命名 / 移动 --------------------------------------------
void CloudiskServer::register_folder_module()
{
    // 新建文件夹
    m_server.POST("/api/folder/create", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string name = PathUtil::base(json_str(in, "name"));
        long long parentId = in.contains("parentId") && in["parentId"].is_number() ? in["parentId"].get<long long>() : 0;
        if (name.empty() || name.size() > 255) { api::fail(resp, 400, 400, "文件夹名不合法"); return; }
        long long uid = user.id;
        string cuname = user.username, cip = client_ip(req);

        // 插入后再用自增 id 回填物化路径 path = 父路径 + id + "/"
        auto do_insert = [resp, uid, name, parentId, cuname, cip](SeriesWork* s, const string& parentPath) {
            string ins = "INSERT INTO tbl_folder (uid, name, parent_id, path) VALUES ("
                + std::to_string(uid) + ", " + SqlUtil::quote(name) + ", " + std::to_string(parentId) + ", '/')";
            push_mysql(s, ins, [resp, name, parentId, parentPath, uid, cuname, cip](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 409, 409, "该目录下已存在同名文件夹"); return; }
                MySQLResultCursor c{ t->get_resp() };
                long long id = c.get_insert_id();
                string np = parentPath + std::to_string(id) + "/";
                string up = "UPDATE tbl_folder SET path=" + SqlUtil::quote(np) + " WHERE id=" + std::to_string(id);
                push_mysql(series_of(t), up, [resp, id, name, parentId, uid, cuname, cip](WFMySQLTask*) {
                    audit_log(uid, cuname, AuditAction::FolderCreate, "新建文件夹 " + name, cip);
                    api::ok(resp, {{"id", id}, {"name", name}, {"parentId", parentId}}, "已创建");
                });
            });
        };

        if (parentId == 0) {
            do_insert(series, "/");
        } else {
            string q = "SELECT path FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(uid)
                     + " AND id=" + std::to_string(parentId);
            push_mysql(series, q, [resp, do_insert](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "创建失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> row;
                if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "目标文件夹不存在"); return; }
                do_insert(series_of(t), row[0].as_string());
            });
        }
    });

    // 递归确保多级路径存在 (用于文件夹上传), 返回叶子文件夹 id
    m_server.POST("/api/folder/ensure", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long parentId = in.contains("parentId") && in["parentId"].is_number() ? in["parentId"].get<long long>() : 0;
        string path = json_str(in, "path");
        // 拆分并清洗每段
        vector<string> segs;
        {
            string cur;
            auto flush = [&]() { if (!cur.empty()) { string b = PathUtil::base(cur); if (!b.empty() && b != "." && b != ".." && b.size() <= 255) segs.push_back(b); cur.clear(); } };
            for (char ch : path) { if (ch == '/' || ch == '\\') flush(); else cur += ch; }
            flush();
        }
        if (segs.size() > 64) { api::fail(resp, 400, 400, "路径层级过深"); return; }

        struct EnsureState { long long uid; long long curParent; string curPath; std::vector<string> segs; size_t idx; HttpResp* resp; };
        auto st = std::make_shared<EnsureState>();
        st->uid = user.id; st->curParent = parentId; st->curPath = "/"; st->segs = segs; st->idx = 0; st->resp = resp;

        auto step = std::make_shared<std::function<void(SeriesWork*)>>();
        *step = [st, step](SeriesWork* s) {
            if (st->idx >= st->segs.size()) {
                api::ok(st->resp, {{"id", st->curParent}, {"path", st->curPath}}, "ok");
                return;
            }
            string seg = st->segs[st->idx];
            string q = "SELECT id, path FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(st->uid)
                     + " AND parent_id=" + std::to_string(st->curParent) + " AND name=" + SqlUtil::quote(seg);
            push_mysql(s, q, [st, step, seg](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(st->resp, 500, 500, "路径处理失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> row;
                if (c.fetch_row(row)) {   // 已存在
                    st->curParent = cell_ll(row[0]);
                    st->curPath = row[1].as_string();
                    st->idx++;
                    (*step)(series_of(t));
                    return;
                }
                // 新建
                string ins = "INSERT INTO tbl_folder (uid, name, parent_id, path) VALUES ("
                    + std::to_string(st->uid) + ", " + SqlUtil::quote(seg) + ", " + std::to_string(st->curParent) + ", '/')";
                push_mysql(series_of(t), ins, [st, step](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(st->resp, 500, 500, "创建目录失败"); return; }
                    long long id = MySQLResultCursor{ t2->get_resp() }.get_insert_id();
                    string np = st->curPath + std::to_string(id) + "/";
                    string up = "UPDATE tbl_folder SET path=" + SqlUtil::quote(np) + " WHERE id=" + std::to_string(id);
                    push_mysql(series_of(t2), up, [st, step, id, np](WFMySQLTask* t3) {
                        st->curParent = id; st->curPath = np; st->idx++;
                        (*step)(series_of(t3));
                    });
                });
            });
        };

        if (st->curParent == 0) { st->curPath = "/"; (*step)(series); }
        else {
            string q = "SELECT path FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(st->uid)
                     + " AND id=" + std::to_string(st->curParent);
            push_mysql(series, q, [st, step](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(st->resp, 500, 500, "路径处理失败"); return; }
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> row;
                if (!c.fetch_row(row)) { api::fail(st->resp, 404, 404, "父目录不存在"); return; }
                st->curPath = row[0].as_string();
                (*step)(series_of(t));
            });
        }
    });

    // 文件夹重命名 (路径基于 id, 不受影响)
    m_server.POST("/api/folder/rename", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        long long id = in.contains("id") && in["id"].is_number() ? in["id"].get<long long>() : 0;
        string newname = PathUtil::base(json_str(in, "newname"));
        if (id <= 0 || newname.empty() || newname.size() > 255) { api::fail(resp, 400, 400, "参数不合法"); return; }

        string sql = "UPDATE tbl_folder SET name=" + SqlUtil::quote(newname)
                   + " WHERE deleted=0 AND uid=" + std::to_string(user.id) + " AND id=" + std::to_string(id);
        push_mysql(series, sql, [resp, newname](WFMySQLTask* task) {
            if (!mysql_ok(task)) { api::fail(resp, 409, 409, "重命名失败(该目录下可能已存在同名文件夹)"); return; }
            MySQLResultCursor c{ task->get_resp() };
            if (c.get_affected_rows() == 0) { api::fail(resp, 404, 404, "文件夹不存在"); return; }
            api::ok(resp, {{"name", newname}}, "已重命名");
        });
    });

    // 移动: 把选中的文件 + 文件夹(含子树)移动到 targetId (0 = 根)
    m_server.POST("/api/fs/move", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        std::vector<long long> fileIds, folderIds;
        if (!ids_from_json(in, "fileIds", fileIds) || !ids_from_json(in, "folderIds", folderIds)) {
            api::fail(resp, 400, 400, "参数不合法"); return;
        }
        long long targetId = in.contains("targetId") && in["targetId"].is_number() ? in["targetId"].get<long long>() : 0;
        long long uid = user.id;
        if (fileIds.empty() && folderIds.empty()) { api::ok(resp, {}, "无变化"); return; }
        for (auto fid : folderIds) if (fid == targetId) { api::fail(resp, 400, 400, "不能移动到自身"); return; }

        auto apply = [resp, uid, fileIds, folderIds, targetId](SeriesWork* s, std::shared_ptr<std::map<long long, string>> pathOf, const string& targetPath) {
            string q;
            for (auto fid : folderIds) {
                auto it = pathOf->find(fid);
                if (it == pathOf->end()) continue;                 // 已删/不存在, 跳过
                const string& oldBase = it->second;
                if (targetPath.rfind(oldBase, 0) == 0) { api::fail(resp, 400, 400, "不能移动到自身的子目录"); return; }
                string newBase = targetPath + std::to_string(fid) + "/";
                q += "UPDATE tbl_folder SET parent_id=" + std::to_string(targetId)
                   + " WHERE deleted=0 AND uid=" + std::to_string(uid) + " AND id=" + std::to_string(fid) + "; ";
                q += "UPDATE tbl_folder SET path=CONCAT(" + SqlUtil::quote(newBase)
                   + ", SUBSTRING(path," + std::to_string((long long)oldBase.size() + 1) + ")) "
                   + "WHERE deleted=0 AND uid=" + std::to_string(uid) + " AND path LIKE " + SqlUtil::quote(oldBase + "%") + "; ";
            }
            if (!fileIds.empty())
                q += "UPDATE tbl_file SET parent_id=" + std::to_string(targetId)
                   + " WHERE deleted=0 AND uid=" + std::to_string(uid) + " AND id IN (" + ids_csv(fileIds) + "); ";
            if (q.empty()) { api::ok(resp, {}, "无变化"); return; }
            push_mysql(s, q, [resp](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 409, 409, "移动失败(目标目录可能存在同名项)"); return; }
                api::ok(resp, {}, "已移动");
            });
        };

        std::vector<long long> needPaths = folderIds;
        if (targetId != 0) needPaths.push_back(targetId);
        if (needPaths.empty()) {
            apply(series, std::make_shared<std::map<long long, string>>(), "/");
            return;
        }
        string q = "SELECT id, path FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(uid)
                 + " AND id IN (" + ids_csv(needPaths) + ")";
        push_mysql(series, q, [resp, apply, targetId](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "移动失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            auto pathOf = std::make_shared<std::map<long long, string>>();
            while (c.fetch_row(row)) (*pathOf)[row[0].as_ulonglong()] = row[1].as_string();
            string targetPath = "/";
            if (targetId != 0) {
                auto it = pathOf->find(targetId);
                if (it == pathOf->end()) { api::fail(resp, 404, 404, "目标文件夹不存在"); return; }
                targetPath = it->second;
            }
            apply(series_of(t), pathOf, targetPath);
        });
    });
}
