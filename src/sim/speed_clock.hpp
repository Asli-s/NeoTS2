#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

namespace ts2::sim {

// The game's speed clock, the time clips play by and ticks are paced by. It runs at 1, 3 or 21
// times real time (normal, fast and ultra speed). Each update moves it by the real time since the
// last one, at most 200 real ms of it and at most 200 ms of clock time in all. Real time past
// 200 ms is carried to the next updates, up to a second of it, and clock time past 200 ms is
// dropped, so at ultra speed the clock falls behind real time whenever frames are slow.
class SpeedClock {
public:
    static constexpr std::int64_t kMaxStepMs = 200;     // real ms one update takes in
    static constexpr std::int64_t kMaxCarryMs = 1000;   // real ms carried to later updates
    static constexpr double kMaxClockStepMs = 200.0;    // clock ms one update moves

    // A new rate drops the real time carried.
    void setRate(float rate) {
        if (rate != rate_) {
            carry_ = 0;
        }
        rate_ = rate;
    }
    float rate() const { return rate_; }

    // Moves the clock by the real time since the last update. `watchMs` is the real time the clock
    // has run in all (ms), which stops while the game is paused.
    void update(std::int64_t watchMs) {
        std::int64_t d = std::min(watchMs - lastWatch_, kMaxStepMs);
        lastWatch_ = watchMs;
        d += carry_;
        carry_ = 0;
        if (d > kMaxStepMs) {
            carry_ = std::min(d - kMaxStepMs, kMaxCarryMs);
            d = kMaxStepMs;
        }
        const double f = std::min(static_cast<double>(d) * rate_ + frac_, kMaxClockStepMs);
        const auto whole = static_cast<std::int64_t>(f);
        timeMs_ += whole;
        frac_ = static_cast<float>(f - static_cast<double>(whole));
    }
    std::int64_t timeMs() const { return timeMs_; }

private:
    float rate_ = 1.0f;
    std::int64_t lastWatch_ = 0;
    std::int64_t carry_ = 0;
    float frac_ = 0.0f;
    std::int64_t timeMs_ = 0;
};

// How many ticks the game runs each frame. A tick is due for each 42 ms of the speed clock, and
// the clock is updated again while ticks run, so ticks keep up with it within about a tick. At
// normal and fast speed a frame runs a quarter of the ticks due, rounded up, plus the average a
// frame ran over the last ten. At ultra speed, or past 125, a frame runs up to 125 and owes at
// most 125 ticks of clock time. A frame stops early once its ticks have taken 250 real ms.
class TickPacer {
public:
    static constexpr std::int64_t kTickMs = 42;  // clock ms per tick, at every speed
    static constexpr int kMaxTicks = 125;
    static constexpr double kMaxFrameMs = 250.0;

    // Changing speed owes no time and scales the ticks remembered by the change in rate.
    void setSpeed(float rate) {
        owedMs_ = 0;
        lastClockMs_ = 0;
        const double ratio = static_cast<double>(rate) / clock_.rate();
        for (auto& h : history_) {
            h = static_cast<std::int64_t>(static_cast<double>(h) * ratio);
        }
        clock_.setRate(rate);
    }
    // Unpausing owes no time.
    void resume() {
        owedMs_ = 0;
        lastClockMs_ = 0;
    }

    // Runs one frame's ticks: `tick` runs one, `watchMs` gives the real time the clock has run
    // (see SpeedClock::update) and `frameMs` the real time this frame's ticks have taken. Returns
    // the ticks run.
    int frame(bool ultra, const std::function<void()>& tick, const std::function<std::int64_t()>& watchMs,
              const std::function<double()>& frameMs) {
        owe(clock_.timeMs());
        std::int64_t due = owedMs_ / kTickMs;
        double sum = 0;
        for (const auto h : history_) {
            sum += static_cast<double>(h);
        }
        const auto average = static_cast<std::int64_t>(sum * static_cast<double>(0.1f));
        auto n = static_cast<std::int64_t>(std::ceil(static_cast<double>(due) * 0.25) + static_cast<double>(average));
        n = std::max<std::int64_t>(n, 0);
        if (n > kMaxTicks || ultra) {
            n = kMaxTicks;
            owedMs_ = std::min(owedMs_, kMaxTicks * kTickMs);
        }
        int ticks = 0;
        for (std::int64_t count = 1;; ++count) {
            const bool done = count >= n;
            const bool update = count == 1 || due <= 0;
            const bool run = due >= 0 && n > 0;
            if (update) {
                clock_.update(watchMs());
                owe(clock_.timeMs());
            }
            due = owedMs_ / kTickMs;
            if (run) {
                tick();
                ++ticks;
                owedMs_ -= kTickMs;
            }
            if (done || frameMs() > kMaxFrameMs) {
                break;
            }
        }
        remember(ticks);
        return ticks;
    }

    // Clock ms owed past the ticks run, negative when ticks ran ahead of the clock.
    std::int64_t owedMs() const { return owedMs_; }
    const SpeedClock& clock() const { return clock_; }

private:
    void owe(std::int64_t clockMs) {
        // A clock reading of 0 counts as none, so the first frame after one owes a tick.
        owedMs_ = lastClockMs_ != 0 ? owedMs_ + (clockMs - lastClockMs_) : kTickMs;
        lastClockMs_ = clockMs;
    }
    // The game keeps the index right after the ten entries and writes a frame's ticks there when it
    // reaches 10, so the next frame's entry goes at that count plus one, or back at 0 past 10.
    void remember(int ticks) {
        if (index_ < history_.size()) {
            history_[index_] = ticks;
            ++index_;
        } else {
            index_ = static_cast<std::size_t>(ticks) + 1;
        }
        if (index_ > history_.size()) {
            index_ = 0;
        }
    }

    SpeedClock clock_;
    std::int64_t owedMs_ = 0;
    std::int64_t lastClockMs_ = 0;
    std::array<std::int64_t, 10> history_{};
    std::size_t index_ = 0;
};

} // namespace ts2::sim
