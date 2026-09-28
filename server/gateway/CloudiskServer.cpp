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

// 共享支撑层: 运行期配置 + 通用工具 (定义见 GatewaySupport.cpp)
#include "GatewaySupport.h"

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

    // 装配 blob 存储子系统 (Abstract Factory: 依配置选择具体后端)
    init_blob_store(g_blob_dir);
    init_blob_backup(g_rabbitmq_url);

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
    // 前端整套资源 (URL 前缀 /static 保持不变, 磁盘根目录为 web/)
    m_server.Static("/static", "web");

    // 页面入口
    m_server.GET("/", [](const HttpReq*, HttpResp* resp) {
        resp->File("web/index.html");
    });
    m_server.GET("/app", [](const HttpReq*, HttpResp* resp) {
        resp->File("web/app.html");
    });
    m_server.GET("/share.html", [](const HttpReq*, HttpResp* resp) {
        resp->File("web/share.html");
    });
    // 健康检查
    m_server.GET("/healthz", [](const HttpReq*, HttpResp* resp) {
        api::ok(resp, {{"status", "up"}});
    });
}
