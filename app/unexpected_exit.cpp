#include "unexpected_exit.hpp"

#include "crash_dialog.hpp"
#include "diag/log.hpp"
#include "report_upload.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winevt.h>
#endif

namespace app::crash {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kLastLines = 64;
constexpr std::uintmax_t kMaxMarkerBytes = 4096;
constexpr unsigned long kEventWaitMs = 2000;
constexpr std::size_t kMaxEvents = 400;               // looked through, newest first
constexpr std::uint32_t kMaxEventBytes = 1 << 16;             // one event as XML is a few KB

fs::path marker;  // this run's, removed at a normal end

void removeMarker() {
    std::error_code ec;
    fs::remove(marker, ec);
}

std::map<std::string, std::string> readFields(const fs::path& file) {
    std::map<std::string, std::string> fields;
    std::error_code ec;
    if (fs::file_size(file, ec) > kMaxMarkerBytes || ec) {
        return fields;  // not one of ours
    }
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (const auto eq = line.find('='); eq != std::string::npos) {
            fields[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return fields;
}

// The last lines of a log file (it was written line by line, so it holds everything up to the end).
std::vector<std::string> lastLines(const fs::path& log) {
    std::deque<std::string> lines;
    std::ifstream in(log);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
        if (lines.size() > kLastLines) {
            lines.pop_front();
        }
    }
    return {lines.begin(), lines.end()};
}

#ifdef _WIN32

std::uint64_t creationTime(HANDLE process) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// Whether the process a marker names is still this same run (pids are reused).
bool stillRunning(std::uint32_t pid, std::uint64_t created) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE && creationTime(process) == created;
    CloseHandle(process);
    return alive;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The user's own folder, which reports don't name.
std::string userFolder() {
    wchar_t profile[MAX_PATH] = {};
    return GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0 ? utf8(profile) : std::string();
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return s;
}

// What Windows noted in its event log about the crash of process `pid` that started at `created`:
// an "Application Error" event names the process and its start, so it is that run's for sure.
// The fields holding full paths are left out.
std::map<std::string, std::string> windowsNote(std::uint32_t pid, std::uint64_t created) {
    EVT_HANDLE query = EvtQuery(nullptr, L"Application", L"*[System[Provider[@Name='Application Error'] and (EventID=1000)]]",
                                EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query) {
        return {};
    }
    char pidText[16], createdText[24];
    std::snprintf(pidText, sizeof pidText, "0x%x", pid);
    std::snprintf(createdText, sizeof createdText, "0x%llx", static_cast<unsigned long long>(created));
    std::map<std::string, std::string> note;
    EVT_HANDLE events[16];
    DWORD got = 0;
    for (std::size_t seen = 0; note.empty() && seen < kMaxEvents && EvtNext(query, 16, events, kEventWaitMs, 0, &got); seen += got) {
        for (DWORD i = 0; i < got; ++i) {
            DWORD used = 0, properties = 0;
            EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &properties);
            std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
            if (note.empty() && used > 0 && used < kMaxEventBytes &&
                EvtRender(nullptr, events[i], EvtRenderEventXml, static_cast<DWORD>(xml.size() * sizeof(wchar_t)), xml.data(), &used,
                          &properties)) {
                auto fields = ts2::diag::eventFields(utf8(xml.c_str()));
                if (lower(fields["ProcessId"]) == pidText && lower(fields["ProcessCreationTime"]) == createdText) {
                    fields.erase("AppPath");
                    fields.erase("ModulePath");
                    note = std::move(fields);
                }
            }
            EvtClose(events[i]);
        }
    }
    EvtClose(query);
    return note;
}

// The crash dump Windows made of that run, when it is set to make them (its "local dumps", by
// default in the user's CrashDumps folder, named after the program and its pid).
fs::path windowsDump(std::uint32_t pid, std::uint64_t created) {
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) {
        return {};
    }
    const fs::path dump = fs::path(local) / L"CrashDumps" / (L"neots2.exe." + std::to_wstring(pid) + L".dmp");
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(dump.c_str(), GetFileExInfoStandard, &data)) {
        return {};
    }
    const std::uint64_t written = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return written >= created && size <= kMaxSentDump ? dump : fs::path();
}

#endif

std::string localTimeName(std::string_view started) {
    // "2026-10-12 20:02:25" -> "20261012-200225"
    std::string out;
    for (const char c : started) {
        if (c >= '0' && c <= '9') {
            out += c;
        } else if (c == ' ') {
            out += '-';
        }
    }
    return out.empty() ? "unknown" : out;
}

} // namespace

fs::path markRunning(const fs::path& folder, const std::string& version, const fs::path& log) {
#ifdef _WIN32
    SYSTEMTIME now;
    GetLocalTime(&now);
    char started[32];
    std::snprintf(started, sizeof started, "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond);
    marker = folder / ("running-" + std::to_string(GetCurrentProcessId()) + ".txt");
    std::ofstream out(marker);
    out << "pid=" << GetCurrentProcessId() << "\ncreated=" << creationTime(GetCurrentProcess()) << "\nversion=" << version
        << "\nstarted=" << started << "\nlog=" << ts2::diag::utf8(log.filename()) << "\ndebugger=" << (IsDebuggerPresent() ? 1 : 0)
        << "\n";
    if (!out) {
        marker.clear();
        return {};
    }
    std::atexit(removeMarker);
    return marker;
#else
    (void)folder;
    (void)version;
    (void)log;
    return {};
#endif
}

std::vector<UnexpectedRun> findUnexpectedExits(const fs::path& folder) {
    std::vector<UnexpectedRun> found;
#ifdef _WIN32
    std::vector<fs::path> markers;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
        const std::string name = ts2::diag::utf8(it->path().filename());
        if (ts2::diag::isMarkerName(name)) {
            markers.push_back(it->path());
        }
    }
    for (const auto& file : markers) {
        auto fields = readFields(file);
        std::uint32_t pid = 0;
        std::uint64_t created = 0;
        try {
            pid = static_cast<std::uint32_t>(std::stoul(fields["pid"]));
            created = std::stoull(fields["created"]);
        } catch (const std::exception&) {
            // unreadable: treated as gone
        }
        if (pid != 0 && stillRunning(pid, created)) {
            continue;  // another copy of NeoTS2, running now
        }
        fs::remove(file, ec);
        if (fields["debugger"] == "1") {
            continue;  // stopped in a debugger, most likely
        }
        UnexpectedRun run;
        run.exit.version = fields["version"];
        run.exit.started = fields["started"];
        run.exit.windows = windowsNote(pid, created);
        run.folder = folder / ("crash-" + localTimeName(run.exit.started) + "-unexpected-" + std::to_string(pid));
        fs::create_directories(run.folder, ec);
        // Only a log name of ours is read: anything else in a marker could point at another file.
        const std::string log = fields["log"];
        const auto lines = ts2::diag::isLogName(log) ? lastLines(folder / log) : std::vector<std::string>{};
        std::ofstream(run.folder / "crash.txt") << ts2::diag::withoutUserFolder(ts2::diag::unexpectedExitText(run.exit, lines), userFolder());
        if (const fs::path dump = windowsDump(pid, created); !dump.empty()) {
            fs::copy_file(dump, run.folder / "neots2.dmp", fs::copy_options::overwrite_existing, ec);
        }
        ts2::diag::warning("the run started " + run.exit.started + " ended unexpectedly; its report is in " + ts2::diag::utf8(run.folder));
        found.push_back(std::move(run));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.exit.started < b.exit.started; });
#else
    (void)folder;
#endif
    return found;
}

void tell(const UnexpectedRun& run) {
    const auto crash = ts2::diag::crashOf(run.exit);
    Message m;
    m.heading = "NeoTS2 closed unexpectedly last time";
    m.text = "If you closed it yourself, you can ignore this. What it noted before closing was saved on this computer. "
             "Sending it to us helps fix the problem; nothing is sent unless you choose to.";
    m.close = "Close and continue to the game";
    m.details = crash.kind + (crash.where.module.empty() ? std::string() : " at " + ts2::diag::describe(crash.where)) +
                "\nVersion: " + run.exit.version + "\nStarted: " + run.exit.started + "\nSaved in: " + ts2::diag::utf8(run.folder);
    m.folder = run.folder;
    m.reportLink = ts2::diag::issueLink(crash);
    show(m);
}

} // namespace app::crash
