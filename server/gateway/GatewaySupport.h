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
void publish_oss_backup(const std::string& hash);
bool discover_userservice(std::string& ip, unsigned short& port);

// ----- 请求 / JSON / SQL 工具 -------------------------------------------------
bool parse_body(const wfrest::HttpReq* req, nlohmann::json& out);
std::string json_str(const nlohmann::json& j, const char* key);
WFMySQLTask* push_mysql(SeriesWork* series, const std::string& sql, mysql_callback_t cb);
bool mysql_ok(WFMySQLTask* task);
bool ids_from_json(const nlohmann::json& j, const char* key, std::vector<long long>& out);
std::string ids_csv(const std::vector<long long>& v);
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
