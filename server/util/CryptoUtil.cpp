#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <jwt.h>
#include <stdlib.h>
#include <string.h>
#include <cstdint>
#include <random>
#include <vector>
#include <iostream>

#include "CryptoUtil.h"
#include "User.h"
#include "Config.h"

using namespace std;

// JWT 密钥从配置/环境变量读取, 不再硬编码。
static std::string secret_key()
{
    return Config::instance().jwt_secret();
}

string CryptoUtil::generate_salt(int length)
{
    // 必须使用密码学安全随机数 (CSPRNG)。
    //
    // 历史缺陷: 这里曾用**未播种**的 rand() —— C 标准要求 rand() 在未调用 srand()
    // 时等价于 srand(1), 因此每次进程启动产生的随机序列**完全相同**。后果是
    // "服务重启后第 N 个注册用户"必然拿到相同的 salt; 而相同密码 + 相同 salt
    // 会得到完全相同的哈希, 于是:
    //   · 攻击者拿到哈希表即可直接关联出"哪些用户共用同一密码"(实测 996 个用户
    //     仅 633 个不同 salt, 最热的一个被复用 35 次, 7 个账号哈希完全相同);
    //   · salt 可预测, 逐用户加盐抵御彩虹表的意义被完全抵消。
    static const char alpha[] = "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (length <= 0) length = 8;
    const size_t want = static_cast<size_t>(length);

    string result;
    result.reserve(want);

    std::vector<unsigned char> buf(want);
    if (RAND_bytes(buf.data(), buf.size()) == 1) {
        // 拒绝采样: 256 % 62 = 8, 直接取模会让前 8 个字符概率偏高;
        // 只接受 < 248 (= 62*4) 的字节即可完全消除偏置。
        size_t i = 0;
        while (result.size() < want) {
            if (i >= buf.size()) {                      // 熵用尽则再取一批
                if (RAND_bytes(buf.data(), buf.size()) != 1) break;
                i = 0;
            }
            const unsigned char b = buf[i++];
            if (b < 248) result += alpha[b % 62];
        }
    }
    if (result.size() < want) {
        // CSPRNG 不可用时的兜底: 用 std::random_device (通常直读 /dev/urandom)。
        // 绝不退回未播种的 rand() —— 那会原样重新引入上述可预测性。
        std::random_device rd;
        std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()));
        std::uniform_int_distribution<int> dist(0, 61);
        while (result.size() < want) result += alpha[dist(gen)];
    }
    return result;
}

// OpenSSL 3.0 及更新版本推荐使用 EVP(Envelope) 接口
string CryptoUtil::hash_password(const string& password, const string& salt, const EVP_MD* md)
{
    EVP_MD_CTX* context = EVP_MD_CTX_new(); // 创建 EVP 上下文

    EVP_DigestInit_ex(context, md, NULL);
    // 更新上下文
    EVP_DigestUpdate(context, salt.c_str(), salt.size());
    EVP_DigestUpdate(context, password.c_str(), password.size());
    // 计算哈希值
    unsigned char hash[EVP_MAX_MD_SIZE];    // 最大哈希长度
    unsigned int len = 0;                   // 用来接收实际哈希长度
    EVP_DigestFinal(context, hash, &len);

    // 转换成十六进制字符，存储到result中
    char result[EVP_MAX_MD_SIZE * 2 + 1] = { '\0' };
    for(unsigned i = 0; i < len; i++) {
        sprintf(result + 2 * i, "%02x", hash[i]);
    }

    EVP_MD_CTX_free(context);               // 释放上下文

    return result;
}

string CryptoUtil::generate_token(const User& user, jwt_alg_t algorithm)
{
    jwt_t* jwt;
    jwt_new(&jwt);  // 创建 JWT

    std::string key = secret_key();
    jwt_set_alg(jwt, algorithm, (unsigned char*)key.c_str(), key.size());

    // 设置载荷(Payload): 用户自定义数据
    // 不要放敏感数据
    jwt_add_grant(jwt, "sub", "LoginToken");
    jwt_add_grant_int(jwt, "id", user.id);
    jwt_add_grant(jwt, "username", user.username.c_str());	    // 用户ID
    jwt_add_grant(jwt, "created_at", user.createdAt.c_str());   // 注册时间
    jwt_add_grant_int(jwt, "exp", time(NULL) + 3600);   // 过期时间 (1小时)

    char* token = jwt_encode_str(jwt);		// token长度是不确定的，100-300字节
    string result = token;

    // 释放资源
    jwt_free(jwt);
    free(token);

    return result;
}

bool CryptoUtil::verify_token(const string& token, User& user)
{
    // 在必要的地方写注释
    // 代码即注释
    jwt_t* jwt;
    std::string key = secret_key();
    int err = jwt_decode(&jwt, token.c_str(), (unsigned char*)key.c_str(), key.size());
    if (err) {
        return false;
    }

    // 验证主题
    const char* subject = jwt_get_grant(jwt, "sub");    
    if (subject == nullptr || strcmp(subject, "LoginToken") != 0) {
        jwt_free(jwt);
        return false;
    }

    // 验证是否超时
    long expire = jwt_get_grant_int(jwt, "exp");
    if (expire < time(NULL)) {
        jwt_free(jwt);
        return false;
    }

    user.id = jwt_get_grant_int(jwt, "id");
    user.username = jwt_get_grant(jwt, "username");
    user.createdAt = jwt_get_grant(jwt, "created_at");

    jwt_free(jwt);
    return true;
}

std::string CryptoUtil::generate_hashcode(const char* data, size_t n, const EVP_MD* md)
{
    EVP_MD_CTX* context = EVP_MD_CTX_new();     // 创建 EVP 上下文
    
    EVP_DigestInit_ex(context, md, NULL);

    EVP_DigestUpdate(context, data, n);

    // 计算哈希值
    unsigned char hash[EVP_MAX_MD_SIZE];    // 最大哈希长度
    unsigned int len = 0;                   // 用来接收实际哈希长度
    EVP_DigestFinal(context, hash, &len);   

    char result[EVP_MAX_MD_SIZE * 2 + 1] = { '\0' };
    // 转换成十六进制字符，存储到output中
    for (unsigned i = 0; i < len; i++) {
        sprintf(result + 2 * i, "%02x", hash[i]);
    }
    EVP_MD_CTX_free(context);               // 释放上下文

    return result;
}
