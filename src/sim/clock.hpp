#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ts2::sim {

// The simulator globals the clock and the speed controls use.
namespace global {
constexpr std::size_t kHour = 0x00;
constexpr std::size_t kDayOfMonth = 0x01;
constexpr std::size_t kSelectedPerson = 0x03;
constexpr std::size_t kMinute = 0x05;
constexpr std::size_t kSecond = 0x06;
constexpr std::size_t kMonth = 0x07;
constexpr std::size_t kYear = 0x08;
constexpr std::size_t kBudgetLow = 0x0D;   // funds % 10000
constexpr std::size_t kBudgetHigh = 0x0E;  // funds / 10000
constexpr std::size_t kSpeed = 0x10;
constexpr std::size_t kPaused = 0x11;
} // namespace global

// Game speeds as the Speed global holds them.
enum class Speed : std::int16_t { Normal = 2, High = 3, Ultra = 4 };

// Each tick moves the clock on two Sim seconds.
constexpr int kSecondsPerTick = 2;

// Moves the clock in `globals` on one tick: seconds, minutes, hours, then the date.
void advanceClock(std::span<std::int16_t> globals);

// The day of the week of the clock's date, 0 for Monday.
int weekday(std::span<const std::int16_t> globals);

// The clock as the live panel shows it, e.g. "Mon 3:22 pm".
std::string clockText(std::span<const std::int16_t> globals);

// The funds the Budget globals hold.
std::int32_t funds(std::span<const std::int16_t> globals);

} // namespace ts2::sim
