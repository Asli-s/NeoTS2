#pragma once

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
// one it opens a GitHub issue and the saved files beside it, to drag them in.
void show(const Message& message);

// Opens a page or folder in its usual program.
void open(const std::wstring& what);

// A question in the same style: a heading, a sentence, and choices (each "Title\nWhat it does",
// the note optional). Returns the index of the choice picked, or -1 when closed.
int choose(const std::string& heading, const std::string& text, const std::vector<std::string>& choices);

} // namespace app::crash
