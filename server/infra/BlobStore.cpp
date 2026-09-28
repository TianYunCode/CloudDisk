// =============================================================================
// BlobStore.cpp —— 内容寻址 Blob 存储子系统的实现（单一真源）
// =============================================================================
#include "BlobStore.h"

#include <memory>
#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <SimpleAmqpClient/SimpleAmqpClient.h>

#include "Log.h"

using namespace AmqpClient;

// ----- LocalBlobStore --------------------------------------------------------
LocalBlobStore::LocalBlobStore(std::string root) : root_(std::move(root)) {}

std::string LocalBlobStore::path_of(const std::string& hash) const
{
    return root_ + "/" + hash;
}

bool LocalBlobStore::exists(const std::string& hash) const
{
    return access(path_of(hash).c_str(), F_OK) == 0;
}

bool LocalBlobStore::put_if_absent(const std::string& hash, const std::string& content)
{
    std::string path = path_of(hash);
    if (access(path.c_str(), F_OK) == 0) return false;   // 已存在 -> 去重命中
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

// ----- RabbitMqOssBackup -----------------------------------------------------
RabbitMqOssBackup::RabbitMqOssBackup(std::string amqp_url, const IBlobStore* store)
    : amqp_url_(std::move(amqp_url)), store_(store) {}

void RabbitMqOssBackup::on_new_blob(const std::string& hash)
{
    try {
        Channel::ptr_t channel = Channel::CreateFromUri(amqp_url_);
        nlohmann::json obj;
        obj["object"] = hash;                                 // OSS 对象键 = 内容哈希
        obj["file"]   = store_ ? store_->path_of(hash) : hash; // 本地 blob 路径
        BasicMessage::ptr_t msg = BasicMessage::Create(obj.dump());
        channel->BasicPublish("oss.direct", "oss", msg);
    } catch (const std::exception& e) {
        LOG_WARN("RabbitMQ 备份投递失败(忽略): " << e.what());
    }
}

// ----- Abstract Factory + Singleton ------------------------------------------
namespace {
std::unique_ptr<IBlobStore>  g_store;
std::unique_ptr<IBlobBackup> g_backup;
}

void init_blob_store(const std::string& root)
{
    g_store = std::make_unique<LocalBlobStore>(root);
}

void init_blob_backup(const std::string& amqp_url)
{
    if (amqp_url.empty())
        g_backup = std::make_unique<NullBlobBackup>();
    else
        g_backup = std::make_unique<RabbitMqOssBackup>(amqp_url, g_store.get());
}

IBlobStore& blob_store()
{
    // 兜底: 若组合根未显式初始化 (理论上不会发生), 退化为当前目录, 保证不空引用
    if (!g_store) g_store = std::make_unique<LocalBlobStore>(".");
    return *g_store;
}

IBlobBackup& blob_backup()
{
    if (!g_backup) g_backup = std::make_unique<NullBlobBackup>();
    return *g_backup;
}
