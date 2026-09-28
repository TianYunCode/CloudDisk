#include "OssManager.h"
#include <cstdlib>
#include <iostream>

using namespace std;

// 从环境变量读取配置; 缺失时给出提示并返回默认值(空)
static string env_or(const char* name, const char* fallback = "")
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        std::cerr << "[OSS] 环境变量 " << name << " 未设置" << std::endl;
        return fallback;
    }
    return string(v);
}

// 初始化静态成员变量
OssManager* OssManager::instance = nullptr;
mutex OssManager::m_mutex;

OssManager* OssManager::get_instance()
{   
    // 第一次检查：如果实例已经存在，直接返回
    if (instance == nullptr)
    {
        lock_guard<mutex> lock(m_mutex);
        // 第二次检查：防止在多线程环境下重复创建实例
        if (instance == nullptr)
        {
            instance = new OssManager();
        }
    }
    return instance;
}

void OssManager::destroy_instance() 
{
    lock_guard<mutex> lock(m_mutex);
    if (instance != nullptr) 
    {
        delete instance;
        instance = nullptr;
        AlibabaCloud::OSS::ShutdownSdk(); // 释放SDK资源
    }
}

OssManager::OssManager()
{
    // 1. 初始化SDK
    AlibabaCloud::OSS::InitializeSdk(); 

    // 从环境变量读取 OSS 配置 (不要把密钥写进源码/提交到 git)
    //   OSS_ENDPOINT           例: oss-cn-hangzhou.aliyuncs.com
    //   OSS_REGION             例: cn-hangzhou
    //   OSS_ACCESS_KEY_ID      你的 AccessKey ID
    //   OSS_ACCESS_KEY_SECRET  你的 AccessKey Secret
    string endpoint        = env_or("OSS_ENDPOINT");
    string region          = env_or("OSS_REGION");
    string accessKeyId     = env_or("OSS_ACCESS_KEY_ID");
    string accessKeySecret = env_or("OSS_ACCESS_KEY_SECRET");
    AlibabaCloud::OSS::ClientConfiguration conf;
    m_client = std::make_unique<AlibabaCloud::OSS::OssClient>(endpoint, accessKeyId, accessKeySecret, conf);
            
    m_client->SetRegion(region);
}

bool OssManager::upload_object(const string& bucket, const string& object, const string& file) // 上传文件
{
    auto outcome = m_client->PutObject(bucket, object, file);
    return outcome.isSuccess(); 
}

bool OssManager::upload_object(const string& bucket, const string& object, std::shared_ptr<std::iostream> content)
{
    auto outcome = m_client->PutObject(bucket, object, content);
    return outcome.isSuccess();
}