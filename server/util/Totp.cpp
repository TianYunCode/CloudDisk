// =============================================================================
// Totp.cpp —— Base32 / TOTP 纯算法实现
// =============================================================================
#include "Totp.h"

#include <cctype>
#include <ctime>
#include <openssl/hmac.h>
#include <openssl/evp.h>

// ---- Base32 (RFC4648) ----
std::string base32_encode(const unsigned char* data, std::size_t len)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out; int buf = 0, bits = 0;
    for (std::size_t i = 0; i < len; ++i) {
        buf = (buf << 8) | data[i]; bits += 8;
        while (bits >= 5) { out += A[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) out += A[(buf << (5 - bits)) & 31];
    return out;
}

bool base32_decode(const std::string& in, std::vector<unsigned char>& out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= '2' && c <= '7') return c - '2' + 26;
        return -1;
    };
    int buf = 0, bits = 0; out.clear();
    for (char c : in) {
        if (c == '=' || c == ' ') continue;
        int v = val(c); if (v < 0) return false;
        buf = (buf << 5) | v; bits += 5;
        if (bits >= 8) { out.push_back((unsigned char)((buf >> (bits - 8)) & 0xff)); bits -= 8; }
    }
    return true;
}

// ---- TOTP (RFC6238, SHA1, 6 位, 30 秒) ----
unsigned totp_at(const std::vector<unsigned char>& key, uint64_t counter)
{
    unsigned char msg[8];
    for (int i = 7; i >= 0; --i) { msg[i] = (unsigned char)(counter & 0xff); counter >>= 8; }
    unsigned char mac[EVP_MAX_MD_SIZE]; unsigned int maclen = 0;
    HMAC(EVP_sha1(), key.data(), (int)key.size(), msg, 8, mac, &maclen);
    int off = mac[maclen - 1] & 0x0f;
    unsigned bin = ((mac[off] & 0x7f) << 24) | (mac[off + 1] << 16) | (mac[off + 2] << 8) | (mac[off + 3]);
    return bin % 1000000u;
}

bool totp_verify(const std::string& secretB32, const std::string& codeStr)
{
    if (codeStr.size() != 6) { for (char c : codeStr) if (!isdigit((unsigned char)c)) return false; if (codeStr.empty()) return false; }
    std::vector<unsigned char> key;
    if (!base32_decode(secretB32, key) || key.empty()) return false;
    unsigned code;
    try { code = (unsigned)std::stoul(codeStr); } catch (...) { return false; }
    uint64_t t = (uint64_t)(time(nullptr) / 30);
    for (int w = -1; w <= 1; ++w) {   // 容忍 ±1 时间窗
        if (totp_at(key, t + w) == code) return true;
    }
    return false;
}
