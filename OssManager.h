#pragma once
#include <alibabacloud/oss/OssClient.h>
#include <mutex>
#include <memory>

class OssManager
{
public:
    static OssManager* get_instance();
    static void destroy_instance();

    // 方法的重载
    bool upload_object(const std::string& bucket, const std::string& object, const std::string& file);
    bool upload_object(const std::string& bucket, const std::string& object, std::shared_ptr<std::iostream> content);

    ~OssManager() {}

private:
    // 私有构造函数，防止外部实例化
    OssManager();

    // 禁止拷贝构造和赋值操作
    OssManager(const OssManager&) = delete;
    OssManager& operator=(const OssManager&) = delete;

private:
    static OssManager* instance;
    static std::mutex m_mutex;

    std::unique_ptr<AlibabaCloud::OSS::OssClient> m_client;
};