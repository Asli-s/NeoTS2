#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Turns a crash into a report testers can send: a folder "crash-<date>-<time>" with crash.txt (what
// happened, the stack, the last log lines) and neots2.dmp (a small minidump), then a message offering
// to send it or show the files. Windows only for now; elsewhere install() does nothing.
// Failures Windows ends at once (__fastfail: Control Flow Guard or /GS stack cookie violations) can't
// be caught from inside the program; the next start reports those (see unexpected_exit.hpp).
namespace app::crash {

struct Settings {
    std::filesystem::path folder;  // where crash folders go
    std::string version;           // the build's commit
    bool dialog = true;            // ask the tester what to do (tests turn it off)
    std::filesystem::path runningMarker;  // this run's marker (see unexpected_exit.hpp), removed once reported
};

// Catches crashes from here on, on every thread.
void install(const Settings& settings);

// Crashes on purpose in one of the ways install() catches, for tests: "access", "stack", "abort",
// "terminate", "throw", "purecall", "parameter", "thread" or "twice" (two threads at once); "fastfail" ends the program the way
// install() can't catch. Returns only for an unknown kind.
void test(std::string_view kind);

} // namespace app::crash
