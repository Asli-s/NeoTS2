#include "diag/report.hpp"

#include <cstdio>
#include <stdexcept>

namespace ts2::diag {

namespace {

constexpr std::size_t kMaxLink = 2000;
constexpr std::size_t kLinkFrames = 8;

std::string hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(v));
    return text;
}

} // namespace

std::string exceptionName(std::uint32_t code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC00000FD: return "stack overflow";
    case 0xC0000094: return "integer division by zero";
    case 0xC0000095: return "integer overflow";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000096: return "privileged instruction";
    case 0xC0000006: return "in-page error (a file read failed)";
    case 0xC0000409: return "security check failed (stack buffer overrun or fail-fast)";
    case 0xC0000374: return "heap corruption";
    case 0xC000008C: return "array bounds exceeded";
    case 0xC000008E: return "float division by zero";
    case 0xE06D7363: return "C++ exception not caught";
    case 0xE0000001: return "std::terminate";
    case 0xE0000002: return "abort";
    case 0xE0000003: return "pure virtual call";
    case 0xE0000004: return "invalid parameter to a C runtime function";
    default: return "exception " + hex(code);
    }
}

std::string describe(const Frame& frame) {
    std::string s = (frame.module.empty() ? std::string("?") : frame.module) + "+" + hex(frame.offset);
    if (!frame.function.empty()) {
        s += "  " + frame.function;
    }
    if (!frame.source.empty()) {
        s += "  (" + frame.source + ")";
    }
    return s;
}

std::string crashText(const Crash& crash, const std::vector<std::string>& recentLines) {
    std::string t;
    t += "NeoTS2 crashed\n\n";
    t += "Version: " + crash.version + "\n";
    t += "When: " + crash.time + "\n";
    t += "What: " + crash.kind + " (" + hex(crash.code) + ")\n";
    t += "Where: " + describe(crash.where) + "\n\n";
    t += "Stack (most recent call first):\n";
    for (const auto& f : crash.stack) {
        t += "  " + describe(f) + "\n";
    }
    t += "\nLast log lines:\n";
    for (const auto& line : recentLines) {
        t += "  " + line + "\n";
    }
    t += "\nWhat these files hold, for you to decide what to send:\n"
         "  crash.txt   this text. The log lines can name files and folders on your computer\n"
         "              (your own user folder is written as %USERPROFILE%, so not your user name).\n"
         "  neots2.dmp  a small minidump: the program's threads, their stacks and registers, and\n"
         "              which modules were loaded. It holds some of the program's memory near\n"
         "              those stacks, which can include paths and names of files it had open.\n"
         "Nothing is sent anywhere by NeoTS2; attach them to a report only if you want to.\n";
    return t;
}

std::string fileName(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string withoutUserFolder(std::string_view text, std::string_view userFolder) {
    while (!userFolder.empty() && (userFolder.back() == '\\' || userFolder.back() == '/')) {
        userFolder.remove_suffix(1);
    }
    if (userFolder.size() < 3) {
        return std::string(text);  // "C:" alone would match every path
    }
    auto same = [](char a, char b) {
        auto fold = [](char c) { return c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return fold(a) == fold(b);
    };
    auto partOfName = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
               static_cast<unsigned char>(c) >= 0x80;
    };
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        bool match = i + userFolder.size() <= text.size() && (i + userFolder.size() == text.size() || !partOfName(text[i + userFolder.size()]));
        for (std::size_t k = 0; match && k < userFolder.size(); ++k) {
            match = same(text[i + k], userFolder[k]);
        }
        if (match) {
            out += "%USERPROFILE%";
            i += userFolder.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

std::string linkEncoded(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char escaped[4];
            std::snprintf(escaped, sizeof escaped, "%%%02X", c);
            out += escaped;
        }
    }
    return out;
}

std::string issueLink(const Crash& crash) {
    // Module file names only: a frame's module could have come with a folder.
    auto frame = [](Frame f) {
        f.module = fileName(f.module);
        f.source = fileName(f.source);
        return describe(f);
    };
    const std::string title = "Crash: " + crash.kind + " at " + frame(crash.where) + " (" + crash.version + ")";
    std::string body = "**Version:** " + crash.version + "\n**What:** " + crash.kind + " (" + hex(crash.code) + ")\n\n**Stack:**\n```\n";
    std::string link;
    for (std::size_t n = 0;; ++n) {
        std::string b = body;
        for (std::size_t i = 0; i < crash.stack.size() && i < kLinkFrames - n && n < kLinkFrames; ++i) {
            b += frame(crash.stack[i]) + "\n";
        }
        b += "```\n\n**What I was doing:**\n\n\nPlease attach crash.txt and neots2.dmp from the crash folder that opened "
             "(or that the message named), after reading what they hold at the end of crash.txt.\n";
        link = std::string(kIssuesPage) + "?title=" + linkEncoded(title) + "&body=" + linkEncoded(b);
        if (link.size() <= kMaxLink || n >= kLinkFrames) {
            break;  // fewer frames until it fits
        }
    }
    return link.size() <= kMaxLink ? link : std::string(kIssuesPage) + "?title=" + linkEncoded(title.substr(0, 200));
}

std::map<std::string, std::string> eventFields(std::string_view xml) {
    // <Data Name='ExceptionCode'>c0000409</Data> (Windows quotes with ' ; " is taken too)
    std::map<std::string, std::string> fields;
    constexpr std::string_view open = "<Data Name=";
    for (std::size_t at = xml.find(open); at != std::string_view::npos; at = xml.find(open, at)) {
        at += open.size();
        if (at >= xml.size() || (xml[at] != '\'' && xml[at] != '"')) {
            continue;
        }
        const char quote = xml[at++];
        const auto nameEnd = xml.find(quote, at);
        const auto valueStart = nameEnd == std::string_view::npos ? nameEnd : xml.find('>', nameEnd);
        if (valueStart == std::string_view::npos || xml[valueStart - 1] == '/') {
            continue;  // <Data Name="x"/>: empty
        }
        const auto valueEnd = xml.find("</Data>", valueStart);
        if (valueEnd == std::string_view::npos) {
            break;
        }
        fields[std::string(xml.substr(at, nameEnd - at))] = std::string(xml.substr(valueStart + 1, valueEnd - valueStart - 1));
        at = valueEnd;
    }
    return fields;
}

Crash crashOf(const UnexpectedExit& exit) {
    Crash c;
    c.version = exit.version;
    c.time = exit.started;
    c.kind = "closed unexpectedly";
    const auto field = [&](const char* name) {
        const auto it = exit.windows.find(name);
        return it != exit.windows.end() ? it->second : std::string();
    };
    try {
        if (const std::string code = field("ExceptionCode"); !code.empty()) {
            c.code = static_cast<std::uint32_t>(std::stoul(code, nullptr, 16));
            c.kind = exceptionName(c.code);
        }
        if (const std::string offset = field("FaultingOffset"); !offset.empty()) {
            c.where.offset = std::stoull(offset, nullptr, 16);
        }
    } catch (const std::exception&) {
        // not numbers: left out
    }
    c.where.module = fileName(field("ModuleName"));
    return c;
}

std::string unexpectedExitText(const UnexpectedExit& exit, const std::vector<std::string>& lastLines) {
    std::string t = "NeoTS2 closed unexpectedly\n\n";
    t += "The run that started " + exit.started + " ended without a word: Windows ended it at once (some\n"
         "crashes can't be caught from inside), or it was closed from outside (Task Manager, shutting\n"
         "down), or the power went. This was found when NeoTS2 started again.\n\n";
    t += "Version: " + exit.version + "\n";
    if (exit.windows.empty()) {
        t += "Windows noted no crash for it (so it was most likely closed from outside).\n";
    } else {
        const Crash c = crashOf(exit);
        t += "What (as Windows noted it): " + c.kind + "\nWhere: " + describe(c.where) + "\n\nWindows' note:\n";
        for (const auto& [name, value] : exit.windows) {
            t += "  " + name + ": " + value + "\n";
        }
    }
    t += "\nLast lines of that run's log:\n";
    for (const auto& line : lastLines) {
        t += "  " + line + "\n";
    }
    t += "\nWhat these files hold, for you to decide what to send:\n"
         "  crash.txt   this text. The log lines can name files and folders on your computer\n"
         "              (your own user folder is written as %USERPROFILE%, so not your user name).\n"
         "  neots2.dmp  only when Windows made a crash dump for that run (if it is set to): the\n"
         "              program's threads and some of its memory, which can include such paths too.\n"
         "Nothing is sent anywhere by NeoTS2; attach them to a report only if you want to.\n";
    return t;
}

} // namespace ts2::diag
