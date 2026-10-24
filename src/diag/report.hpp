#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// What a crash report says, put together from what a crash handler found. Kept apart from the
// handler (which is platform code) so it can be tested.
namespace ts2::diag {

constexpr std::string_view kIssuesPage = "https://github.com/Asli-s/NeoTS2/issues/new";

struct Frame {
    std::string module;    // the module's file name (no folder)
    std::uint64_t offset = 0;  // from the module's start
    std::string function;  // when symbols were found, empty otherwise
    std::string source;    // "file.cpp:123" when known
};

struct Crash {
    std::string version;  // the build's commit
    std::string kind;     // e.g. "access violation", "std::terminate"
    std::uint32_t code = 0;
    Frame where;          // the crashing instruction
    std::vector<Frame> stack;
    std::string time;     // when, as the tester's clock had it
};

// The name of a Windows exception code, or "exception 0x...".
std::string exceptionName(std::uint32_t code);

// "neots2.exe+0x1a2b3c", with the function when known.
std::string describe(const Frame& frame);

// The whole crash.txt: what happened, the stack, the last log lines and what the files hold.
std::string crashText(const Crash& crash, const std::vector<std::string>& recentLines);

// A link to a new GitHub issue with the crash's version, kind and stack filled in: module names
// and offsets only, never folders, file paths or log lines (those stay in the files the tester
// chooses to attach). At most about 2,000 characters.
std::string issueLink(const Crash& crash);

// Text made safe for a link's query (UTF-8, everything but letters, digits and -_.~ escaped).
std::string linkEncoded(std::string_view text);

// Only the file name of a path, whichever slashes it uses.
std::string fileName(std::string_view path);

// The text with the user's own folder (their profile folder, any case, either slash) written as
// %USERPROFILE%, so a report doesn't say whose computer it came from. Only whole folder names
// match: a folder whose name only starts with theirs is left alone.
std::string withoutUserFolder(std::string_view text, std::string_view userFolder);

// A run that ended without a word (Windows ended it at once, it was killed, the power went),
// found at the next start from the marker it left.
struct UnexpectedExit {
    std::string version;
    std::string started;
    // What Windows noted of the crash in its event log ("Application Error"), by field name
    // ("ExceptionCode", "ModuleName", "FaultingOffset"...). Empty when it noted none.
    std::map<std::string, std::string> windows;
};

// The named fields of an event as Windows renders it in XML: each <Data Name="...">value</Data>.
std::map<std::string, std::string> eventFields(std::string_view xml);

// The crash an unexpected exit amounts to, for the issue link: what Windows noted when it noted
// one ("closed unexpectedly" otherwise).
Crash crashOf(const UnexpectedExit& exit);

// The crash.txt for an unexpected exit: what is known, the run's last log lines, what the files hold.
std::string unexpectedExitText(const UnexpectedExit& exit, const std::vector<std::string>& lastLines);

} // namespace ts2::diag
