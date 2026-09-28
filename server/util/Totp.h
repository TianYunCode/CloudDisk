// =============================================================================
// Totp.h —— Base32 (RFC4648) 与 TOTP (RFC6238) 纯算法工具
//
// 从网关支撑层抽出的无外部运行时依赖的纯函数模块 (仅依赖 OpenSSL HMAC)，
// 高内聚、可复用、可独立单元测试 (见 tests/unit)。用于两步验证 (2FA)。
// =============================================================================
#ifndef CLOUDDISK_TOTP_H
#define CLOUDDISK_TOTP_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

// Base32 编码 / 解码 (RFC4648, 大小写不敏感, 忽略 '=' 与空格)
std::string base32_encode(const unsigned char* data, std::size_t len);
bool base32_decode(const std::string& in, std::vector<unsigned char>& out);

// TOTP: 给定密钥与时间计数器返回 6 位动态码 (RFC6238, HMAC-SHA1)
unsigned totp_at(const std::vector<unsigned char>& key, uint64_t counter);
// 校验 6 位口令 (容忍 ±1 个 30 秒时间窗)
bool totp_verify(const std::string& secretB32, const std::string& codeStr);

#endif // CLOUDDISK_TOTP_H
