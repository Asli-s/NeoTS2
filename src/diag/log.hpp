#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The run's log: what testers send with a problem. Every line goes to the console and, once
// open() has run, to a file of its own per run (the last kKeptLogs are kept). The last lines are
// also kept in memory for crash reports, in fixed buffers a crash handler can read.
namespace ts2::diag {

constexpr std::size_t kKeptLogs = 10;
constexpr std::size_t kRecentLines = 200;
constexpr std::uintmax_t kMaxLogBytes = 16ull << 20;  // a run's log file stops growing there
constexpr std::size_t kLineLength = 240;  // longer lines are cut in the recent lines (not in the file)

enum class Level { Info, Warning, Error, Step };

// Starts this run's log file in `folder` ("neots2-<date>-<time>.log"), its first lines `header`,
// and removes the oldest logs beyond kKeptLogs. False when the folder can't be written (lines then go
// to the console only).
[[nodiscard]] bool open(const std::filesystem::path& folder, std::string_view header, std::uintmax_t maxBytes = kMaxLogBytes);
// The open log file, empty when none.
std::filesystem::path file();

// A line of the log. Thread-safe.
void write(Level level, std::string_view text);
inline void info(std::string_view text) { write(Level::Info, text); }
inline void warning(std::string_view text) { write(Level::Warning, text); }
inline void error(std::string_view text) { write(Level::Error, text); }
// A step the program took (a lot opened, a simulation built): the trail that leads up to a crash.
inline void step(std::string_view text) { write(Level::Step, text); }
// A warning written only the first time the line calling it is reached, with that file name and
// line, for things only broken data should cause (a safety limit hit): the program carries on, and
// the log isn't flooded when it happens every frame. The calling line comes from the compiler's
// built-ins, because older clang, Apple's included, gives std::source_location::current() the line
// of this declaration instead.
void warnOnce(std::string_view text, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE());

// The last lines, oldest first. Takes no lock and allocates nothing until it copies them out. A
// line being written at that moment may come out partly written.
std::vector<std::string> recentLines();
// The same into a caller's buffer (for crash handlers): lines separated by '\n', cut to fit.
// Returns the length written.
std::size_t recentLines(char* out, std::size_t size);

// Whether a file or folder name is exactly one this program makes (anything else in the folder is
// left alone): "neots2-20261012-153000.log", "crash-20261012-153000" or
// "crash-20261012-153000-unexpected-1234", "running-1234.txt".
bool isLogName(std::string_view name);
bool isReportFolderName(std::string_view name);
bool isMarkerName(std::string_view name);

// A path as UTF-8 text. (path::string() uses the old Windows code page and throws on characters
// it can't hold, such as a Chinese user name on an English Windows.)
std::string utf8(const std::filesystem::path& path);

// Forgets the recent lines and closes the file (for tests).
void reset();

} // namespace ts2::diag
