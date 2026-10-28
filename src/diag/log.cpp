#include "diag/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>

namespace ts2::diag {

namespace {

struct RecentLine {
    std::array<char, kLineLength> text{};
};

std::mutex mutex;  // guards the file and the order lines are added in
std::ofstream out;
std::uintmax_t fileBytes = 0, maxFileBytes = kMaxLogBytes;
std::filesystem::path path;
std::array<RecentLine, kRecentLines> recent;
std::atomic<std::size_t> written{0};  // lines added so far, the next going to written % kRecentLines

const char* name(Level level) {
    switch (level) {
    case Level::Info: return "info";
    case Level::Warning: return "warning";
    case Level::Error: return "error";
    case Level::Step: return "step";
    }
    return "?";
}

std::tm localTime(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// "13:05:42.123"
std::string timeOfDay() {
    const auto now = std::chrono::system_clock::now();
    const std::tm tm = localTime(std::chrono::system_clock::to_time_t(now));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return text;
}

void removeOldLogs(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> logs;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
        const auto file = utf8(it->path().filename());
        if (isLogName(file)) {
            logs.push_back(it->path());
        }
    }
    std::sort(logs.begin(), logs.end());  // the names sort by date and time
    for (std::size_t i = 0; i + kKeptLogs < logs.size(); ++i) {
        std::filesystem::remove(logs[i], ec);
    }
}

} // namespace

bool open(const std::filesystem::path& folder, std::string_view header, std::uintmax_t maxBytes) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::tm tm = localTime(std::time(nullptr));
    char name[64];
    std::snprintf(name, sizeof name, "neots2-%04d%02d%02d-%02d%02d%02d.log", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    {
        std::lock_guard lock(mutex);
        out.close();
        out.clear();
        path = folder / name;
        out.open(path, std::ios::out | std::ios::app);
        if (!out) {
            path.clear();
            return false;
        }
        out << header << '\n';
        fileBytes = header.size() + 1;
        maxFileBytes = maxBytes;
        out.flush();
    }
    removeOldLogs(folder);
    return true;
}

std::filesystem::path file() {
    std::lock_guard lock(mutex);
    return path;
}

void write(Level level, std::string_view text) {
    const std::string line = timeOfDay() + " " + name(level) + ": " + std::string(text);
    std::lock_guard lock(mutex);
    std::fprintf(level == Level::Error || level == Level::Warning ? stderr : stdout, "%s\n", line.c_str());
    if (out && fileBytes + line.size() + 1 > maxFileBytes) {
        out << "(the log is full; later lines are only kept for crash reports)\n";
        out.close();
    }
    if (out) {
        out << line << '\n';
        out.flush();  // on disk at once: the next line might never come
        fileBytes += line.size() + 1;
    }
    auto& slot = recent[written.load(std::memory_order_relaxed) % kRecentLines].text;
    const std::size_t n = std::min(line.size(), slot.size() - 1);
    std::copy_n(line.data(), n, slot.data());
    slot[n] = '\0';
    written.fetch_add(1, std::memory_order_release);
}

std::size_t recentLines(char* buffer, std::size_t size) {
    if (buffer == nullptr || size == 0) {
        return 0;
    }
    const std::size_t total = written.load(std::memory_order_acquire);
    const std::size_t first = total > kRecentLines ? total - kRecentLines : 0;
    std::size_t used = 0;
    for (std::size_t i = first; i < total; ++i) {
        const auto& text = recent[i % kRecentLines].text;
        for (std::size_t c = 0; c < text.size() && text[c] != '\0' && used + 1 < size; ++c) {
            buffer[used++] = text[c];
        }
        if (used + 1 < size) {
            buffer[used++] = '\n';
        }
    }
    buffer[used] = '\0';
    return used;
}

std::vector<std::string> recentLines() {
    std::vector<std::string> lines;
    std::lock_guard lock(mutex);
    const std::size_t total = written.load(std::memory_order_acquire);
    for (std::size_t i = total > kRecentLines ? total - kRecentLines : 0; i < total; ++i) {
        lines.emplace_back(recent[i % kRecentLines].text.data());
    }
    return lines;
}

namespace {

// `text` starts with `n` digits at `at`.
bool digits(std::string_view text, std::size_t at, std::size_t n) {
    if (at + n > text.size()) {
        return false;
    }
    for (std::size_t i = at; i < at + n; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

// "<prefix>YYYYMMDD-HHMMSS", how much of `name` that takes (0 when it doesn't start so).
std::size_t stamped(std::string_view name, std::string_view prefix) {
    const std::size_t p = prefix.size();
    return name.substr(0, p) == prefix && digits(name, p, 8) && name.size() > p + 8 && name[p + 8] == '-' && digits(name, p + 9, 6)
               ? p + 15
               : 0;
}

bool numberThen(std::string_view rest, std::string_view end) {
    std::size_t n = 0;
    while (n < rest.size() && n < 10 && rest[n] >= '0' && rest[n] <= '9') {
        ++n;
    }
    return n > 0 && rest.substr(n) == end;
}

} // namespace

bool isLogName(std::string_view name) {
    const std::size_t at = stamped(name, "neots2-");
    return at > 0 && name.substr(at) == ".log";
}

bool isReportFolderName(std::string_view name) {
    const std::size_t at = stamped(name, "crash-");
    constexpr std::string_view unexpected = "-unexpected-";
    return at > 0 && (at == name.size() || (name.substr(at, unexpected.size()) == unexpected && numberThen(name.substr(at + unexpected.size()), "")));
}

bool isMarkerName(std::string_view name) {
    constexpr std::string_view running = "running-";
    return name.substr(0, running.size()) == running && numberThen(name.substr(running.size()), ".txt");
}

std::string utf8(const std::filesystem::path& of) {
    const std::u8string text = of.u8string();
    return std::string(text.begin(), text.end());
}

namespace {
std::mutex warnedMutex;
// The calling lines that warned already, in a fixed table so that warning never allocates. A file's
// name is the same text in memory each time it is reached from one compiled file, so it is told apart
// by where it is (a header's line reached from two files may warn twice). Past the table's end no more
// lines warn, which the few dozen safety limits never reach.
struct WarnedLine {
    const char* file = nullptr;
    std::uint_least32_t line = 0;
};
constexpr std::size_t kMaxWarnedLines = 256;
std::array<WarnedLine, kMaxWarnedLines> warned;
std::size_t warnedCount = 0;
} // namespace

void warnOnce(std::string_view text, const char* file, unsigned line) {
    {
        std::lock_guard lock(warnedMutex);
        const auto end = warned.begin() + static_cast<std::ptrdiff_t>(warnedCount);
        const bool seen = std::any_of(warned.begin(), end, [&](const WarnedLine& w) {
            return w.file == file && w.line == line;
        });
        if (seen || warnedCount == kMaxWarnedLines) {
            return;
        }
        warned[warnedCount++] = {file, line};
    }
    // The file's name only: the folder it was built in says nothing useful and can hold a user name.
    std::string_view name = file != nullptr ? file : "";
    name = name.substr(name.find_last_of("/\\") + 1);
    warning(std::format("{} ({}:{})", text, name, line));
}

void reset() {
    {
        std::lock_guard lock(warnedMutex);
        warnedCount = 0;
    }
    std::lock_guard lock(mutex);
    out.close();
    out.clear();
    std::filesystem::path().swap(path);  // frees it too
    for (auto& line : recent) {
        line.text[0] = '\0';
    }
    written.store(0, std::memory_order_release);
}

} // namespace ts2::diag
