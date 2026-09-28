// =============================================================================
// RateLimiter.h —— 进程内滑动窗口失败限速器 (登录暴力破解防护)
//
// 独立、线程安全、可注入时间的策略对象: 把"窗口内失败计数 + 触发冷却"的策略
// 从网关支撑层抽出, 高内聚、可复用、可确定性单元测试 (now 由调用方注入)。
// =============================================================================
#ifndef CLOUDDISK_RATE_LIMITER_H
#define CLOUDDISK_RATE_LIMITER_H

#include <string>
#include <mutex>
#include <unordered_map>
#include <ctime>

class RateLimiter {
public:
    // maxFails: 窗口内最大失败次数; windowSec: 统计窗口; cooldownSec: 触发后冷却
    RateLimiter(int maxFails, int windowSec, int cooldownSec);

    // 返回剩余冷却秒数 (>0 表示当前被限流)。now 由调用方注入以保证可测试与确定性。
    int blocked(const std::string& key, std::time_t now) const;
    // 记录一次失败; 达到阈值则进入冷却。
    void onFail(const std::string& key, std::time_t now);
    // 记录一次成功: 清除该 key 的计数与冷却。
    void onSuccess(const std::string& key);

private:
    struct Entry { int fails = 0; std::time_t first = 0; std::time_t blockUntil = 0; };
    int maxFails_, window_, cooldown_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
};

#endif // CLOUDDISK_RATE_LIMITER_H
