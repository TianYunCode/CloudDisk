// =============================================================================
// GatewaySupport.cpp —— HTTP 网关共享支撑层的定义（单一真源）
// 全局配置、指标、令牌注册表与通用工具在此唯一定义。
// =============================================================================
#include "GatewaySupport.h"

#include <workflow/MySQLResult.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/MySQLUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/Workflow.h>
#include <workflow/HttpUtil.h>

#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cctype>
#include <ctime>

#include <SimpleAmqpClient/SimpleAmqpClient.h>

#include "Config.h"
#include "Log.h"
#include "SqlUtil.h"
#include "ApiResp.h"
#include "RateLimiter.h"

using namespace std;
using namespace wfrest;
using namespace protocol;
using namespace AmqpClient;

// ----- 运行期配置 (定义) -------------------------------------------------------
string  g_mysql_url;
string  g_rabbitmq_url;
string  g_consul_url;
string  g_storage_dir;
string  g_blob_dir;
string  g_thumb_dir;
string  g_upload_dir;
long long g_max_file_size = 0;
long long g_user_quota = 0;

// ----- 进程级单例对象 (定义) ---------------------------------------------------
Metrics g_metrics;
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

// ---- mime_of / is_thumbnailable: 已抽出到 server/util/FileType.{h,cpp} ----

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
// 委托给可插拔的 blob 存储后端 (Strategy/Bridge), 收敛存储细节。
bool write_blob_if_absent(const string& hash, const string& content)
{
    return blob_store().put_if_absent(hash, content);
}

// 内容哈希 -> 物理 blob 路径 (委托给存储后端)
string blob_path(const string& hash)
{
    return blob_store().path_of(hash);
}

// 异步备份: 新 blob 落地后交由备份后端处理 (失败不影响主流程)。
void publish_oss_backup(const string& hash)
{
    blob_backup().on_new_blob(hash);
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

// json_str / ids_from_json / ids_csv: 已抽出到 server/util/JsonUtil.{h,cpp}

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

// ids_from_json / ids_csv: 已抽出到 server/util/JsonUtil.{h,cpp}

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

// ---- 登录失败限速 (进程内, 按 用户名|IP 计数; 策略实现见 util/RateLimiter) ----
namespace {
RateLimiter g_login_rl(5, 300, 300);   // 窗口内最多 5 次失败, 300s 窗口, 300s 冷却
} // namespace

// 返回剩余冷却秒数(>0 表示被限流)
int rl_blocked(const string& key)
{
    return g_login_rl.blocked(key, time(nullptr));
}
void rl_on_fail(const string& key)
{
    g_login_rl.onFail(key, time(nullptr));
}
void rl_on_success(const string& key)
{
    g_login_rl.onSuccess(key);
}

// ---- Base32 / TOTP: 已抽出到 server/util/Totp.{h,cpp} (见 GatewaySupport.h 转发) ----

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
