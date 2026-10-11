#include "sim/clock.hpp"

#include <cstdio>

namespace ts2::sim {

namespace {

bool leapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int month, int year) {
    constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leapYear(year) ? 29 : kDays[(month >= 1 && month <= 12 ? month : 1) - 1];
}

} // namespace

void advanceClock(std::span<std::int16_t> g) {
    if (g.size() <= global::kYear) {
        return;
    }
    g[global::kSecond] = static_cast<std::int16_t>(g[global::kSecond] + kSecondsPerTick);
    if (g[global::kSecond] < 60) {
        return;
    }
    g[global::kSecond] = static_cast<std::int16_t>(g[global::kSecond] - 60);
    if (++g[global::kMinute] < 60) {
        return;
    }
    g[global::kMinute] = 0;
    if (++g[global::kHour] < 24) {
        return;
    }
    g[global::kHour] = 0;
    if (++g[global::kDayOfMonth] <= daysInMonth(g[global::kMonth], g[global::kYear])) {
        return;
    }
    g[global::kDayOfMonth] = 1;
    if (++g[global::kMonth] <= 12) {
        return;
    }
    g[global::kMonth] = 1;
    ++g[global::kYear];
}

int weekday(std::span<const std::int16_t> g) {
    // Days since Monday 1 January 1900 (proleptic Gregorian calendar), modulo 7.
    const int year = g[global::kYear], month = g[global::kMonth], day = g[global::kDayOfMonth];
    long days = 0;
    for (int y = 1900; y < year; ++y) {
        days += leapYear(y) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) {
        days += daysInMonth(m, year);
    }
    days += day - 1;
    return static_cast<int>(((days % 7) + 7) % 7);
}

std::string clockText(std::span<const std::int16_t> g) {
    if (g.size() <= global::kYear) {
        return {};
    }
    constexpr const char* kDays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const int hour = g[global::kHour];
    const int twelve = hour % 12 == 0 ? 12 : hour % 12;
    char text[32];
    std::snprintf(text, sizeof(text), "%s %d:%02d %s", kDays[weekday(g)], twelve, g[global::kMinute], hour < 12 ? "am" : "pm");
    return text;
}

std::int32_t funds(std::span<const std::int16_t> g) {
    return g.size() > global::kBudgetHigh ? static_cast<std::int32_t>(g[global::kBudgetHigh]) * 10000 + g[global::kBudgetLow] : 0;
}

} // namespace ts2::sim
