#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// The message testers see after a crash: plain words, the technical part folded away, and a choice
// to send the report, see the saved files, or close.
namespace app::crash {

struct Message {
    std::string heading;
    std::string text;
    std::string details;  // folded away: what happened, where, the version
    std::filesystem::path folder;
    std::string reportLink;        // a new GitHub issue with the crash filled in
    std::string close = "Close";   // the button that ends the message
    void (*shown)() = nullptr;     // called once the message is on screen
};

// Shows the message until the tester closes it (seeing the files keeps it up). Sending a report
// sends it from a fresh copy of NeoTS2 (see report_upload.hpp) when it holds a crash dump. Without
// one it opens a GitHub issue and the saved files beside it, to drag them in. On Linux and macOS
// it is the system's message box, which has no room for the details, and follows the system's light
// or dark look where the system draws it.
void show(const Message& message);

// Opens a web page (UTF-8) or a folder in its usual program.
void openLink(const std::string& url);
void openFolder(const std::filesystem::path& folder);

// A small window with `heading`, `text` and a moving bar, up until `done` (Linux and macOS, whose
// message boxes can't stay up while work goes on). With no display it only waits.
void showWhile(const std::string& heading, const std::string& text, const std::atomic<bool>& done);

// A question in the same style: a heading, a sentence, and choices (each "Title\nWhat it does",
// the note optional, and left out where the system's message box has no room for it). Returns the
// index of the choice picked, or -1 when closed.
int choose(const std::string& heading, const std::string& text, const std::vector<std::string>& choices);

} // namespace app::crash
