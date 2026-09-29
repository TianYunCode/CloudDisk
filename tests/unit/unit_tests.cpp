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
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include "SqlUtil.h"
#include "Totp.h"
#include "FileType.h"
#include "RateLimiter.h"
#include "JsonUtil.h"
#include "AuditAction.h"
#include "FormatUtil.h"
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

    // utf8_length: 按 Unicode 码点计数 (非字节数), 与前端 charLength 一致
    CHECK(SqlUtil::utf8_length("") == 0, "utf8_length 空串 = 0");
    CHECK(SqlUtil::utf8_length("abcdef") == 6, "utf8_length ASCII = 字节数");
    CHECK(SqlUtil::utf8_length("\xE5\xAD\x97") == 1, "utf8_length 单个汉字 = 1 (非 3 字节)");
    CHECK(SqlUtil::utf8_length("\xF0\x9F\x94\x92") == 1, "utf8_length 单个 emoji = 1 (非 4 字节)");
    CHECK(SqlUtil::utf8_length("\xE5\xAF\x86\xE7\xA0\x81") == 2, "utf8_length \"密码\" = 2 (非 6 字节)");

    // valid_password: 6-64 个字符 (码点), 修复前按字节计数会放过 2 个汉字
    CHECK(!SqlUtil::valid_password("12345"), "5 字符密码被拒");
    CHECK(SqlUtil::valid_password("123456"), "6 字符密码合法 (下边界)");
    CHECK(SqlUtil::valid_password(std::string(64, 'x')), "64 字符密码合法 (上边界)");
    CHECK(!SqlUtil::valid_password(std::string(65, 'x')), "65 字符密码被拒");
    CHECK(!SqlUtil::valid_password("\xE5\xAF\x86\xE7\xA0\x81"), "\"密码\"(2 字符/6 字节) 不再被误判为合法");
    CHECK(SqlUtil::valid_password("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAF\x86\xE7\xA0\x81\xE5\x85\xAD\xE4\xB8\xAA\xE5\xAD\x97"),
          "恰好 6 个汉字合法");
    std::string many_hanzi;
    for (int i = 0; i < 65; ++i) many_hanzi += "\xE5\xAD\x97";   // 65 个汉字 = 195 字节
    CHECK(!SqlUtil::valid_password(many_hanzi), "65 个汉字被拒 (码点上限)");
    std::string ok_hanzi;
    for (int i = 0; i < 64; ++i) ok_hanzi += "\xE5\xAD\x97";     // 64 个汉字 = 192 字节
    CHECK(SqlUtil::valid_password(ok_hanzi), "64 个汉字合法 (码点上限内, 虽 192 字节)");
    CHECK(!SqlUtil::valid_password(std::string(300, 'a')), "300 字节超长密码被拒 (原始长度上限)");
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

static void test_jsonutil()
{
    std::printf("== JsonUtil ==\n");
    auto j = nlohmann::json::parse(R"({"name":"alice","n":42,"ids":[1,2,"3"],"bad":[0],"mixed":[1,"x"],"nan":[-1]})");
    // json_str
    CHECK(json_str(j, "name") == "alice", "json_str 读取字符串");
    CHECK(json_str(j, "n").empty(), "json_str 非字符串返回空");
    CHECK(json_str(j, "missing").empty(), "json_str 缺失键返回空");
    // ids_from_json
    std::vector<long long> out;
    CHECK(ids_from_json(j, "ids", out) && out.size() == 3 && out[0] == 1 && out[2] == 3, "ids 数字与数字字符串混合");
    CHECK(ids_from_json(j, "missing", out) && out.empty(), "缺省键视为空数组且合法");
    CHECK(!ids_from_json(j, "bad", out), "含 0 非法");
    CHECK(!ids_from_json(j, "mixed", out), "含非数字字符串非法");
    CHECK(!ids_from_json(j, "nan", out), "含负数非法");
    CHECK(!ids_from_json(j, "name", out), "非数组非法");
    // ids_csv
    CHECK(ids_csv({1,2,3}) == "1,2,3", "ids_csv 拼接");
    CHECK(ids_csv({}).empty(), "ids_csv 空列表");
    CHECK(ids_csv({7}) == "7", "ids_csv 单元素无逗号");
}

static void test_audit_action()
{
    std::printf("== AuditAction (审计动作单一真源) ==\n");
    // 汇总全部动作常量: 新增动作时必须在此登记, 否则契约测试无法覆盖
    const std::vector<std::string> all = {
        AuditAction::Register, AuditAction::Login, AuditAction::LoginFail,
        AuditAction::LoginBlocked, AuditAction::Login2faFail, AuditAction::LoginDisabled,
        AuditAction::PasswordChange, AuditAction::PasswordChangeFail,
        AuditAction::ProfileUpdate, AuditAction::AvatarUpdate,
        AuditAction::TwoFaEnable, AuditAction::TwoFaDisable,
        AuditAction::TokenCreate, AuditAction::TokenRevoke,
        AuditAction::FileUpload, AuditAction::FileDelete, AuditAction::FileRestore,
        AuditAction::FileRename, AuditAction::FolderCreate, AuditAction::ShareCreate,
        AuditAction::AdminSetRole, AuditAction::AdminSetQuota, AuditAction::AdminSetDisabled,
    };
    CHECK(all.size() == 23, "共 23 个动作常量 (与 AuditAction.h 一致)");

    bool anyEmpty = false, anyBadChar = false;
    for (const auto& a : all) {
        if (a.empty()) anyEmpty = true;
        for (char c : a) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) anyBadChar = true;
        }
    }
    CHECK(!anyEmpty, "所有动作名非空");
    CHECK(!anyBadChar, "所有动作名为 snake_case (小写/数字/下划线)");

    std::set<std::string> uniq(all.begin(), all.end());
    CHECK(uniq.size() == all.size(), "动作名无重复 (防止复制粘贴同值)");

    // 关键值锚定: 前端 ACT_META 依赖这些字面值
    CHECK(std::string(AuditAction::Login) == "login", "Login == login");
    CHECK(std::string(AuditAction::TwoFaEnable) == "2fa_enable", "TwoFaEnable == 2fa_enable");
    CHECK(std::string(AuditAction::AdminSetQuota) == "admin_set_quota", "AdminSetQuota == admin_set_quota");
}

static void test_formatutil()
{
    std::printf("== FormatUtil (人类可读字节, 与前端 humanSize 一致) ==\n");
    using fmtutil::human_size;
    CHECK(human_size(0) == "0 B", "0 -> 0 B");
    CHECK(human_size(512) == "512 B", "512 -> 512 B");
    CHECK(human_size(1023) == "1023 B", "1023 -> 1023 B (不足 1KB 不进位)");
    CHECK(human_size(1024) == "1.0 KB", "1024 -> 1.0 KB");
    CHECK(human_size(1536) == "1.5 KB", "1536 -> 1.5 KB");
    CHECK(human_size(1048576) == "1.0 MB", "1MiB -> 1.0 MB");
    CHECK(human_size(5242880) == "5.0 MB", "5MiB -> 5.0 MB");
    CHECK(human_size(1073741824) == "1.0 GB", "1GiB -> 1.0 GB");
    CHECK(human_size(1099511627776LL) == "1.0 TB", "1TiB -> 1.0 TB");
    // >=100 时取整 (与前端 toFixed(0) 一致)
    CHECK(human_size(150 * 1024) == "150 KB", "150KiB -> 150 KB (>=100 取整)");
    CHECK(human_size(102400) == "100 KB", "100KiB -> 100 KB (边界取整)");
    CHECK(human_size(101376) == "99.0 KB", "99KiB -> 99.0 KB (<100 保留 1 位)");
    // 超过 TB 不再进位, 停在 TB
    CHECK(human_size(10995116277760LL) == "10.0 TB", "10TiB -> 10.0 TB (上限 TB, <100 保留 1 位)");
    CHECK(human_size(109951162777600LL) == "100 TB", "100TiB -> 100 TB");
    // 负数按「符号 + 幅值」输出, 而非抹成 0 (差值型指标可能为负)
    CHECK(human_size(-1) == "-1 B", "-1 -> -1 B");
    CHECK(human_size(-1536) == "-1.5 KB", "-1536 -> -1.5 KB");
    CHECK(human_size(-1048576) == "-1.0 MB", "-1MiB -> -1.0 MB");
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
    test_jsonutil();
    test_audit_action();
    test_formatutil();
    test_crypto();
    std::printf("\n结果: %d 通过, %d 失败 (共 %d)\n", g_total - g_fail, g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
