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
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <ctime>
#include <set>

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

// 与 blob 同目录的临时文件路径。同目录是硬要求: 只有同一文件系统上的 rename()
// 才是原子操作, 跨设备 rename 会返回 EXDEV 并退化成"复制 + 删除"(非原子且更慢)。
string blob_tmp_path(const string& tag)
{
    string dest = blob_path("x");                 // 只为取出 blob 根目录
    size_t slash = dest.find_last_of('/');
    string dir = (slash == string::npos) ? string(".") : dest.substr(0, slash);
    return dir + "/.tmp-" + tag + "-" + std::to_string((long long)time(nullptr))
         + "-" + std::to_string((long long)getpid());
}

bool merge_chunks_streaming(const string& dir, int totalChunks, const string& dest,
                            string& outHash, long long& outSize, string& err)
{
    outHash.clear();
    outSize = 0;
    err.clear();
    if (totalChunks <= 0) { err = "分片数非法"; return false; }

    int out = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { err = "无法创建临时文件: " + string(strerror(errno)); return false; }

    // 每次只持有一块 (上限 4 MiB), 内存占用与文件总大小无关。
    // 注意: 不信任分片的实际大小 —— 客户端可以上传任意大的 .part,
    // 因此按固定块循环读取, 而不是一次性 read_file_all 整个分片。
    static const size_t BLOCK = 4u * 1024 * 1024;
    std::vector<char> buf(BLOCK);
    Sha256Stream hasher;
    long long total = 0;
    bool ok = true;

    for (int i = 0; i < totalChunks && ok; ++i) {
        string part = dir + "/" + std::to_string(i) + ".part";
        int in = open(part.c_str(), O_RDONLY);
        if (in < 0) { err = "分片缺失: " + std::to_string(i); ok = false; break; }
        for (;;) {
            ssize_t r = read(in, buf.data(), BLOCK);
            if (r < 0) {
                if (errno == EINTR) continue;
                err = "读取分片 " + std::to_string(i) + " 失败: " + string(strerror(errno));
                ok = false; break;
            }
            if (r == 0) break;                       // 该分片读完
            // 全量写出 (write 可能短写)
            ssize_t off = 0;
            while (off < r) {
                ssize_t w = write(out, buf.data() + off, (size_t)(r - off));
                if (w <= 0) {
                    if (w < 0 && errno == EINTR) continue;
                    err = "写入合并文件失败: " + string(strerror(errno));
                    ok = false; break;
                }
                off += w;
            }
            if (!ok) break;
            hasher.update(buf.data(), (size_t)r);
            total += r;
        }
        close(in);
    }

    // 确保数据真正落盘, 之后 rename 才对读者可见
    if (ok && fsync(out) != 0) { err = "同步合并文件失败: " + string(strerror(errno)); ok = false; }
    close(out);

    if (!ok) { remove(dest.c_str()); return false; }
    outHash = hasher.final_hex();
    outSize = total;
    return true;
}

bool adopt_blob_file(const string& hash, const string& src, string& err)
{
    err.clear();
    const string dest = blob_path(hash);
    if (access(dest.c_str(), F_OK) == 0) {          // 已存在 -> 去重命中
        remove(src.c_str());
        return false;
    }
    if (rename(src.c_str(), dest.c_str()) == 0) return true;

    // rename 失败 (典型为 EXDEV 跨设备, 或 blob 目录不存在): 退化为流式复制,
    // 仍然不把文件读进内存。
    int in = open(src.c_str(), O_RDONLY);
    if (in < 0) { err = "打开临时文件失败: " + string(strerror(errno)); return false; }
    int out = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        err = "创建 blob 失败: " + string(strerror(errno));
        close(in); remove(src.c_str()); return false;
    }
    static const size_t BLOCK = 4u * 1024 * 1024;
    std::vector<char> buf(BLOCK);
    bool ok = true;
    for (;;) {
        ssize_t r = read(in, buf.data(), BLOCK);
        if (r < 0) { if (errno == EINTR) continue; err = "复制读取失败: " + string(strerror(errno)); ok = false; break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf.data() + off, (size_t)(r - off));
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; err = "复制写入失败: " + string(strerror(errno)); ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    close(in); close(out);
    remove(src.c_str());
    if (!ok) { remove(dest.c_str()); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 滞留上传会话回收 (GC)
// ---------------------------------------------------------------------------
static int g_upload_ttl_hours = 24;

// 清扫"有目录、无会话"的孤儿分片目录。
// 只按 tbl_upload 的 TTL 清是不够的: 一旦会话行被删 (人工清理、异常中断后的
// 补偿删除) 而目录还在, 就再没有任何路径能发现它 —— 实测就留了一个这样的目录。
// 做法: 一次取出全部在册 upload_id 作为集合, 再对照目录列表, 集合外且足够旧的即删。
static void purge_orphan_upload_dirs()
{
    const int ttl = g_upload_ttl_hours;
    WFMySQLTask* t = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [ttl](WFMySQLTask* task) {
        if (task->get_state() != WFT_STATE_SUCCESS) return;
        std::set<string> known;
        try {
            MySQLResultCursor cur{ task->get_resp() };
            std::vector<MySQLCell> row;
            while (cur.fetch_row(row)) { if (!row.empty()) known.insert(row[0].as_string()); }
        } catch (...) { return; }

        DIR* dp = opendir(g_upload_dir.c_str());
        if (!dp) return;
        const time_t cutoff = time(nullptr) - (time_t)ttl * 3600;
        int removed = 0;
        struct dirent* ent;
        while ((ent = readdir(dp)) != nullptr) {
            string name = ent->d_name;
            if (name == "." || name == "..") continue;
            if (known.count(name)) continue;              // 在册会话, 交给 TTL 逻辑处理
            string full = g_upload_dir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            // 仅删足够旧的: 避免与"刚 mkdir 还没插入会话行"的并发上传抢跑。
            if (st.st_mtime > cutoff) continue;
            remove_upload_dir(full);
            ++removed;
        }
        closedir(dp);
        if (removed > 0) LOG_INFO("孤儿分片目录清扫: 删除 " << removed << " 个无会话归属的目录");
    });
    t->get_req()->set_query("SELECT upload_id FROM tbl_upload");
    t->start();
}

void purge_stale_uploads()
{
    purge_orphan_upload_dirs();               // 先清无归属的目录, 再清超时会话
    const int ttl = g_upload_ttl_hours;
    string q = "SELECT upload_id FROM tbl_upload WHERE status=0 AND created_at < (NOW() - INTERVAL "
             + std::to_string(ttl) + " HOUR)";
    WFMySQLTask* t = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [ttl](WFMySQLTask* task) {
        if (task->get_state() != WFT_STATE_SUCCESS) {
            LOG_WARN("滞留上传清扫: 查询失败, 本轮跳过");
            return;
        }
        std::vector<string> ids;
        try {
            MySQLResultCursor cur{ task->get_resp() };
            std::vector<MySQLCell> row;
            while (cur.fetch_row(row)) { if (!row.empty()) ids.push_back(row[0].as_string()); }
        } catch (...) { }
        if (ids.empty()) return;

        // 先删分片目录, 再删记录: 顺序反了会在崩溃时留下永远查不到的孤儿目录。
        for (const string& id : ids) remove_upload_dir(g_upload_dir + "/" + id);
        string del = "DELETE FROM tbl_upload WHERE status=0 AND created_at < (NOW() - INTERVAL "
                   + std::to_string(ttl) + " HOUR)";
        WFMySQLTask* d = WFTaskFactory::create_mysql_task(g_mysql_url, 1, [n = ids.size(), ttl](WFMySQLTask*) {
            LOG_INFO("滞留上传清扫: 回收 " << n << " 个超过 " << ttl << " 小时的未完成会话及其分片目录");
        });
        d->get_req()->set_query(del);
        d->start();
    });
    t->get_req()->set_query(q);
    t->start();
}

// 周期回调不能带捕获, 因此用一对普通函数互相调用形成自续期循环。
static void schedule_upload_gc();
static void upload_gc_tick(WFTimerTask* t)
{
    // 定时器失败 (例如进程正在退出) 就不再续期, 避免空转。
    if (t->get_state() == WFT_STATE_SUCCESS) { purge_stale_uploads(); schedule_upload_gc(); }
}
static void schedule_upload_gc()
{
    WFTaskFactory::create_timer_task(3600, 0, upload_gc_tick)->start();   // 每小时一次
}

void start_upload_gc(int ttlHours)
{
    g_upload_ttl_hours = ttlHours > 0 ? ttlHours : 24;
    LOG_INFO("分片上传会话回收已启用: TTL " << g_upload_ttl_hours << " 小时, 每小时清扫一次");
    purge_stale_uploads();       // 启动即清一次, 回收历史遗留
    schedule_upload_gc();
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
// 兜底路径同样不能用 rand(): 未播种的 rand() 每次进程启动序列相同, 会让分享
// token / 提取码变得可猜测 —— 那等于任何人都能枚举出他人的分享链接。
string gen_token(int n)
{
    static const char* alpha = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (n <= 0) return string();
    const size_t want = static_cast<size_t>(n);
    string out;
    out.reserve(want);

    std::vector<unsigned char> buf(want);
    if (RAND_bytes(buf.data(), buf.size()) == 1) {
        // 拒绝采样消除 256 % 62 的取模偏置 (只接受 < 248 = 62*4 的字节)
        size_t i = 0;
        while (out.size() < want) {
            if (i >= buf.size()) {
                if (RAND_bytes(buf.data(), buf.size()) != 1) break;
                i = 0;
            }
            const unsigned char b = buf[i++];
            if (b < 248) out += alpha[b % 62];
        }
    }
    if (out.size() < want) {
        std::random_device rd;
        std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()));
        std::uniform_int_distribution<int> dist(0, 61);
        while (out.size() < want) out += alpha[dist(gen)];
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
