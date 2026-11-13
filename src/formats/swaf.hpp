#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <optional>
#include <span>
#include <vector>

namespace ts2::swaf {

// A Sim's current wants and fears and its want history, saved in the neighborhood package
// (one resource per Sim, instance = neighbor id).

struct Want {
    std::uint32_t version = 0;
    std::uint16_t id = 0;       // neighbor id of the Sim it belongs to
    std::uint32_t guid = 0;     // which want
    std::uint8_t type = 0;      // what kind of argument it has (0 = none)
    std::int32_t argument = 0;
    std::int16_t value = 0;
    // Values after the argument. The second one is the aspiration points (negative for fears).
    std::vector<std::int32_t> tail;
    std::uint8_t flags = 0;
};

struct WantsAndFears {
    std::uint32_t version = 0;
    std::optional<Want> extra;
    std::uint32_t maxWants = 0;
    std::vector<Want> wants;
    std::uint32_t maxFears = 0;
    std::vector<Want> fears;
    std::uint32_t lastValue = 0;
    std::uint32_t historyVersion = 0;
    std::int32_t historyValue = 0;
    std::map<std::uint32_t, std::vector<Want>> history;  // past wants, by want GUID
    // Bytes after the history. The game never reads them, and many saves have stale data there.
    std::size_t extraBytes = 0;
};

WantsAndFears parse(std::span<const std::uint8_t> data);

// A Sim's want and fear slots and how many wants it may hold, as the game reads them: the saved
// slot counts (4 and 3 before version 2) and one hold more than the saved extra holds (none
// before version 4). Saved wants past the slots are dropped.
struct Slots {
    std::uint32_t wants = 4, fears = 3;
    std::uint32_t holds = 1;
};
Slots slots(const WantsAndFears& saved);

constexpr std::uint32_t kType = 0xCD95548E;
constexpr std::uint32_t kWrittenVersion = 6;
constexpr std::uint32_t kWrittenWantVersion = 10;
constexpr std::uint32_t kWrittenHistoryVersion = 2;

// Every byte of a Sim's wants and fears as the game writes them now (version 6, each want version 10,
// the history version 2), to be written back. The history keeps its groups in the game's order (by
// want GUID).
struct Record {
    std::optional<Want> extra;
    std::uint32_t maxWants = 0;
    std::vector<Want> wants;
    std::uint32_t maxFears = 0;
    std::vector<Want> fears;
    std::int16_t lastValue = 0;  // the game keeps it in 16 bits and writes it widened
    std::int32_t historyValue = 0;
    std::vector<std::pair<std::uint32_t, std::vector<Want>>> history;
};

// Empty for another version of the record, a want or the history, a want type the game does not
// write, a value outside the bits the game keeps, or bytes cut short or left over. With `leftOver`,
// bytes after the history are allowed and their count is given there: the game's reader stops at
// the history's count, and some saves keep bytes after it whose origin is not known yet.
std::optional<Record> readRecord(std::span<const std::uint8_t> data, std::size_t* leftOver = nullptr);

// Throws std::runtime_error for a want type the game does not write or a list too long to write.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::swaf
