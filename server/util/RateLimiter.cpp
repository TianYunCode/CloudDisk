// =============================================================================
// RateLimiter.cpp —— 滑动窗口失败限速器实现 (单一真源)
// =============================================================================
#include "RateLimiter.h"

RateLimiter::RateLimiter(int maxFails, int windowSec, int cooldownSec)
    : maxFails_(maxFails), window_(windowSec), cooldown_(cooldownSec) {}

int RateLimiter::blocked(const std::string& key, std::time_t now) const
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return 0;
    if (it->second.blockUntil > now) return (int)(it->second.blockUntil - now);
    return 0;
}

void RateLimiter::onFail(const std::string& key, std::time_t now)
{
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = map_[key];
    if (now - e.first > window_) { e.first = now; e.fails = 0; }
    e.fails++;
    if (e.fails >= maxFails_) { e.blockUntil = now + cooldown_; e.fails = 0; e.first = now; }
}

void RateLimiter::onSuccess(const std::string& key)
{
    std::lock_guard<std::mutex> lk(mu_);
    map_.erase(key);
}
