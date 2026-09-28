// =============================================================================
// BlobStore.h —— 内容寻址 Blob 存储子系统（设计模式落位）
//
//   · Strategy / Bridge : IBlobStore / IBlobBackup 抽象与具体实现分离，
//                         上层只依赖接口，实现（本地 FS / RabbitMQ-OSS）可替换。
//   · Abstract Factory  : init_* 依据运行期配置装配一族具体实现。
//   · Singleton         : blob_store() / blob_backup() 提供进程级唯一访问点。
//   · Flyweight         : 相同内容哈希共享同一物理对象（去重秒传）。
//
// 该抽象把原先散落在各 handler 中的 "g_blob_dir + '/' + hash" 路径拼接、去重
// 写入与 OSS 备份投递统一收敛，达到高内聚、低耦合、可移植、可测试的目的。
// =============================================================================
#ifndef CLOUDDISK_BLOB_STORE_H
#define CLOUDDISK_BLOB_STORE_H

#include <string>

// ----- 存储后端抽象 (Strategy / Bridge) --------------------------------------
class IBlobStore {
public:
    virtual ~IBlobStore() = default;
    // 内容哈希 -> 物理路径
    virtual std::string path_of(const std::string& hash) const = 0;
    // 该 blob 是否已存在
    virtual bool exists(const std::string& hash) const = 0;
    // 去重写入: 已存在则跳过并返回 false; 新写入返回 true
    virtual bool put_if_absent(const std::string& hash, const std::string& content) = 0;
};

// 本地文件系统实现
class LocalBlobStore : public IBlobStore {
public:
    explicit LocalBlobStore(std::string root);
    std::string path_of(const std::string& hash) const override;
    bool exists(const std::string& hash) const override;
    bool put_if_absent(const std::string& hash, const std::string& content) override;
private:
    std::string root_;
};

// ----- 备份后端抽象 (Strategy / Bridge) --------------------------------------
class IBlobBackup {
public:
    virtual ~IBlobBackup() = default;
    // 新 blob 落地后触发的备份动作 (fire-and-forget, 不得抛出)
    virtual void on_new_blob(const std::string& hash) = 0;
};

// RabbitMQ -> OSS 备份实现
class RabbitMqOssBackup : public IBlobBackup {
public:
    RabbitMqOssBackup(std::string amqp_url, const IBlobStore* store);
    void on_new_blob(const std::string& hash) override;
private:
    std::string amqp_url_;
    const IBlobStore* store_;
};

// 空实现 (未配置消息队列时使用, 保证行为确定且不产生连接开销)
class NullBlobBackup : public IBlobBackup {
public:
    void on_new_blob(const std::string&) override {}
};

// ----- 组合根装配 + 进程级访问点 (Abstract Factory + Singleton) ---------------
void init_blob_store(const std::string& root);                 // 由 register_modules 调用一次
void init_blob_backup(const std::string& amqp_url);            // 由 register_modules 调用一次
IBlobStore&  blob_store();
IBlobBackup& blob_backup();

#endif // CLOUDDISK_BLOB_STORE_H
