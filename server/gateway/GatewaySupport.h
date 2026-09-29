// =============================================================================
// GatewaySupport.h —— HTTP 网关共享支撑层（运行期配置 + 通用工具）
//
// 本头把原先散落在 CloudiskServer.cpp 匿名命名空间中的运行期全局配置与通用
// 辅助函数抽取为一个独立的、可被多个翻译单元复用的支撑层，服务于「高内聚、
// 低耦合、可复用、可测试」的架构目标。全局对象以 extern 声明，仅在
// GatewaySupport.cpp 中定义一次（资源确定性 / 单一真源）。
// =============================================================================
#ifndef CLOUDDISK_GATEWAY_SUPPORT_H
#define CLOUDDISK_GATEWAY_SUPPORT_H

#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <random>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <chrono>

#include <nlohmann/json.hpp>
#include <wfrest/HttpServer.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/MySQLResult.h>

#include "CryptoUtil.h"
#include "User.h"
#include "BlobStore.h"
#include "AuditAction.h"   // 审计动作名常量 (前后端契约单一真源)

// ----- 运行期配置 (启动时由 register_modules 从 Config 读取一次) -----------------
extern std::string g_mysql_url;
extern std::string g_rabbitmq_url;
extern std::string g_consul_url;
extern std::string g_storage_dir;
extern std::string g_blob_dir;
extern std::string g_thumb_dir;
extern std::string g_upload_dir;
extern long long   g_max_file_size;
extern long long   g_user_quota;

constexpr int RETRY_MAX = 3;

// ----- Prometheus 指标计数器 (进程级, 线程安全) -------------------------------
struct Metrics {
    std::atomic<long long> uploads{0}, upload_bytes{0}, downloads{0}, download_bytes{0};
    std::atomic<long long> logins{0}, login_fails{0}, shares_created{0};
    std::atomic<long long> token_auth{0}, webdav_requests{0};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
extern Metrics g_metrics;

// ----- API Token 内存注册表 (支持 require_auth 同步解析) -----------------------
struct TokenRegistry {
    struct Ent { int uid; std::string username; std::string createdAt; long long expires; long long lastPersist; };
    std::mutex mu;
    std::unordered_map<std::string, TokenRegistry::Ent> map;   // key = token_hash
    static std::string hash_of(const std::string& tok) { return CryptoUtil::generate_hashcode(tok.data(), tok.size()); }
    void put(const std::string& hash, int uid, const std::string& un, const std::string& ca, long long exp) {
        std::lock_guard<std::mutex> l(mu); map[hash] = Ent{ uid, un, ca, exp, 0 };
    }
    void erase(const std::string& hash) { std::lock_guard<std::mutex> l(mu); map.erase(hash); }
    bool resolve(const std::string& tok, User& u, std::string& hashOut, bool& persist) {
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
extern TokenRegistry g_tokens;

// ----- 文件系统 / blob 工具 ---------------------------------------------------
void mkdir_p(const std::string& path);
bool file_exists(const std::string& p);
#include "FileType.h"   // mime_of / is_thumbnailable / ext_of (纯实现)
long long file_size_of(const std::string& p);
bool write_file_all(const std::string& path, const char* data, std::size_t n);
bool read_file_all(const std::string& path, std::string& out);
std::set<int> list_uploaded_chunks(const std::string& dir);
void remove_upload_dir(const std::string& dir);
bool write_blob_if_absent(const std::string& hash, const std::string& content);
std::string blob_path(const std::string& hash);   // = blob_store().path_of(hash)

// ----- 大文件流式合并 (不把整文件读进内存) -----------------------------------
// 背景: 分片合并原先把所有 .part 拼进一个 std::string 再整体哈希, 峰值内存 =
// 文件大小。单文件上限提到 10 GiB 后, 一次合并就要 10 GiB 内存, 会 OOM 拖垮整个
// 服务进程 (影响所有用户)。以下两个函数改为"边读边写边算": 每次只持有一个分片。

// 与 blob 同目录的临时文件路径 (同目录才能保证后续 rename 是原子操作)。
std::string blob_tmp_path(const std::string& tag);

// 流式合并 dir 下的 0.part .. (totalChunks-1).part 到 dest, 同时增量计算 SHA-256。
// 成功: 返回 true, outHash = 64 位十六进制摘要, outSize = 实际合并字节数。
// 失败: 返回 false, err 为原因, 并已删除 dest 残留。
bool merge_chunks_streaming(const std::string& dir, int totalChunks,
                            const std::string& dest,
                            std::string& outHash, long long& outSize,
                            std::string& err);

// 把已写好的 src 文件原子纳入 blob 存储 (rename)。
// 若同哈希 blob 已存在: 删除 src 并返回 false (去重命中); 新纳入返回 true。
bool adopt_blob_file(const std::string& hash, const std::string& src, std::string& err);

// ----- 滞留上传会话回收 (GC) --------------------------------------------------
// 中断的分片上传会永久留下两样垃圾: tbl_upload 里 status=0 的行, 以及
// storage/uploads/<uploadId>/ 下的全部分片。二者都不会被任何路径清理 ——
// 实测已积累 137 MiB 孤儿分片 (含一个目标目录早已被删进回收站的会话)。
// TTL 到期即回收; 仍在有效期内的会话保持可续传。
void purge_stale_uploads();               // 立即清扫一次 (异步, fire-and-forget)
void start_upload_gc(int ttlHours);       // 启动时清扫一次, 之后每小时清扫

void publish_oss_backup(const std::string& hash);
bool discover_userservice(std::string& ip, unsigned short& port);

// ----- 请求 / JSON / SQL 工具 -------------------------------------------------
#include "JsonUtil.h"   // json_str / ids_from_json / ids_csv (纯实现)
bool parse_body(const wfrest::HttpReq* req, nlohmann::json& out);
WFMySQLTask* push_mysql(SeriesWork* series, const std::string& sql, mysql_callback_t cb);
bool mysql_ok(WFMySQLTask* task);
std::string gen_token(int n);
long long cell_ll(const protocol::MySQLCell& c);
std::string usage_quota_sql(long long uid);
std::string client_ip(const wfrest::HttpReq* req);

// ----- 审计 / 限速 ------------------------------------------------------------
void audit_log(long long uid, const std::string& username, const std::string& action,
               const std::string& detail, const std::string& ip);
int  rl_blocked(const std::string& key);       // 返回剩余冷却秒数 (>0 表示被限流)
void rl_on_fail(const std::string& key);
void rl_on_success(const std::string& key);

// ----- Base32 / TOTP (两步验证) —— 纯算法, 见 server/util/Totp.h -----------------
#include "Totp.h"

// ----- 管理员守卫 -------------------------------------------------------------
void guard_admin(SeriesWork* series, const User& user, wfrest::HttpResp* resp,
                 std::function<void(SeriesWork*)> onOk);

#endif // CLOUDDISK_GATEWAY_SUPPORT_H
