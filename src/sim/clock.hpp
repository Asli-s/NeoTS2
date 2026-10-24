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
constexpr std::size_t kTimeOfDay = 0x04;
constexpr std::size_t kMinute = 0x05;
constexpr std::size_t kSecond = 0x06;
constexpr std::size_t kMonth = 0x07;
constexpr std::size_t kYear = 0x08;
constexpr std::size_t kCameraStarted = 0x0B;  // named "Unused" in the game's list, and Camera Control sets it
constexpr std::size_t kBudgetLow = 0x0D;   // funds % 10000
constexpr std::size_t kBudgetHigh = 0x0E;  // funds / 10000
constexpr std::size_t kSpeed = 0x10;
constexpr std::size_t kPaused = 0x11;
constexpr std::size_t kDayOfWeek = 0x22;  // 0 Monday
constexpr std::size_t kCameraBusy = 0x34;
} // namespace global

// Game speeds as the Speed global holds them.
enum class Speed : std::int16_t { Normal = 2, High = 3, Ultra = 4 };

// Each tick moves the clock on two Sim seconds.
constexpr int kSecondsPerTick = 2;

// The Time Of Day global's values.
enum class TimeOfDay : std::int16_t { Day = 0, Evening = 1, Night = 2, Morning = 3 };

// The time of day an hour (0..23) falls in: night before 6, morning from 6, day from 7, evening
// from 18 and night again from 19.
TimeOfDay timeOfDay(int hour);

// Moves the clock in `globals` on one tick: seconds, minutes, hours, then the date and the day of
// the week. The Time Of Day global follows the hour, as the game sets it on every clock step.
void advanceClock(std::span<std::int16_t> globals);

// Sets the Time Of Day global from the Hour global (as the game does when a lot loads).
void updateTimeOfDay(std::span<std::int16_t> globals);

// The day of the week, 0 for Monday: the Day Of Week global, which need not match the date's
// (the game shows it). It is the date's when the global is out of range.
int weekday(std::span<const std::int16_t> globals);

// The clock as the live panel shows it, e.g. "Mon 3:22 pm".
std::string clockText(std::span<const std::int16_t> globals);

// The funds the Budget globals hold, and setting them (held to what the globals can hold, 0 and up).
std::int32_t funds(std::span<const std::int16_t> globals);
void setFunds(std::span<std::int16_t> globals, std::int64_t funds);

} // namespace ts2::sim
