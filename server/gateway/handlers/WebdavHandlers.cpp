// =============================================================================
// WebDAV 处理器: /webdav/* (Basic 鉴权)
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
// WebDAV: /webdav 挂载用户的文件树 (OPTIONS/PROPFIND/GET/HEAD/PUT/DELETE/MKCOL/MOVE)
//   认证: HTTP Basic, 密码可用账户密码或个人访问令牌 (cvt_...)
// =============================================================================
namespace {

std::string dav_xml_escape(const std::string& s)
{
    std::string o; o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string dav_http_date()
{
    char buf[64]; time_t t = time(nullptr); struct tm g; gmtime_r(&t, &g);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
    return buf;
}

long long dav_affected(WFMySQLTask* t)
{
    MySQLResultCursor c{ t->get_resp() };
    return (long long)c.get_affected_rows();
}

std::vector<std::string> dav_segments(const std::string& matchPath)
{
    std::vector<std::string> segs;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        std::string d = wfrest::CodeUtil::url_decode(cur);
        cur.clear();
        if (d.empty() || d == "." || d == "..") return;
        if (d.size() > 255) d = d.substr(0, 255);
        segs.push_back(d);
    };
    for (char ch : matchPath) { if (ch == '/') flush(); else cur += ch; }
    flush();
    return segs;
}

std::string dav_href(const std::vector<std::string>& segs, bool isCollection)
{
    std::string h = "/webdav";
    for (auto& s : segs) h += "/" + wfrest::CodeUtil::url_encode(s);
    if (isCollection) h += "/";
    return h;
}

void dav_deny(HttpResp* resp)
{
    resp->set_status(401);
    resp->add_header("WWW-Authenticate", "Basic realm=\"CloudVault WebDAV\"");
    resp->String("Unauthorized");
}

using DavResolveCb = std::function<void(bool, long long, std::string, SeriesWork*)>;

void dav_resolve(long long uid, std::shared_ptr<std::vector<std::string>> segs, size_t idx,
                 long long curId, const std::string& curPath, SeriesWork* s, DavResolveCb cb)
{
    if (idx >= segs->size()) { cb(true, curId, curPath, s); return; }
    std::string sql = "SELECT id, path FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(uid)
                    + " AND parent_id=" + std::to_string(curId)
                    + " AND name=" + SqlUtil::quote((*segs)[idx]) + " LIMIT 1";
    push_mysql(s, sql, [uid, segs, idx, cb](WFMySQLTask* t) {
        if (!mysql_ok(t)) { cb(false, 0, "", series_of(t)); return; }
        MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
        if (!c.fetch_row(row)) { cb(false, 0, "", series_of(t)); return; }
        long long id = cell_ll(row[0]); std::string p = row[1].as_string();
        dav_resolve(uid, segs, idx + 1, id, p, series_of(t), cb);
    });
}

void dav_emit_file_response(HttpResp* resp, const std::vector<std::string>& segs, const std::string& name, long long size)
{
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">\n";
    x += "  <D:response><D:href>" + dav_xml_escape(dav_href(segs, false)) + "</D:href>\n";
    x += "    <D:propstat><D:prop>\n";
    x += "      <D:displayname>" + dav_xml_escape(name) + "</D:displayname>\n";
    x += "      <D:getlastmodified>" + dav_http_date() + "</D:getlastmodified>\n";
    x += "      <D:resourcetype/>\n";
    x += "      <D:getcontentlength>" + std::to_string(size) + "</D:getcontentlength>\n";
    x += "      <D:getcontenttype>" + dav_xml_escape(mime_of(name)) + "</D:getcontenttype>\n";
    x += "    </D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n  </D:response>\n";
    x += "</D:multistatus>\n";
    resp->set_status(207);
    resp->add_header("Content-Type", "application/xml; charset=utf-8");
    resp->String(x);
}

void dav_propfind(const User& u, const HttpReq* req, HttpResp* resp, SeriesWork* series,
                  std::shared_ptr<std::vector<std::string>> segs)
{
    std::string depth = req->has_header("Depth") ? req->header("Depth") : "1";
    long long uid = u.id;

    dav_resolve(uid, segs, 0, 0, "/", series, [uid, resp, segs, depth](bool ok, long long folderId, std::string, SeriesWork* s) {
        if (ok) {
            using Item = std::tuple<std::vector<std::string>, bool, long long>;
            auto emit = [resp](std::shared_ptr<std::vector<Item>> items) {
                std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">\n";
                std::string date = dav_http_date();
                for (auto& it : *items) {
                    const auto& isegs = std::get<0>(it);
                    bool col = std::get<1>(it);
                    long long size = std::get<2>(it);
                    std::string name = isegs.empty() ? std::string("CloudVault") : isegs.back();
                    x += "  <D:response><D:href>" + dav_xml_escape(dav_href(isegs, col)) + "</D:href>\n";
                    x += "    <D:propstat><D:prop>\n";
                    x += "      <D:displayname>" + dav_xml_escape(name) + "</D:displayname>\n";
                    x += "      <D:getlastmodified>" + date + "</D:getlastmodified>\n";
                    if (col) {
                        x += "      <D:resourcetype><D:collection/></D:resourcetype>\n";
                    } else {
                        x += "      <D:resourcetype/>\n";
                        x += "      <D:getcontentlength>" + std::to_string(size) + "</D:getcontentlength>\n";
                        x += "      <D:getcontenttype>" + dav_xml_escape(mime_of(name)) + "</D:getcontenttype>\n";
                    }
                    x += "    </D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n  </D:response>\n";
                }
                x += "</D:multistatus>\n";
                resp->set_status(207);
                resp->add_header("Content-Type", "application/xml; charset=utf-8");
                resp->String(x);
            };
            auto items = std::make_shared<std::vector<Item>>();
            items->emplace_back(*segs, true, 0);
            if (depth == "0") { emit(items); return; }
            std::string fq = "SELECT name FROM tbl_folder WHERE deleted=0 AND uid=" + std::to_string(uid)
                           + " AND parent_id=" + std::to_string(folderId) + " ORDER BY name";
            push_mysql(s, fq, [uid, folderId, segs, items, emit](WFMySQLTask* t) {
                if (mysql_ok(t)) {
                    MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                    while (c.fetch_row(row)) { auto ns = *segs; ns.push_back(row[0].as_string()); items->emplace_back(ns, true, 0); }
                }
                std::string flq = "SELECT filename, size FROM tbl_file WHERE deleted=0 AND uid=" + std::to_string(uid)
                                + " AND parent_id=" + std::to_string(folderId) + " ORDER BY filename";
                push_mysql(series_of(t), flq, [segs, items, emit](WFMySQLTask* t2) {
                    if (mysql_ok(t2)) {
                        MySQLResultCursor c{ t2->get_resp() }; std::vector<MySQLCell> row;
                        while (c.fetch_row(row)) { auto ns = *segs; ns.push_back(row[0].as_string()); items->emplace_back(ns, false, (long long)row[1].as_ulonglong()); }
                    }
                    emit(items);
                });
            });
            return;
        }
        // 尝试作为文件
        if (segs->empty()) { resp->set_status(404); resp->String("Not Found"); return; }
        auto parent = std::make_shared<std::vector<std::string>>(segs->begin(), segs->end() - 1);
        dav_resolve(uid, parent, 0, 0, "/", s, [uid, resp, segs](bool ok2, long long pid, std::string, SeriesWork* s2) {
            if (!ok2) { resp->set_status(404); resp->String("Not Found"); return; }
            std::string fname = segs->back();
            std::string sql = "SELECT filename, size FROM tbl_file WHERE deleted=0 AND uid=" + std::to_string(uid)
                            + " AND parent_id=" + std::to_string(pid) + " AND filename=" + SqlUtil::quote(fname) + " LIMIT 1";
            push_mysql(s2, sql, [resp, segs](WFMySQLTask* t) {
                if (!mysql_ok(t)) { resp->set_status(500); resp->String("Error"); return; }
                MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
                if (!c.fetch_row(row)) { resp->set_status(404); resp->String("Not Found"); return; }
                dav_emit_file_response(resp, *segs, segs->back(), (long long)row[1].as_ulonglong());
            });
        });
    });
}

void dav_get(const User& u, bool headOnly, HttpResp* resp, SeriesWork* series,
             std::shared_ptr<std::vector<std::string>> segs)
{
    long long uid = u.id;
    if (segs->empty()) { resp->set_status(405); resp->String("Cannot GET a collection"); return; }
    auto parent = std::make_shared<std::vector<std::string>>(segs->begin(), segs->end() - 1);
    dav_resolve(uid, parent, 0, 0, "/", series, [uid, headOnly, resp, segs](bool ok, long long pid, std::string, SeriesWork* s) {
        if (!ok) { resp->set_status(404); resp->String("Not Found"); return; }
        std::string fname = segs->back();
        std::string sql = "SELECT filename, hashcode, size FROM tbl_file WHERE deleted=0 AND uid=" + std::to_string(uid)
                        + " AND parent_id=" + std::to_string(pid) + " AND filename=" + SqlUtil::quote(fname) + " LIMIT 1";
        push_mysql(s, sql, [headOnly, resp](WFMySQLTask* t) {
            if (!mysql_ok(t)) { resp->set_status(500); resp->String("Error"); return; }
            MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { resp->set_status(404); resp->String("Not Found"); return; }
            std::string name = row[0].as_string();
            std::string blob = blob_path(row[1].as_string());
            long long size = (long long)row[2].as_ulonglong();
            if (!file_exists(blob)) { resp->set_status(404); resp->String("Not Found"); return; }
            resp->add_header("Content-Type", mime_of(name));
            if (headOnly) {
                resp->add_header("Content-Length", std::to_string(size));
                resp->set_status(200); resp->String("");
            } else {
                g_metrics.downloads++; g_metrics.download_bytes += (long long)file_size_of(blob);
                resp->File(blob);
            }
        });
    });
}

void dav_put(const User& u, const HttpReq* req, HttpResp* resp, SeriesWork* series,
             std::shared_ptr<std::vector<std::string>> segs)
{
    long long uid = u.id;
    if (segs->empty()) { resp->set_status(409); resp->String("Conflict"); return; }
    auto parent = std::make_shared<std::vector<std::string>>(segs->begin(), segs->end() - 1);
    auto body = std::make_shared<std::string>(req->body());
    dav_resolve(uid, parent, 0, 0, "/", series, [uid, resp, segs, body](bool ok, long long pid, std::string, SeriesWork* s) {
        if (!ok) { resp->set_status(409); resp->String("Parent collection missing"); return; }
        std::string fname = segs->back();
        long long incoming = (long long)body->size();
        if (incoming > g_max_file_size) { resp->set_status(413); resp->String("Too Large"); return; }
        std::string usageSql = usage_quota_sql(uid);
        push_mysql(s, usageSql, [uid, resp, fname, body, pid, incoming](WFMySQLTask* t) {
            long long used = 0, quota = g_user_quota;
            if (mysql_ok(t)) { MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> r; if (c.fetch_row(r)) { quota = cell_ll(r[0]); used = (long long)r[1].as_ulonglong(); } }
            if (used + incoming > quota) { resp->set_status(507); resp->String("Insufficient Storage"); return; }
            std::string hash = CryptoUtil::generate_hashcode(body->c_str(), body->size());
            bool isNew = write_blob_if_absent(hash, *body);
            if (isNew) publish_oss_backup(hash);
            std::string ins = "REPLACE INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                + std::to_string(uid) + ", " + std::to_string(pid) + ", " + SqlUtil::quote(fname) + ", "
                + SqlUtil::quote(hash) + ", " + std::to_string(incoming) + ")";
            push_mysql(series_of(t), ins, [resp, incoming](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { resp->set_status(500); resp->String("Error"); return; }
                g_metrics.uploads++; g_metrics.upload_bytes += incoming;
                resp->set_status(201); resp->String("Created");
            });
        });
    });
}

void dav_delete(const User& u, HttpResp* resp, SeriesWork* series,
                std::shared_ptr<std::vector<std::string>> segs)
{
    long long uid = u.id; std::string su = std::to_string(uid);
    if (segs->empty()) { resp->set_status(403); resp->String("Cannot delete root"); return; }
    dav_resolve(uid, segs, 0, 0, "/", series, [uid, su, resp, segs](bool ok, long long fid, std::string fpath, SeriesWork* s) {
        if (ok) {
            std::string like = SqlUtil::quote(fpath + "%");
            std::string q;
            q += "UPDATE tbl_folder SET deleted=1, deleted_at=NOW(), trashed_root=IF(id=" + std::to_string(fid) + ",1,0), dkey=id "
                 "WHERE deleted=0 AND uid=" + su + " AND path LIKE " + like + "; ";
            q += "UPDATE tbl_file SET deleted=1, deleted_at=NOW(), trashed_root=0, dkey=id "
                 "WHERE deleted=0 AND uid=" + su + " AND parent_id IN (SELECT id FROM (SELECT id FROM tbl_folder WHERE uid=" + su + " AND path LIKE " + like + ") z); ";
            push_mysql(s, q, [resp](WFMySQLTask* t) {
                if (!mysql_ok(t)) { resp->set_status(500); resp->String("Error"); return; }
                resp->set_status(204); resp->String("");
            });
            return;
        }
        auto parent = std::make_shared<std::vector<std::string>>(segs->begin(), segs->end() - 1);
        dav_resolve(uid, parent, 0, 0, "/", s, [uid, su, resp, segs](bool ok2, long long pid, std::string, SeriesWork* s2) {
            if (!ok2) { resp->set_status(404); resp->String("Not Found"); return; }
            std::string fname = segs->back();
            std::string q = "UPDATE tbl_file SET deleted=1, deleted_at=NOW(), trashed_root=1, dkey=id "
                            "WHERE deleted=0 AND uid=" + su + " AND parent_id=" + std::to_string(pid)
                          + " AND filename=" + SqlUtil::quote(fname);
            push_mysql(s2, q, [resp](WFMySQLTask* t) {
                if (!mysql_ok(t)) { resp->set_status(500); resp->String("Error"); return; }
                if (dav_affected(t) == 0) { resp->set_status(404); resp->String("Not Found"); return; }
                resp->set_status(204); resp->String("");
            });
        });
    });
}

void dav_mkcol(const User& u, HttpResp* resp, SeriesWork* series,
               std::shared_ptr<std::vector<std::string>> segs)
{
    long long uid = u.id;
    if (segs->empty()) { resp->set_status(405); resp->String("Method Not Allowed"); return; }
    auto parent = std::make_shared<std::vector<std::string>>(segs->begin(), segs->end() - 1);
    dav_resolve(uid, parent, 0, 0, "/", series, [uid, resp, segs](bool ok, long long pid, std::string ppath, SeriesWork* s) {
        if (!ok) { resp->set_status(409); resp->String("Conflict"); return; }
        std::string name = segs->back();
        std::string ins = "INSERT INTO tbl_folder (uid, name, parent_id, path) VALUES ("
            + std::to_string(uid) + ", " + SqlUtil::quote(name) + ", " + std::to_string(pid) + ", '/')";
        push_mysql(s, ins, [resp, ppath](WFMySQLTask* t) {
            if (!mysql_ok(t)) { resp->set_status(405); resp->String("Exists"); return; }
            MySQLResultCursor c{ t->get_resp() };
            long long id = c.get_insert_id();
            std::string np = ppath + std::to_string(id) + "/";
            std::string up = "UPDATE tbl_folder SET path=" + SqlUtil::quote(np) + " WHERE id=" + std::to_string(id);
            push_mysql(series_of(t), up, [resp](WFMySQLTask*) { resp->set_status(201); resp->String("Created"); });
        });
    });
}

void dav_move(const User& u, const HttpReq* req, HttpResp* resp, SeriesWork* series,
              std::shared_ptr<std::vector<std::string>> segs)
{
    long long uid = u.id; std::string su = std::to_string(uid);
    if (segs->empty()) { resp->set_status(403); resp->String("Cannot move root"); return; }
    if (!req->has_header("Destination")) { resp->set_status(400); resp->String("Missing Destination"); return; }
    std::string dest = req->header("Destination");
    auto pos = dest.find("/webdav");
    if (pos == std::string::npos) { resp->set_status(502); resp->String("Bad Destination"); return; }
    auto dsegs = std::make_shared<std::vector<std::string>>(dav_segments(dest.substr(pos + 7)));
    if (dsegs->empty()) { resp->set_status(403); resp->String("Bad Destination"); return; }
    std::string newName = dsegs->back();
    auto dParent = std::make_shared<std::vector<std::string>>(dsegs->begin(), dsegs->end() - 1);

    dav_resolve(uid, dParent, 0, 0, "/", series, [uid, su, resp, segs, newName](bool okd, long long destPid, std::string, SeriesWork* sd) {
        if (!okd) { resp->set_status(409); resp->String("Destination parent missing"); return; }
        dav_resolve(uid, segs, 0, 0, "/", sd, [uid, su, resp, segs, newName, destPid](bool okf, long long fid, std::string fpath, SeriesWork* s) {
            if (okf) {
                std::string q;
                q += "SET @pp := (SELECT IFNULL((SELECT path FROM tbl_folder WHERE uid=" + su + " AND id=" + std::to_string(destPid) + "),'/')); ";
                q += "SET @old := " + SqlUtil::quote(fpath) + "; ";
                q += "SET @new := CONCAT(@pp, " + std::to_string(fid) + ", '/'); ";
                q += "UPDATE tbl_folder SET path=CONCAT(@new, SUBSTRING(path, CHAR_LENGTH(@old)+1)) "
                     "WHERE uid=" + su + " AND deleted=0 AND path LIKE CONCAT(@old,'%') AND id<>" + std::to_string(fid) + "; ";
                q += "UPDATE tbl_folder SET parent_id=" + std::to_string(destPid) + ", name=" + SqlUtil::quote(newName)
                   + ", path=@new WHERE uid=" + su + " AND id=" + std::to_string(fid) + "; ";
                push_mysql(s, q, [resp](WFMySQLTask* t) {
                    if (!mysql_ok(t)) { resp->set_status(500); resp->String("Error"); return; }
                    resp->set_status(201); resp->String("Moved");
                });
                return;
            }
            auto parent = std::make_shared<std::vector<std::string>>(segs->begin(), segs->end() - 1);
            dav_resolve(uid, parent, 0, 0, "/", s, [uid, su, resp, segs, newName, destPid](bool okp, long long pid, std::string, SeriesWork* s2) {
                if (!okp) { resp->set_status(404); resp->String("Not Found"); return; }
                std::string fname = segs->back();
                std::string q = "UPDATE tbl_file SET parent_id=" + std::to_string(destPid) + ", filename=" + SqlUtil::quote(newName)
                              + " WHERE deleted=0 AND uid=" + su + " AND parent_id=" + std::to_string(pid)
                              + " AND filename=" + SqlUtil::quote(fname);
                push_mysql(s2, q, [resp](WFMySQLTask* t) {
                    if (!mysql_ok(t)) { resp->set_status(500); resp->String("Error"); return; }
                    if (dav_affected(t) == 0) { resp->set_status(404); resp->String("Not Found"); return; }
                    resp->set_status(201); resp->String("Moved");
                });
            });
        });
    });
}

void dav_dispatch(const std::string& method, const User& u, const HttpReq* req, HttpResp* resp, SeriesWork* series)
{
    auto segs = std::make_shared<std::vector<std::string>>(dav_segments(req->match_path()));
    if (method == "OPTIONS") {
        resp->add_header("DAV", "1, 2");
        resp->add_header("MS-Author-Via", "DAV");
        resp->add_header("Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE");
        resp->set_status(200); resp->String("");
    } else if (method == "PROPFIND") { dav_propfind(u, req, resp, series, segs); }
    else if (method == "GET")       { dav_get(u, false, resp, series, segs); }
    else if (method == "HEAD")      { dav_get(u, true, resp, series, segs); }
    else if (method == "PUT")       { dav_put(u, req, resp, series, segs); }
    else if (method == "DELETE")    { dav_delete(u, resp, series, segs); }
    else if (method == "MKCOL")     { dav_mkcol(u, resp, series, segs); }
    else if (method == "MOVE")      { dav_move(u, req, resp, series, segs); }
    else { resp->set_status(405); resp->String("Method Not Allowed"); }
}

void dav_authenticate(const HttpReq* req, HttpResp* resp, SeriesWork* series, std::function<void(User)> onOk)
{
    if (!req->has_header("Authorization")) { dav_deny(resp); return; }
    std::string h = req->header("Authorization");
    if (h.rfind("Basic ", 0) != 0) { dav_deny(resp); return; }
    std::string dec = Base64::decode(h.substr(6));
    auto pos = dec.find(':');
    if (pos == std::string::npos) { dav_deny(resp); return; }
    std::string uname = dec.substr(0, pos), secret = dec.substr(pos + 1);
    if (secret.rfind("cvt_", 0) == 0) {
        User u; std::string hash; bool persist = false;
        if (g_tokens.resolve(secret, u, hash, persist)) {
            g_metrics.token_auth++;
            if (persist) {
                std::string sql = "UPDATE tbl_token SET last_used_at=NOW() WHERE token_hash=" + SqlUtil::quote(hash);
                WFMySQLTask* t = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
                t->get_req()->set_query(sql); t->start();
            }
            onOk(u);
        } else dav_deny(resp);
        return;
    }
    if (uname.empty()) { dav_deny(resp); return; }
    std::string sql = "SELECT id, username, password, salt, disabled, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') "
                      "FROM tbl_user WHERE username=" + SqlUtil::quote(uname) + " AND tomb=0";
    push_mysql(series, sql, [resp, secret, onOk](WFMySQLTask* t) {
        if (!mysql_ok(t)) { dav_deny(resp); return; }
        MySQLResultCursor c{ t->get_resp() }; std::vector<MySQLCell> row;
        if (!c.fetch_row(row)) { dav_deny(resp); return; }
        User u; u.id = (int)cell_ll(row[0]); u.username = row[1].as_string();
        std::string ph = row[2].as_string(), salt = row[3].as_string();
        int dis = (int)cell_ll(row[4]);
        u.createdAt = row[5].is_string() ? row[5].as_string() : std::string();
        if (dis || CryptoUtil::hash_password(secret, salt) != ph) { dav_deny(resp); return; }
        onOk(u);
    });
}

void dav_handle(const HttpReq* req, HttpResp* resp, SeriesWork* series)
{
    g_metrics.webdav_requests++;
    std::string method = req->get_method();
    dav_authenticate(req, resp, series, [method, req, resp, series](User u) {
        dav_dispatch(method, u, req, resp, series);
    });
}

} // namespace (webdav)

void CloudiskServer::register_webdav_module()
{
    m_server.ROUTE("/webdav", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) { dav_handle(req, resp, series); }, Verb::ANY);
    m_server.ROUTE("/webdav/*", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) { dav_handle(req, resp, series); }, Verb::ANY);
}
