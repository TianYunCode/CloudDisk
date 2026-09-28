#include <iostream>
#include <string>
#include <cstdlib>
#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>
#include "OssManager.h"
#include "Config.h"

using namespace std;
using namespace AmqpClient;

int main()
{
    Config& cfg = Config::instance();
    string uri = cfg.rabbitmq_url();
    string bucket = cfg.oss_bucket();
    const string& q = "oss.queue";

    if (bucket.empty()) {
        cerr << "Error: 未配置 OSS_BUCKET (环境变量或 config.json), 无法备份到 OSS" << endl;
        return 1;
    }

    Channel::ptr_t channel = Channel::CreateFromUri(uri);

    // 绑定队列，channel会消耗队列q中的消息
    channel->BasicConsume(q);

    OssManager* oss = OssManager::get_instance();
    cout << "[INFO] backup 已启动, 消费队列 " << q << ", 目标 bucket " << bucket << endl;
    for(;;) {
        // 获取一封消息
        // 如果队列中没有消息，则一直等待
        Envelope::ptr_t envelope = channel->BasicConsumeMessage();

        if (envelope && envelope->Message()) {
            nlohmann::json message = nlohmann::json::parse(envelope->Message()->Body());
            string object = message["object"];
            string file = message["file"];
#ifdef DEBUG
            cout << "object: " << object << ", file: " << file << endl;
#endif
            if (!oss->upload_object(bucket, object, file)) {
                cerr << "Error: upload failed! object: " << object
                     << ", file: " << file << endl;
            } else {
                cout << "[OK] uploaded to OSS: bucket=" << bucket
                     << ", object=" << object << endl;
            }
        }
    }

    OssManager::destroy_instance();
}
