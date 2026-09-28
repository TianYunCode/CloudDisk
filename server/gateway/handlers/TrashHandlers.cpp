// =============================================================================
// Trash 处理器: 回收站 (软删/列表/恢复/彻底删/清空)
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

// ----- 回收站: 软删除 / 列表 / 恢复 / 彻底删除 / 清空 ----------------------------
void CloudiskServer::register_trash_module()
{
    // 软删除 (移入回收站): 文件夹递归软删其子树
    m_server.POST("/api/fs/delete", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        std::vector<long long> fileIds, folderIds;
        if (!ids_from_json(in, "fileIds", fileIds) || !ids_from_json(in, "folderIds", folderIds)) {
            api::fail(resp, 400, 400, "参数不合法"); return;
        }
        long long uid = user.id;
        if (fileIds.empty() && folderIds.empty()) { api::ok(resp, {}, "无变化"); return; }
        string su = std::to_string(uid);
        string username = user.username, ip = client_ip(req);
        long long nItems = (long long)fileIds.size() + (long long)folderIds.size();

        auto apply = [resp, uid, fileIds, su, username, ip, nItems](SeriesWork* s, std::shared_ptr<std::map<long long, string>> pathOf) {
            string q;
            if (!fileIds.empty())
                q += "UPDATE tbl_file SET deleted=1, deleted_at=NOW(), trashed_root=1, dkey=id "
                     "WHERE deleted=0 AND uid=" + su + " AND id IN (" + ids_csv(fileIds) + "); ";
            for (auto& kv : *pathOf) {
                long long fid = kv.first;
                const string& P = kv.second;
                string like = SqlUtil::quote(P + "%");
                q += "UPDATE tbl_folder SET deleted=1, deleted_at=NOW(), trashed_root=IF(id=" + std::to_string(fid) + ",1,0), dkey=id "
                     "WHERE deleted=0 AND uid=" + su + " AND path LIKE " + like + "; ";
                q += "UPDATE tbl_file SET deleted=1, deleted_at=NOW(), trashed_root=0, dkey=id "
                     "WHERE deleted=0 AND uid=" + su + " AND parent_id IN (SELECT id FROM tbl_folder WHERE uid=" + su + " AND path LIKE " + like + "); ";
            }
            if (q.empty()) { api::ok(resp, {}, "无变化"); return; }
            push_mysql(s, q, [resp, uid, username, ip, nItems](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "删除失败"); return; }
                audit_log(uid, username, "file_delete", std::to_string(nItems) + " 个项目移入回收站", ip);
                api::ok(resp, {}, "已移入回收站");
            });
        };

        if (folderIds.empty()) {
            apply(series, std::make_shared<std::map<long long, string>>());
            return;
        }
        string q = "SELECT id, path FROM tbl_folder WHERE deleted=0 AND uid=" + su
                 + " AND id IN (" + ids_csv(folderIds) + ")";
        push_mysql(series, q, [resp, apply](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "删除失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            auto pathOf = std::make_shared<std::map<long long, string>>();
            while (c.fetch_row(row)) (*pathOf)[row[0].as_ulonglong()] = row[1].as_string();
            apply(series_of(t), pathOf);
        });
    });

    // 回收站列表 (仅显示用户直接删除的顶层项)
    m_server.GET("/api/trash/list", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string su = std::to_string(user.id);
        auto out = std::make_shared<nlohmann::json>(nlohmann::json::object());
        (*out)["folders"] = nlohmann::json::array();
        (*out)["files"]   = nlohmann::json::array();

        string fsql = "SELECT id, name, deleted_at FROM tbl_folder WHERE uid=" + su
                    + " AND deleted=1 AND trashed_root=1 ORDER BY deleted_at DESC";
        push_mysql(series, fsql, [resp, out, su](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "查询失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> rec;
            nlohmann::json folders = nlohmann::json::array();
            while (c.fetch_row(rec))
                folders.push_back({ {"id", rec[0].as_ulonglong()}, {"name", rec[1].as_string()}, {"deletedAt", rec[2].as_datetime()} });
            (*out)["folders"] = folders;

            string flsql = "SELECT id, filename, size, deleted_at FROM tbl_file WHERE uid=" + su
                         + " AND deleted=1 AND trashed_root=1 ORDER BY deleted_at DESC";
            push_mysql(series_of(t), flsql, [resp, out](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "查询失败"); return; }
                MySQLResultCursor c2{ t2->get_resp() };
                std::vector<MySQLCell> rec2;
                nlohmann::json files = nlohmann::json::array();
                while (c2.fetch_row(rec2))
                    files.push_back({ {"id", rec2[0].as_ulonglong()}, {"filename", rec2[1].as_string()},
                                      {"size", rec2[2].as_ulonglong()}, {"deletedAt", rec2[3].as_datetime()} });
                (*out)["files"] = files;
                api::ok(resp, *out);
            });
        });
    });

    // 恢复
    m_server.POST("/api/trash/restore", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        std::vector<long long> fileIds, folderIds;
        if (!ids_from_json(in, "fileIds", fileIds) || !ids_from_json(in, "folderIds", folderIds)) {
            api::fail(resp, 400, 400, "参数不合法"); return;
        }
        long long uid = user.id;
        if (fileIds.empty() && folderIds.empty()) { api::ok(resp, {}, "无变化"); return; }
        string su = std::to_string(uid);
        string username = user.username, ip = client_ip(req);
        long long nItems = (long long)fileIds.size() + (long long)folderIds.size();

        auto apply = [resp, fileIds, su, uid, username, ip, nItems](SeriesWork* s, std::shared_ptr<std::map<long long, string>> pathOf) {
            string q;
            if (!fileIds.empty())
                q += "UPDATE tbl_file SET deleted=0, deleted_at=NULL, trashed_root=0, dkey=0 "
                     "WHERE deleted=1 AND trashed_root=1 AND uid=" + su + " AND id IN (" + ids_csv(fileIds) + "); ";
            for (auto& kv : *pathOf) {
                const string& P = kv.second;
                string like = SqlUtil::quote(P + "%");
                q += "UPDATE tbl_folder SET deleted=0, deleted_at=NULL, trashed_root=0, dkey=0 "
                     "WHERE deleted=1 AND uid=" + su + " AND path LIKE " + like + "; ";
                q += "UPDATE tbl_file SET deleted=0, deleted_at=NULL, trashed_root=0, dkey=0 "
                     "WHERE deleted=1 AND uid=" + su + " AND parent_id IN (SELECT id FROM tbl_folder WHERE uid=" + su + " AND path LIKE " + like + "); ";
            }
            if (q.empty()) { api::ok(resp, {}, "无变化"); return; }
            push_mysql(s, q, [resp, uid, username, ip, nItems](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 409, 409, "恢复失败(原目录可能已存在同名项)"); return; }
                audit_log(uid, username, "file_restore", std::to_string(nItems) + " 个项目从回收站恢复", ip);
                api::ok(resp, {}, "已恢复");
            });
        };

        if (folderIds.empty()) {
            apply(series, std::make_shared<std::map<long long, string>>());
            return;
        }
        string q = "SELECT id, path FROM tbl_folder WHERE deleted=1 AND trashed_root=1 AND uid=" + su
                 + " AND id IN (" + ids_csv(folderIds) + ")";
        push_mysql(series, q, [resp, apply](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "恢复失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            auto pathOf = std::make_shared<std::map<long long, string>>();
            while (c.fetch_row(row)) (*pathOf)[row[0].as_ulonglong()] = row[1].as_string();
            apply(series_of(t), pathOf);
        });
    });

    // 彻底删除 (回收站顶层选中项; 文件夹连同子树; 回收无引用 blob)
    m_server.POST("/api/trash/delete", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        std::vector<long long> fileIds, folderIds;
        if (!ids_from_json(in, "fileIds", fileIds) || !ids_from_json(in, "folderIds", folderIds)) {
            api::fail(resp, 400, 400, "参数不合法"); return;
        }
        long long uid = user.id;
        if (fileIds.empty() && folderIds.empty()) { api::ok(resp, {}, "无变化"); return; }
        string su = std::to_string(uid);

        // pathLikes 用于选中文件夹的子树
        auto build_and_run = [resp, uid, fileIds, su](SeriesWork* s, std::shared_ptr<std::map<long long, string>> pathOf) {
            // 待删除文件的选择条件 (直接文件 + 子树内文件)
            string fileCond = "deleted=1 AND uid=" + su + " AND (";
            bool any = false;
            if (!fileIds.empty()) { fileCond += "id IN (" + ids_csv(fileIds) + ")"; any = true; }
            if (!pathOf->empty()) {
                string likes;
                for (auto& kv : *pathOf) { if (!likes.empty()) likes += " OR "; likes += "path LIKE " + SqlUtil::quote(kv.second + "%"); }
                fileCond += string(any ? " OR " : "") + "parent_id IN (SELECT id FROM tbl_folder WHERE uid=" + su + " AND (" + likes + "))";
                any = true;
            }
            fileCond += ")";
            if (!any) { api::ok(resp, {}, "无变化"); return; }

            // 1) 先取要删除文件的 hash 集合 (含历史版本 blob, 用于 blob GC)
            string hsql = "SELECT DISTINCT hashcode FROM tbl_file WHERE " + fileCond
                        + " UNION SELECT DISTINCT hashcode FROM tbl_file_version WHERE file_id IN "
                          "(SELECT id FROM (SELECT id FROM tbl_file WHERE " + fileCond + ") zf)";
            push_mysql(s, hsql, [resp, su, fileCond, pathOf](WFMySQLTask* t) {
                if (!mysql_ok(t)) { api::fail(resp, 500, 500, "删除失败"); return; }
                auto hashes = std::make_shared<std::vector<string>>();
                MySQLResultCursor c{ t->get_resp() };
                std::vector<MySQLCell> row;
                while (c.fetch_row(row)) hashes->push_back(row[0].as_string());

                // 2) 删除历史版本行 (其 blob 若不再被引用将在下方回收) + 文件行 + 文件夹行 (子树)
                string del = "DELETE FROM tbl_file_version WHERE file_id IN "
                             "(SELECT id FROM (SELECT id FROM tbl_file WHERE " + fileCond + ") zf); "
                             "DELETE FROM tbl_file_tag WHERE file_id IN "
                             "(SELECT id FROM (SELECT id FROM tbl_file WHERE " + fileCond + ") zt); "
                             "DELETE FROM tbl_file WHERE " + fileCond + "; ";
                if (!pathOf->empty()) {
                    string likes;
                    for (auto& kv : *pathOf) { if (!likes.empty()) likes += " OR "; likes += "path LIKE " + SqlUtil::quote(kv.second + "%"); }
                    del += "DELETE FROM tbl_folder WHERE deleted=1 AND uid=" + su + " AND (" + likes + "); ";
                }
                push_mysql(series_of(t), del, [resp, su, hashes](WFMySQLTask* t2) {
                    if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "删除失败"); return; }
                    if (hashes->empty()) { api::ok(resp, {}, "已彻底删除"); return; }
                    // 3) 找出仍被引用的 hash, 其余的 blob 可安全回收
                    string in_list;
                    for (auto& h : *hashes) { if (!in_list.empty()) in_list += ","; in_list += SqlUtil::quote(h); }
                    string refsql = "SELECT DISTINCT hashcode FROM tbl_file WHERE hashcode IN (" + in_list + ")"
                    " UNION SELECT DISTINCT hashcode FROM tbl_file_version WHERE hashcode IN (" + in_list + ")";
                    push_mysql(series_of(t2), refsql, [resp, hashes](WFMySQLTask* t3) {
                        std::set<string> alive;
                        if (mysql_ok(t3)) {
                            MySQLResultCursor c3{ t3->get_resp() };
                            std::vector<MySQLCell> r3;
                            while (c3.fetch_row(r3)) alive.insert(r3[0].as_string());
                        }
                        int removed = 0;
                        for (auto& h : *hashes) {
                            if (alive.count(h)) continue;
                            string blob = g_blob_dir + "/" + h;
                            if (file_exists(blob) && unlink(blob.c_str()) == 0) ++removed;
                        }
                        api::ok(resp, {{"blobsRemoved", removed}}, "已彻底删除");
                    });
                });
            });
        };

        if (folderIds.empty()) {
            build_and_run(series, std::make_shared<std::map<long long, string>>());
            return;
        }
        string q = "SELECT id, path FROM tbl_folder WHERE deleted=1 AND trashed_root=1 AND uid=" + su
                 + " AND id IN (" + ids_csv(folderIds) + ")";
        push_mysql(series, q, [resp, build_and_run](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "删除失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            auto pathOf = std::make_shared<std::map<long long, string>>();
            while (c.fetch_row(row)) (*pathOf)[row[0].as_ulonglong()] = row[1].as_string();
            build_and_run(series_of(t), pathOf);
        });
    });

    // 清空回收站
    m_server.POST("/api/trash/empty", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
        User user;
        if (!api::require_auth(req, resp, user)) return;
        string su = std::to_string(user.id);

        string hsql = "SELECT DISTINCT hashcode FROM tbl_file WHERE deleted=1 AND uid=" + su
                    + " UNION SELECT DISTINCT hashcode FROM tbl_file_version WHERE file_id IN "
                      "(SELECT id FROM (SELECT id FROM tbl_file WHERE deleted=1 AND uid=" + su + ") zf)";
        push_mysql(series, hsql, [resp, su](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "清空失败"); return; }
            auto hashes = std::make_shared<std::vector<string>>();
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            while (c.fetch_row(row)) hashes->push_back(row[0].as_string());

            string del = "DELETE FROM tbl_file_version WHERE file_id IN "
                         "(SELECT id FROM (SELECT id FROM tbl_file WHERE deleted=1 AND uid=" + su + ") zf); "
                         "DELETE FROM tbl_file_tag WHERE file_id IN "
                         "(SELECT id FROM (SELECT id FROM tbl_file WHERE deleted=1 AND uid=" + su + ") zt); "
                         "DELETE FROM tbl_file WHERE deleted=1 AND uid=" + su + "; "
                         "DELETE FROM tbl_folder WHERE deleted=1 AND uid=" + su + "; ";
            push_mysql(series_of(t), del, [resp, hashes](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "清空失败"); return; }
                if (hashes->empty()) { api::ok(resp, {}, "回收站已清空"); return; }
                string in_list;
                for (auto& h : *hashes) { if (!in_list.empty()) in_list += ","; in_list += SqlUtil::quote(h); }
                string refsql = "SELECT DISTINCT hashcode FROM tbl_file WHERE hashcode IN (" + in_list + ")"
                    " UNION SELECT DISTINCT hashcode FROM tbl_file_version WHERE hashcode IN (" + in_list + ")";
                push_mysql(series_of(t2), refsql, [resp, hashes](WFMySQLTask* t3) {
                    std::set<string> alive;
                    if (mysql_ok(t3)) {
                        MySQLResultCursor c3{ t3->get_resp() };
                        std::vector<MySQLCell> r3;
                        while (c3.fetch_row(r3)) alive.insert(r3[0].as_string());
                    }
                    int removed = 0;
                    for (auto& h : *hashes) {
                        if (alive.count(h)) continue;
                        string blob = g_blob_dir + "/" + h;
                        if (file_exists(blob) && unlink(blob.c_str()) == 0) ++removed;
                    }
                    api::ok(resp, {{"blobsRemoved", removed}}, "回收站已清空");
                });
            });
        });
    });
}
