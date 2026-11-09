#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// On Windows, turns a crash into a report testers can send: a folder "crash-<date>-<time>" with crash.txt
// (what happened, the stack, the last log lines) and the minidump neots2.dmp, then offers to send it.
// On Linux and macOS the crash catcher (crash_catcher.hpp) saves the minidump and the next start makes
// the report (unexpected_exit.hpp), and without the catcher install() does nothing. Failures Windows
// ends at once (__fastfail, such as Control Flow Guard or /GS) can't be caught and are reported on the next start.
namespace app::crash {

struct Settings {
    std::filesystem::path folder;  // where crash folders go
    std::string version;           // the build's commit
    bool dialog = true;            // ask the tester what to do (tests turn it off)
    std::filesystem::path runningMarker;  // this run's marker (see unexpected_exit.hpp), removed once reported
};

// Catches crashes from here on, on every thread.
void install(const Settings& settings);

// Reports the program as stuck when it is still running `seconds` from now, then ends it. Called
// once closing starts: closing waits for loads still running, and a wait that never ended would
// leave the program open with no window, holding the game's files.
void watchClosing(unsigned seconds);

// Crashes on purpose in one of the ways install() catches, for tests: "access", "stack", "abort",
// "terminate", "throw", "purecall", "parameter", "thread" or "twice" (two threads at once).
// "fastfail" ends the program the way install() can't catch. Returns only for an unknown kind.
void test(std::string_view kind);

} // namespace app::crash
