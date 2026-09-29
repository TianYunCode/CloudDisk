#pragma once
#include <string>
#include <cstddef>
#include <jwt.h>
#include <openssl/evp.h>

#include "User.h"

// 增量式 SHA-256 计算器 (RAII)。
//
// 存在意义: 大文件不能整块读进内存再哈希 —— 原实现把全部分片拼成一个 string
// 后调用 generate_hashcode(), 10 GiB 文件即需 10 GiB 内存, 会直接耗尽资源拖垮
// 整个服务进程。本类允许"边读边算": 每次只持有一个分片 (约 4 MiB), 内存占用
// 与文件总大小无关。
//
// 用法:
//     Sha256Stream s;
//     while (读取到一块 buf/n) s.update(buf, n);
//     std::string hex = s.final_hex();     // 64 位小写十六进制
class Sha256Stream
{
public:
    Sha256Stream();
    ~Sha256Stream();
    // 不可拷贝 (持有 EVP_MD_CTX 所有权)
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    void update(const void* data, std::size_t n);
    void update(const std::string& s) { update(s.data(), s.size()); }
    // 结束计算并返回摘要; 之后对象进入已完成状态, 再调用返回空串。
    std::string final_hex();

private:
    EVP_MD_CTX* m_ctx;
    bool m_done;
};

class CryptoUtil
{
public:
    static std::string generate_salt(int length = 8);
    static std::string hash_password(const std::string& password, const std::string& salt, const EVP_MD* md = EVP_sha256());
    static std::string generate_hashcode(const char* data, size_t n, const EVP_MD* md = EVP_sha256());
    static std::string generate_token(const User& user, jwt_alg_t algorithm = JWT_ALG_HS256);
    static bool verify_token(const std::string& token, User& user);
private:
    /* 禁止构造对象 */
    CryptoUtil() = delete;
};
