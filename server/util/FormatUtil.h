#ifndef CLOUDDISK_FORMATUTIL_H
#define CLOUDDISK_FORMATUTIL_H
// =============================================================================
// FormatUtil.h —— 面向用户的数值格式化 (纯函数, 无副作用, 可单测)
//
// 动机: 错误提示中直接拼 std::to_string(bytes) + " 字节" 会让用户看到
//       "存储空间不足: 剩余 1048576 字节" 这类无法阅读的数字。
//
// 约定: human_size() 与前端 web/js/core/api.js 的 humanSize() 保持**完全一致**
//       的算法与输出 (单位 KB/MB/GB/TB, <100 保留 1 位小数, 否则取整),
//       由 tests/e2e/humansize_contract_validate.mjs 做跨语言一致性校验,
//       防止两端各自演化后同一文件在页面与报错里显示不同大小。
// =============================================================================
#include <cstdio>
#include <string>

namespace fmtutil {

// 将字节数格式化为人类可读字符串, 例: 0 -> "0 B", 1536 -> "1.5 KB",
// 5242880 -> "5.0 MB", 1099511627776 -> "1.0 TB"。
// 负数按「符号 + 幅值」输出 (如 -1048576 -> "-1.0 MB"), 而非抹成 0:
// 差值型指标 (如"去重节省"= 逻辑大小 - 物理占用) 可能为负, 显示 0 会误导。
inline std::string human_size(long long bytes)
{
    const bool neg = bytes < 0;
    // 用无符号量承载幅值, 避免 LLONG_MIN 取负时溢出 (未定义行为)
    const unsigned long long mag =
        neg ? static_cast<unsigned long long>(-(bytes + 1)) + 1ULL
            : static_cast<unsigned long long>(bytes);

    std::string body;
    if (mag < 1024) {
        body = std::to_string(mag) + " B";
    } else {
        static const char* const UNITS[] = { "KB", "MB", "GB", "TB" };
        double v = static_cast<double>(mag);
        int i = -1;
        do { v /= 1024.0; ++i; } while (v >= 1024.0 && i < 3);   // 上限 TB, 不再进位
        char buf[32];
        std::snprintf(buf, sizeof(buf), (v >= 100.0 ? "%.0f" : "%.1f"), v);
        body = std::string(buf) + " " + UNITS[i];
    }
    return neg ? "-" + body : body;
}

} // namespace fmtutil

#endif // CLOUDDISK_FORMATUTIL_H
