#include "diag/timing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <string_view>

namespace ts2::diag {

namespace {

struct Total {
    std::uint64_t calls = 0;
    std::int64_t ns = 0;
    std::int64_t longestNs = 0;
};

std::atomic<bool> on{false};
std::mutex mutex;
std::map<const char*, Total> totals;  // by where the name is in memory: each timer's name is the same text

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void setTimings(bool enable) {
    on.store(enable, std::memory_order_relaxed);
}

bool timingsOn() {
    return on.load(std::memory_order_relaxed);
}

Timer::Timer(const char* name) : name_(name) {
    if (timingsOn()) {
        start_ = nowNs();
    }
}

Timer::~Timer() {
    if (start_ < 0) {
        return;
    }
    const std::int64_t ns = nowNs() - start_;
    std::lock_guard lock(mutex);
    Total& t = totals[name_];
    ++t.calls;
    t.ns += ns;
    t.longestNs = std::max(t.longestNs, ns);
}

std::string takeTimings() {
    std::map<const char*, Total> taken;
    {
        std::lock_guard lock(mutex);
        taken.swap(totals);
    }
    // A name used in two files can be at two places in memory: they count as one here.
    std::map<std::string_view, Total> byName;
    for (const auto& [name, t] : taken) {
        Total& merged = byName[name];
        merged.calls += t.calls;
        merged.ns += t.ns;
        merged.longestNs = std::max(merged.longestNs, t.longestNs);
    }
    std::string out;
    for (const auto& [name, t] : byName) {
        out += std::format("{}: {} calls, {:.3f} ms on average, {:.3f} ms longest\n", name, t.calls,
                           static_cast<double>(t.ns) / static_cast<double>(t.calls) / 1e6, static_cast<double>(t.longestNs) / 1e6);
    }
    return out;
}

} // namespace ts2::diag
