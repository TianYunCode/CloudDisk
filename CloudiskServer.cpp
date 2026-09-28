// =============================================================================
// CloudiskServer —— HTTP API 网关
//   - 统一 JSON 响应封装 (code/message/data)
//   - Bearer Token 鉴权
//   - 内容寻址的 Blob 存储 + 秒传去重
//   - 上传(多文件/进度) / 列表(搜索/排序/分页) / 下载 / 删除 / 重命名 / 用量统计
//   - 参数化转义, 杜绝 SQL 注入
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

// ----- 运行期配置 (启动时从 Config 读取一次) -----------------------------------
static string  g_mysql_url;
static string  g_rabbitmq_url;
static string  g_consul_url;
static string  g_storage_dir;
static string  g_blob_dir;
static string  g_thumb_dir;
static string  g_upload_dir;
static long long g_max_file_size;
static long long g_user_quota;
static const int RETRY_MAX = 3;

// ----- 小工具 -----------------------------------------------------------------
namespace {

// ----- Prometheus 指标计数器 (进程级, 线程安全) -------------------------------
struct Metrics {
    std::atomic<long long> uploads{0}, upload_bytes{0}, downloads{0}, download_bytes{0};
    std::atomic<long long> logins{0}, login_fails{0}, shares_created{0};
    std::atomic<long long> token_auth{0}, webdav_requests{0};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
Metrics g_metrics;

// ----- API Token 内存注册表 (支持 require_auth 同步解析) -----------------------
struct TokenRegistry {
    struct Ent { int uid; string username; string createdAt; long long expires; long long lastPersist; };
    std::mutex mu;
    std::unordered_map<string, Ent> map;   // key = token_hash
    static string hash_of(const string& tok) { return CryptoUtil::generate_hashcode(tok.data(), tok.size()); }
    void put(const string& hash, int uid, const string& un, const string& ca, long long exp) {
        std::lock_guard<std::mutex> l(mu); map[hash] = Ent{ uid, un, ca, exp, 0 };
    }
    void erase(const string& hash) { std::lock_guard<std::mutex> l(mu); map.erase(hash); }
    bool resolve(const string& tok, User& u, string& hashOut, bool& persist) {
        hashOut = hash_of(tok); persist = false;
        std::lock_guard<std::mutex> l(mu);
        auto it = map.find(hashOut);
        if (it == map.end()) return false;
        long long now = (long long)time(nullptr);
        if (it->second.expires > 0 && now > it->second.expires) { map.erase(it); return false; }
        u.id = it->second.uid; u.username = it->second.username; u.createdAt = it->second.createdAt;
        if (now - it->second.lastPersist > 60) { it->second.lastPersist = now; persist = true; }
        return true;
    }
};
TokenRegistry g_tokens;

void mkdir_p(const string& path)
{
    string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) {
            if (!cur.empty() && access(cur.c_str(), F_OK) != 0)
                mkdir(cur.c_str(), 0755);
        }
    }
}

bool file_exists(const string& p) { return access(p.c_str(), F_OK) == 0; }

// 按扩展名推断 MIME 类型 (用于在线预览时正确渲染)
string mime_of(const string& filename)
{
    string ext;
    auto dot = filename.rfind('.');
    if (dot != string::npos) { ext = filename.substr(dot + 1); for (auto& ch : ext) ch = (char)tolower((unsigned char)ch); }
    static const std::map<string, string> M = {
        {"png","image/png"}, {"jpg","image/jpeg"}, {"jpeg","image/jpeg"}, {"gif","image/gif"},
        {"webp","image/webp"}, {"bmp","image/bmp"}, {"svg","image/svg+xml"}, {"ico","image/x-icon"},
        {"mp4","video/mp4"}, {"webm","video/webm"}, {"mov","video/quicktime"}, {"mkv","video/x-matroska"}, {"ogv","video/ogg"},
        {"mp3","audio/mpeg"}, {"wav","audio/wav"}, {"flac","audio/flac"}, {"aac","audio/aac"},
        {"ogg","audio/ogg"}, {"m4a","audio/mp4"},
        {"pdf","application/pdf"},
        {"txt","text/plain; charset=utf-8"}, {"md","text/markdown; charset=utf-8"},
        {"json","application/json; charset=utf-8"}, {"xml","application/xml; charset=utf-8"},
        {"csv","text/csv; charset=utf-8"}, {"log","text/plain; charset=utf-8"},
        {"js","text/javascript; charset=utf-8"}, {"ts","text/plain; charset=utf-8"},
        {"css","text/css; charset=utf-8"}, {"html","text/html; charset=utf-8"},
        {"c","text/plain; charset=utf-8"}, {"cpp","text/plain; charset=utf-8"}, {"h","text/plain; charset=utf-8"},
        {"py","text/plain; charset=utf-8"}, {"java","text/plain; charset=utf-8"}, {"go","text/plain; charset=utf-8"},
        {"sh","text/plain; charset=utf-8"}, {"rs","text/plain; charset=utf-8"}, {"yml","text/plain; charset=utf-8"},
        {"yaml","text/plain; charset=utf-8"}, {"sql","text/plain; charset=utf-8"},
    };
    auto it = M.find(ext);
    return it == M.end() ? "application/octet-stream" : it->second;
}

// 判断扩展名是否为可生成缩略图的位图格式 (stb 支持)
bool is_thumbnailable(const string& filename)
{
    string ext;
    auto dot = filename.rfind('.');
    if (dot != string::npos) { ext = filename.substr(dot + 1); for (auto& ch : ext) ch = (char)tolower((unsigned char)ch); }
    static const std::set<string> S = {"png","jpg","jpeg","gif","bmp","tga","psd","ppm","pgm"};
    return S.count(ext) > 0;
}

// 取文件字节大小 (失败返回 -1)
long long file_size_of(const string& p)
{
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
}

// 原子写入文件 (完整覆盖)
bool write_file_all(const string& path, const char* data, size_t n)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w <= 0) { close(fd); return false; }
        off += (size_t)w;
    }
    close(fd);
    return true;
}

// 读取整个文件到字符串
bool read_file_all(const string& path, string& out)
{
    long long sz = file_size_of(path);
    if (sz < 0) return false;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.resize((size_t)sz);
    size_t off = 0;
    while (off < (size_t)sz) {
        ssize_t r = read(fd, &out[off], (size_t)sz - off);
        if (r <= 0) { close(fd); return false; }
        off += (size_t)r;
    }
    close(fd);
    return true;
}

// 分片会话目录: 列出已存在的分片序号
std::set<int> list_uploaded_chunks(const string& dir)
{
    std::set<int> present;
    DIR* d = opendir(dir.c_str());
    if (!d) return present;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        string name = ent->d_name;
        auto dot = name.rfind(".part");
        if (dot != string::npos && dot + 5 == name.size()) {
            try { present.insert(std::stoi(name.substr(0, dot))); } catch (...) {}
        }
    }
    closedir(d);
    return present;
}

// 删除分片会话目录及其内容
void remove_upload_dir(const string& dir)
{
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            string name = ent->d_name;
            if (name == "." || name == "..") continue;
            unlink((dir + "/" + name).c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

// 把内容写入 blob (若已存在则跳过, 实现去重)。返回是否为新写入。
bool write_blob_if_absent(const string& hash, const string& content)
{
    string path = g_blob_dir + "/" + hash;
    if (file_exists(path)) return false;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t off = 0, n = (ssize_t)content.size();
    while (off < n) {
        ssize_t w = write(fd, content.data() + off, n - off);
        if (w <= 0) break;
        off += w;
    }
    close(fd);
    return true;
}

// 异步备份: 把 blob 对象投递到 RabbitMQ (失败不影响主流程)。
void publish_oss_backup(const string& hash)
{
    try {
        Channel::ptr_t channel = Channel::CreateFromUri(g_rabbitmq_url);
        nlohmann::json obj;
        obj["object"] = hash;                       // OSS 对象键 = 内容哈希
        obj["file"]   = g_blob_dir + "/" + hash;    // 本地 blob 路径
        BasicMessage::ptr_t msg = BasicMessage::Create(obj.dump());
        channel->BasicPublish("oss.direct", "oss", msg);
    } catch (const std::exception& e) {
        LOG_WARN("RabbitMQ 备份投递失败(忽略): " << e.what());
    }
}

// 通过 Consul 同步发现 UserService 实例地址。成功返回 true。
bool discover_userservice(string& ip, unsigned short& port)
{
    ip.clear(); port = 0;
    WFFacilities::WaitGroup wg{ 1 };
    string url = g_consul_url + "/v1/health/service/UserService?passing=true";
    WFHttpTask* task = WFTaskFactory::create_http_task(url, 3, 3, [&](WFHttpTask* t) {
        if (t->get_state() != WFT_STATE_SUCCESS) { wg.done(); return; }
        string body = HttpUtil::decode_chunked_body(t->get_resp());
        try {
            auto data = nlohmann::json::parse(body);
            if (!data.empty()) {
                ip   = data[0]["Service"]["Address"].get<string>();
                port = data[0]["Service"]["Port"].get<unsigned short>();
            }
        } catch (...) {}
        wg.done();
    });
    task->start();
    wg.wait();
    return !ip.empty() && port != 0;
}

// 解析请求体为 JSON (兼容 application/json 与表单)。失败返回 false。
bool parse_body(const HttpReq* req, nlohmann::json& out)
{
    if (req->content_type() == APPLICATION_URLENCODED || req->content_type() == MULTIPART_FORM_DATA) {
        out = nlohmann::json::object();
        for (auto& kv : req->form_kv()) out[kv.first] = kv.second;
        return true;
    }
    try { out = nlohmann::json::parse(req->body()); return true; }
    catch (...) { return false; }
}

string json_str(const nlohmann::json& j, const char* key)
{
    if (j.contains(key) && j[key].is_string()) return j[key].get<string>();
    return string();
}

// 创建并向 series 追加一个 MySQL 任务。
WFMySQLTask* push_mysql(SeriesWork* series, const string& sql, mysql_callback_t cb)
{
    WFMySQLTask* t = WFTaskFactory::create_mysql_task(g_mysql_url, RETRY_MAX, std::move(cb));
    t->get_req()->set_query(sql);
    series->push_back(t);
    return t;
}

bool mysql_ok(WFMySQLTask* task)
{
    return task->get_state() == WFT_STATE_SUCCESS &&
           task->get_resp()->get_packet_type() != MYSQL_PACKET_ERROR;
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
            const string s = v.get<string>();
            if (s.empty()) return false;
            for (char c : s) if (c < '0' || c > '9') return false;
            out.push_back(std::stoll(s));
        } else return false;
    }
    return true;
}

// 把 id 列表拼成安全的 "1,2,3" (仅数字, 无注入风险)。
string ids_csv(const std::vector<long long>& v)
{
    string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
    return s;
}

// 生成 URL 安全的随机字符串 (加密级随机)。用于分享 token / 提取码。
string gen_token(int n)
{
    static const char* alpha = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::vector<unsigned char> buf(n);
    string out;
    if (RAND_bytes(buf.data(), n) == 1) {
        for (int i = 0; i < n; ++i) out += alpha[buf[i] % 62];
    } else {
        for (int i = 0; i < n; ++i) out += alpha[rand() % 62];
    }
    return out;
}

// 通用: 读取任意数值型单元格为 long long (兼容 INT/TINYINT/BIGINT/字符串)。
long long cell_ll(const MySQLCell& c)
{
    if (c.is_int()) return c.as_int();
    if (c.is_ulonglong()) return (long long)c.as_ulonglong();
    if (c.is_string()) { try { return std::stoll(c.as_string()); } catch (...) { return 0; } }
    return 0;
}

// 用量+有效配额查询: 第 1 列为该用户有效配额(个人配额>0 则用之, 否则全局默认), 第 2 列为已用字节
string usage_quota_sql(long long uid)
{
    string g = std::to_string(g_user_quota);
    string u = std::to_string(uid);
    return "SELECT (SELECT IF(quota>0,quota," + g + ") FROM tbl_user WHERE id=" + u + "), "
           "CAST(COALESCE(SUM(size),0) AS UNSIGNED) FROM tbl_file WHERE deleted=0 AND uid=" + u;
}

// 客户端 IP (尽力而为: 优先反代头)
string client_ip(const HttpReq* req)
{
    if (req->has_header("X-Forwarded-For")) { string v = req->header("X-Forwarded-For"); auto c = v.find(','); return c == string::npos ? v : v.substr(0, c); }
    if (req->has_header("X-Real-IP")) return req->header("X-Real-IP");
    return "-";
}

// ---- 审计日志: 异步 fire-and-forget 写入 ----
void audit_log(long long uid, const string& username, const string& action, const string& detail, const string& ip)
{
    string d = detail.size() > 500 ? detail.substr(0, 500) : detail;
    string sql = "INSERT INTO tbl_audit (uid, username, action, detail, ip) VALUES ("
        + std::to_string(uid) + ", " + SqlUtil::quote(username) + ", " + SqlUtil::quote(action) + ", "
        + SqlUtil::quote(d) + ", " + SqlUtil::quote(ip) + ")";
    WFMySQLTask* t = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [](WFMySQLTask*){});
    t->get_req()->set_query(sql);
    t->start();
}

// ---- 登录失败限速 (进程内, 按 用户名|IP 计数) ----
struct RlEntry { int fails = 0; time_t first = 0; time_t blockUntil = 0; };
static std::mutex g_rl_mutex;
static std::unordered_map<string, RlEntry> g_rl;
static const int   RL_MAX_FAILS = 5;      // 窗口内最大失败次数
static const int   RL_WINDOW    = 300;    // 统计窗口(秒)
static const int   RL_COOLDOWN  = 300;    // 触发后冷却(秒)

// 返回剩余冷却秒数(>0 表示被限流)
int rl_blocked(const string& key)
{
    std::lock_guard<std::mutex> lk(g_rl_mutex);
    auto it = g_rl.find(key);
    if (it == g_rl.end()) return 0;
    time_t now = time(nullptr);
    if (it->second.blockUntil > now) return (int)(it->second.blockUntil - now);
    return 0;
}
void rl_on_fail(const string& key)
{
    std::lock_guard<std::mutex> lk(g_rl_mutex);
    time_t now = time(nullptr);
    RlEntry& e = g_rl[key];
    if (now - e.first > RL_WINDOW) { e.first = now; e.fails = 0; }
    e.fails++;
    if (e.fails >= RL_MAX_FAILS) { e.blockUntil = now + RL_COOLDOWN; e.fails = 0; e.first = now; }
}
void rl_on_success(const string& key)
{
    std::lock_guard<std::mutex> lk(g_rl_mutex);
    g_rl.erase(key);
}

// ---- Base32 (RFC4648) ----
string base32_encode(const unsigned char* data, size_t len)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    string out; int buf = 0, bits = 0;
    for (size_t i = 0; i < len; ++i) {
        buf = (buf << 8) | data[i]; bits += 8;
        while (bits >= 5) { out += A[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) out += A[(buf << (5 - bits)) & 31];
    return out;
}
bool base32_decode(const string& in, std::vector<unsigned char>& out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= '2' && c <= '7') return c - '2' + 26;
        return -1;
    };
    int buf = 0, bits = 0; out.clear();
    for (char c : in) {
        if (c == '=' || c == ' ') continue;
        int v = val(c); if (v < 0) return false;
        buf = (buf << 5) | v; bits += 5;
        if (bits >= 8) { out.push_back((unsigned char)((buf >> (bits - 8)) & 0xff)); bits -= 8; }
    }
    return true;
}

// ---- TOTP (RFC6238, SHA1, 6 位, 30 秒) ----
unsigned totp_at(const std::vector<unsigned char>& key, uint64_t counter)
{
    unsigned char msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = (unsigned char)(counter & 0xff); counter >>= 8; }
    unsigned char mac[EVP_MAX_MD_SIZE]; unsigned int maclen = 0;
    HMAC(EVP_sha1(), key.data(), (int)key.size(), msg, 8, mac, &maclen);
    int off = mac[maclen - 1] & 0x0f;
    unsigned bin = ((mac[off] & 0x7f) << 24) | (mac[off + 1] << 16) | (mac[off + 2] << 8) | (mac[off + 3]);
    return bin % 1000000u;
}
bool totp_verify(const string& secretB32, const string& codeStr)
{
    if (codeStr.size() != 6) { for (char c : codeStr) if (!isdigit((unsigned char)c)) return false; if (codeStr.empty()) return false; }
    std::vector<unsigned char> key;
    if (!base32_decode(secretB32, key) || key.empty()) return false;
    unsigned code;
    try { code = (unsigned)std::stoul(codeStr); } catch (...) { return false; }
    uint64_t t = (uint64_t)(time(nullptr) / 30);
    for (int w = -1; w <= 1; ++w) {   // 容忍 ±1 时间窗
        if (totp_at(key, t + w) == code) return true;
    }
    return false;
}

// 管理员守卫: 校验当前用户 role==1 后执行 onOk
void guard_admin(SeriesWork* series, const User& user, HttpResp* resp, std::function<void(SeriesWork*)> onOk)
{
    string sql = "SELECT role FROM tbl_user WHERE id=" + std::to_string(user.id) + " AND tomb=0";
    push_mysql(series, sql, [resp, onOk](WFMySQLTask* t) {
        if (!mysql_ok(t)) { api::fail(resp, 500, 500, "权限校验失败"); return; }
        MySQLResultCursor c{ t->get_resp() };
        std::vector<MySQLCell> row;
        if (!c.fetch_row(row) || cell_ll(row[0]) != 1) { api::fail(resp, 403, 403, "需要管理员权限"); return; }
        onOk(series_of(t));
    });
}

} // namespace

// =============================================================================
void CloudiskServer::register_modules()
{
    // 缓存配置
    Config& c = Config::instance();
    g_mysql_url     = c.mysql_url();
    g_rabbitmq_url  = c.rabbitmq_url();
    g_consul_url    = c.consul_url();
    g_storage_dir   = c.storage_dir();
    g_blob_dir      = g_storage_dir + "/blobs";
    g_thumb_dir     = g_storage_dir + "/thumbs";
    g_upload_dir    = g_storage_dir + "/uploads";
    g_max_file_size = c.max_file_size();
    g_user_quota    = c.user_quota();
    mkdir_p(g_blob_dir);
    mkdir_p(g_thumb_dir);
    mkdir_p(g_upload_dir);
    mkdir_p(g_storage_dir + "/avatars");

    register_static_resources_module();
    register_signup_module();
    register_signin_module();
    register_userinfo_module();
    register_fileupload_module();
    register_filelist_module();
    register_filedownload_module();
    register_folder_module();
    register_trash_module();
    register_share_module();
    register_preview_module();
    register_chunk_upload_module();
    register_offline_module();
    register_account_module();
    register_admin_module();
    register_metrics_module();
    register_token_module();
    register_webdav_module();
    register_favorite_module();
    register_search_module();
    register_version_module();
    register_stats_module();
    register_tag_module();
    register_activity_module();
}

// ----- 静态资源与页面 ----------------------------------------------------------
void CloudiskServer::register_static_resources_module()
{
    // 前端整套资源
    m_server.Static("/static", "static");

    // 页面入口
    m_server.GET("/", [](const HttpReq*, HttpResp* resp) {
        resp->File("static/index.html");
    });
    m_server.GET("/app", [](const HttpReq*, HttpResp* resp) {
        resp->File("static/app.html");
    });
    m_server.GET("/share.html", [](const HttpReq*, HttpResp* resp) {
        resp->File("static/share.html");
    });
    // 健康检查
    m_server.GET("/healthz", [](const HttpReq*, HttpResp* resp) {
        api::ok(resp, {{"status", "up"}});
    });
}

// ----- 注册 --------------------------------------------------------------------
void CloudiskServer::register_signup_module()
{
    m_server.POST("/api/auth/register", [](const HttpReq* req, HttpResp* resp) {
        nlohmann::json in;
        if (!parse_body(req, in)) { api::fail(resp, 400, 400, "请求体格式错误"); return; }
        string username = json_str(in, "username");
        string password = json_str(in, "password");

        if (!SqlUtil::valid_username(username)) { api::fail(resp, 400, 400, "用户名需为 3-32 位字母/数字/下划线/中划线"); return; }
        if (!SqlUtil::valid_password(password)) { api::fail(resp, 400, 400, "密码长度需为 6-64 位"); return; }

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
            audit_log(0, username, "register", "", client_ip(req));
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
            audit_log(0, username, "login_blocked", "剩余冷却 " + std::to_string(wait) + "s", ip);
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
                audit_log(0, username, "login_fail", why, ip);
                api::fail(resp, 401, 401, "用户名或密码错误");
            };
            if (!cur.fetch_row(row)) { failLogin("no such user"); return; }
            User u;
            u.id = (int)cell_ll(row[0]); u.username = row[1].as_string();
            string hash = row[2].as_string(); u.salt = row[3].as_string();
            u.createdAt = row[4].is_string() ? row[4].as_string() : string();
            int disabled = (int)cell_ll(row[5]); int totpEnabled = (int)cell_ll(row[6]);
            string totpSecret = row[7].as_string(); int role = (int)cell_ll(row[8]);

            if (disabled) { audit_log(u.id, username, "login_disabled", "", ip); api::fail(resp, 403, 403, "账户已被禁用"); return; }
            if (CryptoUtil::hash_password(password, u.salt) != hash) { failLogin("bad password"); return; }
            if (totpEnabled) {
                if (code.empty()) { api::fail(resp, 401, 4012, "需要两步验证码"); return; }
                if (!totp_verify(totpSecret, code)) { rl_on_fail(rlkey); audit_log(u.id, username, "login_2fa_fail", "", ip); api::fail(resp, 401, 4012, "两步验证码错误"); return; }
            }
            rl_on_success(rlkey);
            g_metrics.logins++;
            string token = CryptoUtil::generate_token(u);
            audit_log(u.id, username, "login", "", ip);
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
                    + std::to_string(quota > used ? quota - used : 0)
                    + " 字节, 本次需 " + std::to_string(incoming) + " 字节");
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
                audit_log(uid, username, "file_upload",
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
        if (filename.empty() || !SqlUtil::valid_hash(hash)) { api::fail(resp, 400, 400, "参数不合法"); return; }

        if (!file_exists(g_blob_dir + "/" + hash)) {
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
                    + std::to_string(quota > used ? quota - used : 0) + " 字节");
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
            audit_log(ruid, uname, "file_rename", "重命名为 " + newname, cip);
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
            string blob  = g_blob_dir + "/" + row[1].as_string();
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
        int uid = user.id;
        bool blobExists = file_exists(g_blob_dir + "/" + hash);

        // 配额校验(以完整大小计)
        string usageSql = usage_quota_sql(uid);
        push_mysql(series, usageSql,
            [resp, uid, filename, hash, size, parentId, chunkSize, totalChunks, blobExists](WFMySQLTask* task) {
            long long used = 0, quota = g_user_quota;
            if (mysql_ok(task)) { MySQLResultCursor cur{ task->get_resp() }; std::vector<MySQLCell> row; if (cur.fetch_row(row)) { quota = cell_ll(row[0]); used = (long long)row[1].as_ulonglong(); } }
            if (used + size > quota) {
                api::fail(resp, 413, 413, "存储空间不足: 剩余 " + std::to_string(quota > used ? quota - used : 0) + " 字节"); return;
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
            // 查找可复用的进行中会话(断点续传), 否则新建
            string q = "SELECT upload_id FROM tbl_upload WHERE status=0 AND uid=" + std::to_string(uid)
                     + " AND hashcode=" + SqlUtil::quote(hash) + " AND parent_id=" + std::to_string(parentId)
                     + " ORDER BY id DESC LIMIT 1";
            push_mysql(s, q, [resp, uid, filename, hash, size, parentId, chunkSize, totalChunks](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "会话查询失败"); return; }
                MySQLResultCursor cur{ t2->get_resp() };
                std::vector<MySQLCell> row;
                string uploadId;
                if (cur.fetch_row(row)) uploadId = row[0].as_string();
                SeriesWork* s2 = series_of(t2);
                auto reply = [resp, chunkSize, totalChunks](const string& uid_) {
                    string dir = g_upload_dir + "/" + uid_;
                    auto have = list_uploaded_chunks(dir);
                    nlohmann::json arr = nlohmann::json::array();
                    for (int idx : have) arr.push_back(idx);
                    api::ok(resp, {{"instant", false}, {"uploadId", uid_}, {"uploaded", arr},
                                   {"chunkSize", chunkSize}, {"totalChunks", totalChunks}}, "就绪");
                };
                if (!uploadId.empty()) { mkdir_p(g_upload_dir + "/" + uploadId); reply(uploadId); return; }
                string newId = gen_token(16);
                mkdir_p(g_upload_dir + "/" + newId);
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
        string q = "SELECT hashcode, filename, size, parent_id, total_chunks FROM tbl_upload WHERE status=0 AND uid="
                 + std::to_string(uid) + " AND upload_id=" + SqlUtil::quote(uploadId);
        push_mysql(series, q, [resp, uid, uploadId](WFMySQLTask* task) {
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

            // 顺序拼接所有分片
            string content;
            content.reserve(size > 0 ? (size_t)size : 0);
            for (int i = 0; i < total; ++i) {
                string part = dir + "/" + std::to_string(i) + ".part";
                string buf;
                if (!read_file_all(part, buf)) { api::fail(resp, 400, 400, "分片缺失: " + std::to_string(i)); return; }
                content += buf;
            }
            // 校验内容 hash 与声明一致
            string real = CryptoUtil::generate_hashcode(content.c_str(), content.size());
            if (real != hash) { api::fail(resp, 400, 400, "文件校验失败(hash 不一致)"); return; }

            SeriesWork* s = series_of(task);
            string usageSql = usage_quota_sql(uid);
            // 用 shared_ptr 移交大内容, 避免多层 lambda 复制
            auto contentPtr = std::make_shared<string>(std::move(content));
            push_mysql(s, usageSql, [resp, uid, filename, hash, size, parentId, uploadId, dir, contentPtr](WFMySQLTask* t2) {
                long long used = 0, quota = g_user_quota;
                if (mysql_ok(t2)) { MySQLResultCursor c{ t2->get_resp() }; std::vector<MySQLCell> r; if (c.fetch_row(r)) { quota = cell_ll(r[0]); used = (long long)r[1].as_ulonglong(); } }
                if (used + (long long)contentPtr->size() > quota) {
                    api::fail(resp, 413, 413, "存储空间不足"); return;
                }
                bool isNew = write_blob_if_absent(hash, *contentPtr);
                if (isNew) publish_oss_backup(hash);
                long long realSize = (long long)contentPtr->size();
                SeriesWork* s2 = series_of(t2);
                string ins = "REPLACE INTO tbl_file (uid, parent_id, filename, hashcode, size) VALUES ("
                    + std::to_string(uid) + ", " + std::to_string(parentId) + ", " + SqlUtil::quote(filename) + ", "
                    + SqlUtil::quote(hash) + ", " + std::to_string(realSize) + ")";
                push_mysql(s2, ins, [resp, uploadId, dir, filename, hash, realSize, isNew](WFMySQLTask* t3) {
                    if (!mysql_ok(t3)) { api::fail(resp, 500, 500, "写入元数据失败"); return; }
                    SeriesWork* s3 = series_of(t3);
                    string upd = "UPDATE tbl_upload SET status=1 WHERE upload_id=" + SqlUtil::quote(uploadId);
                    push_mysql(s3, upd, [resp, dir, filename, hash, realSize, isNew](WFMySQLTask*) {
                        remove_upload_dir(dir);
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
                                string sql = "UPDATE tbl_offline SET status=2, message='存储空间不足' WHERE id=" + std::to_string(offlineId);
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
            string blob  = g_blob_dir + "/" + row[1].as_string();
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
            string blob  = g_blob_dir + "/" + hash;
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
                    audit_log(uid, cuname, "folder_create", "新建文件夹 " + name, cip);
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
            audit_log(uid, uname, "profile_update", "", ip);
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
        if (!SqlUtil::valid_password(newPw)) { api::fail(resp, 400, 400, "新密码长度需为 6-64 位"); return; }
        int uid = user.id; string uname = user.username, ip = client_ip(req);
        string sel = "SELECT password, salt FROM tbl_user WHERE id=" + std::to_string(uid);
        push_mysql(series, sel, [resp, oldPw, newPw, uid, uname, ip](WFMySQLTask* t) {
            if (!mysql_ok(t)) { api::fail(resp, 500, 500, "校验失败"); return; }
            MySQLResultCursor c{ t->get_resp() };
            std::vector<MySQLCell> row;
            if (!c.fetch_row(row)) { api::fail(resp, 404, 404, "用户不存在"); return; }
            string hash = row[0].as_string(), salt = row[1].as_string();
            if (CryptoUtil::hash_password(oldPw, salt) != hash) {
                audit_log(uid, uname, "password_change_fail", "", ip);
                api::fail(resp, 401, 401, "原密码错误"); return;
            }
            string newSalt = CryptoUtil::generate_salt();
            string newHash = CryptoUtil::hash_password(newPw, newSalt);
            string upd = "UPDATE tbl_user SET password=" + SqlUtil::quote(newHash) + ", salt=" + SqlUtil::quote(newSalt)
                       + " WHERE id=" + std::to_string(uid);
            push_mysql(series_of(t), upd, [resp, uid, uname, ip](WFMySQLTask* t2) {
                if (!mysql_ok(t2)) { api::fail(resp, 500, 500, "更新失败"); return; }
                audit_log(uid, uname, "password_change", "", ip);
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
            audit_log(uid, uname, "avatar_update", "", ip);
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
                audit_log(uid, uname, "2fa_enable", "", ip);
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
                audit_log(uid, uname, "2fa_disable", "", ip);
                api::ok(resp, {{"twoFactor", false}}, "两步验证已关闭");
            });
        });
    });
}

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
            audit_log(uid, uname, "token_create", "name=" + name, ip);
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
                audit_log(uid, uname, "token_revoke", "id=" + std::to_string(id), ip);
                api::ok(resp, {{"id", id}}, "令牌已吊销");
            });
        });
    });
}

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
            std::string blob = g_blob_dir + "/" + row[1].as_string();
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
