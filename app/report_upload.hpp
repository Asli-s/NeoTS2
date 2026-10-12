#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Sending a saved crash report to BugSplat's NeoTS2 database, only when the tester picks "Send a
// report". It is done by a fresh copy of NeoTS2 ("--send-report <folder>"), never by the program
// that crashed (its memory may be damaged), which uploads the folder's minidump and crash.txt and
// says how it went. Windows only for now.
namespace app::crash {

// The largest crash dump that is sent; bigger ones (Windows' own can be) stay on the computer.
constexpr std::uintmax_t kMaxSentDump = 256ull << 20;

// Whether a saved report can be sent this way: it holds a minidump no bigger than kMaxSentDump.
bool canSend(const std::filesystem::path& folder);

// Starts the copy that sends `folder`'s report; false when it couldn't be started.
bool startSending(const std::filesystem::path& folder);

// What that copy does: sends the report (showing that it is), then says how it went, offering to
// try again or report it on GitHub instead. `version` is used when the report doesn't name one.
// Returns the program's exit code (0 when sent).
int sendAndTell(const std::filesystem::path& folder, const std::string& version);

} // namespace app::crash
