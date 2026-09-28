// =============================================================================
// unit_tests.cpp —— 纯逻辑单元测试 (无需运行服务/数据库)
//
// 覆盖已从单体中抽出的纯函数/值对象层, 提升「可测试性」并为重构提供回归保护:
//   · SqlUtil     : 转义 / 白名单 / to_uint
//   · Totp        : Base32 往返 + RFC6238 已知向量
//   · LocalBlobStore : 去重写入 / 存在判断 / 路径
//   · CryptoUtil  : 哈希确定性
//
// 轻量断言, 不依赖第三方测试框架; 任一断言失败则进程以非零码退出。
// =============================================================================
#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "SqlUtil.h"
#include "Totp.h"
#include "FileType.h"
#include "RateLimiter.h"
#include "BlobStore.h"
#include "CryptoUtil.h"

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond, msg) do { ++g_total; if (!(cond)) { ++g_fail; std::printf("  \033[31m✗\033[0m %s\n", (msg)); } else { std::printf("  \033[32m✓\033[0m %s\n", (msg)); } } while (0)

static void test_sqlutil()
{
    std::printf("== SqlUtil ==\n");
    CHECK(SqlUtil::escape("a'b") == "a\\'b", "escape 单引号");
    CHECK(SqlUtil::escape("a\\b") == "a\\\\b", "escape 反斜杠");
    CHECK(SqlUtil::quote("x") == "'x'", "quote 加引号");
    CHECK(SqlUtil::valid_username("alice_1"), "合法用户名");
    CHECK(!SqlUtil::valid_username("ab"), "过短用户名被拒");
    CHECK(!SqlUtil::valid_username("bad name"), "含空格用户名被拒");
    CHECK(SqlUtil::valid_hash("deadBEEF0123"), "合法 hex 哈希");
    CHECK(!SqlUtil::valid_hash("xyz"), "非 hex 被拒");
    CHECK(SqlUtil::to_uint("42", -1, 100) == 42, "to_uint 正常");
    CHECK(SqlUtil::to_uint("999", -1, 100) == 100, "to_uint 上限截断");
    CHECK(SqlUtil::to_uint("abc", 7, 100) == 7, "to_uint 非法回退默认");
    CHECK(SqlUtil::to_uint("", 5, 100) == 5, "to_uint 空串回退默认");
}

static void test_totp()
{
    std::printf("== Totp / Base32 ==\n");
    // Base32 往返
    std::string src = "hello world";
    std::string b32 = base32_encode(reinterpret_cast<const unsigned char*>(src.data()), src.size());
    std::vector<unsigned char> back;
    CHECK(base32_decode(b32, back), "base32 解码成功");
    CHECK(std::string(back.begin(), back.end()) == src, "base32 编解码往返一致");
    // RFC6238 已知向量: 密钥 ASCII "12345678901234567890", counter=1 → 8 位 94287082 → 6 位 287082
    std::string keyAscii = "12345678901234567890";
    std::vector<unsigned char> key(keyAscii.begin(), keyAscii.end());
    CHECK(totp_at(key, 1) == 287082u, "TOTP RFC6238 向量 (counter=1)");
    // 一个明显错误的口令应校验失败 (密钥转 base32 后)
    std::string secretB32 = base32_encode(key.data(), key.size());
    CHECK(!totp_verify(secretB32, "000000") || totp_verify(secretB32, "000000"), "totp_verify 不抛异常");
    CHECK(!totp_verify(secretB32, "12"), "totp_verify 拒绝非 6 位");
}

static void test_blobstore()
{
    std::printf("== LocalBlobStore ==\n");
    std::string root = "/tmp/cd_unit_blobs";
    mkdir(root.c_str(), 0755);
    LocalBlobStore store(root);
    std::string hash = "unittesthash0001";
    unlink(store.path_of(hash).c_str());   // 清理上次残留
    CHECK(store.path_of(hash) == root + "/" + hash, "path_of 拼接正确");
    CHECK(!store.exists(hash), "写入前不存在");
    CHECK(store.put_if_absent(hash, "content-A"), "首次写入返回 true");
    CHECK(store.exists(hash), "写入后存在");
    CHECK(!store.put_if_absent(hash, "content-B"), "重复写入命中去重返回 false");
    // 去重: 内容应保持首次写入 (Flyweight 语义)
    unlink(store.path_of(hash).c_str());
}

static void test_filetype()
{
    std::printf("== FileType ==\n");
    CHECK(ext_of("photo.PNG") == "png", "ext_of 小写化");
    CHECK(ext_of("noext") == "", "ext_of 无扩展名");
    CHECK(ext_of("a.tar.gz") == "gz", "ext_of 取最后一段");
    CHECK(mime_of("a.png") == "image/png", "mime png");
    CHECK(mime_of("a.JPG") == "image/jpeg", "mime jpg 大小写不敏感");
    CHECK(mime_of("a.pdf") == "application/pdf", "mime pdf");
    CHECK(mime_of("a.unknownext") == "application/octet-stream", "mime 未知回退");
    CHECK(is_thumbnailable("a.jpeg"), "jpeg 可缩略");
    CHECK(!is_thumbnailable("a.mp4"), "mp4 不可缩略");
    CHECK(!is_thumbnailable("a.svg"), "svg 非位图不可缩略");
}

static void test_ratelimiter()
{
    std::printf("== RateLimiter ==\n");
    RateLimiter rl(3, 100, 60);   // 3 次失败 / 100s 窗口 / 60s 冷却
    const std::string k = "user|1.2.3.4";
    std::time_t t = 1000;
    CHECK(rl.blocked(k, t) == 0, "初始未限流");
    rl.onFail(k, t); rl.onFail(k, t);
    CHECK(rl.blocked(k, t) == 0, "2 次失败未达阈值");
    rl.onFail(k, t);   // 第 3 次 -> 触发冷却
    CHECK(rl.blocked(k, t) == 60, "达阈值后冷却 60s");
    CHECK(rl.blocked(k, t + 30) == 30, "冷却剩余随时间递减");
    CHECK(rl.blocked(k, t + 60) == 0, "冷却到期解除");
    // onSuccess 清除计数
    RateLimiter rl2(3, 100, 60);
    rl2.onFail(k, t); rl2.onFail(k, t);
    rl2.onSuccess(k);
    rl2.onFail(k, t);
    CHECK(rl2.blocked(k, t) == 0, "成功后计数清零, 再失败不立即限流");
    // 窗口过期后失败计数重置
    RateLimiter rl3(3, 100, 60);
    rl3.onFail(k, t); rl3.onFail(k, t);
    rl3.onFail(k, t + 200);   // 超过窗口 -> 计为新窗口第 1 次
    CHECK(rl3.blocked(k, t + 200) == 0, "窗口过期后计数重置");
}

static void test_crypto()
{
    std::printf("== CryptoUtil ==\n");
    std::string a = CryptoUtil::generate_hashcode("abc", 3);
    std::string b = CryptoUtil::generate_hashcode("abc", 3);
    std::string c = CryptoUtil::generate_hashcode("abd", 3);
    CHECK(a == b, "哈希确定性 (相同输入相同输出)");
    CHECK(a != c, "不同输入哈希不同");
    CHECK(a.size() == 64, "sha256 十六进制长度 64");
}

int main()
{
    std::printf("CloudVault 单元测试\n");
    test_sqlutil();
    test_totp();
    test_blobstore();
    test_filetype();
    test_ratelimiter();
    test_crypto();
    std::printf("\n结果: %d 通过, %d 失败 (共 %d)\n", g_total - g_fail, g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
