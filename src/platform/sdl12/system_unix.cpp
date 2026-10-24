// What SDL 1.2 doesn't give of the system, for Mac OS X and other Unix systems: the program's
// folders, message boxes and the computer's details.
#include "platform/platform.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#include <sys/utsname.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

namespace ts2::platform {

namespace {

std::filesystem::path home() {
    const char* h = std::getenv("HOME");
    return h && *h ? std::filesystem::path(h) : std::filesystem::path();
}

} // namespace

#ifdef __APPLE__

void showMessageBox(MessageKind kind, const std::string& title, const std::string& text, Window*) {
    // CoreFoundation's own alert, which needs no window of ours and is there on every Mac OS X.
    const auto make = [](const std::string& s) {
        CFStringRef made = CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
        return made ? made : CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingISOLatin1);
    };
    CFStringRef header = make(title);
    CFStringRef message = make(text);
    const CFOptionFlags level = kind == MessageKind::Error     ? kCFUserNotificationStopAlertLevel
                                : kind == MessageKind::Warning ? kCFUserNotificationCautionAlertLevel
                                                               : kCFUserNotificationNoteAlertLevel;
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(0, level, nullptr, nullptr, nullptr, header, message, nullptr, nullptr, nullptr, &response);
    if (header) {
        CFRelease(header);
    }
    if (message) {
        CFRelease(message);
    }
}

std::filesystem::path basePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path = std::string(path.c_str());  // without the padding after its end
    char resolved[PATH_MAX];
    const std::filesystem::path program = realpath(path.c_str(), resolved) ? std::filesystem::path(resolved) : std::filesystem::path(path);
    return program.parent_path() / "";
}

std::filesystem::path prefPath(const char* application) {
    const std::filesystem::path h = home();
    if (h.empty()) {
        return {};
    }
    const std::filesystem::path folder = h / "Library" / "Application Support" / application;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return error ? std::filesystem::path() : folder / "";
}

SystemInfo systemInfo() {
    SystemInfo info;
    utsname un{};
    uname(&un);
    info.name = std::string("Mac OS X (Darwin ") + un.release + ")";
    // Tiger has no _SC_NPROCESSORS_ONLN for sysconf.
    int cpus = 0;
    std::size_t length = sizeof cpus;
    info.cores = sysctlbyname("hw.ncpu", &cpus, &length, nullptr, 0) == 0 && cpus > 0 ? cpus : 1;
    std::uint64_t bytes = 0;
    length = sizeof bytes;
    if (sysctlbyname("hw.memsize", &bytes, &length, nullptr, 0) != 0) {
        std::uint32_t small = 0;
        length = sizeof small;
        bytes = sysctlbyname("hw.physmem", &small, &length, nullptr, 0) == 0 ? small : 0;
    }
    info.memoryMb = static_cast<int>(bytes >> 20);
    return info;
}

#else

void showMessageBox(MessageKind kind, const std::string& title, const std::string& text, Window*) {
    // No box of ours here: the message is said where the program was started from.
    std::fprintf(stderr, "%s (%s): %s\n", title.c_str(), kind == MessageKind::Error ? "error" : kind == MessageKind::Warning ? "warning" : "note",
                 text.c_str());
}

std::filesystem::path basePath() {
    char path[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) {
        return {};
    }
    path[n] = '\0';
    return std::filesystem::path(path).parent_path() / "";
}

std::filesystem::path prefPath(const char* application) {
    std::filesystem::path base;
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) {
        base = data;
    } else if (const std::filesystem::path h = home(); !h.empty()) {
        base = h / ".local" / "share";
    } else {
        return {};
    }
    const std::filesystem::path folder = base / application;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return error ? std::filesystem::path() : folder / "";
}

SystemInfo systemInfo() {
    SystemInfo info;
    utsname un{};
    uname(&un);
    info.name = std::string(un.sysname) + " " + un.release;
    info.cores = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    info.memoryMb = static_cast<int>((static_cast<std::uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE))) >> 20);
    return info;
}

#endif

std::filesystem::path documentsFolder() {
    const std::filesystem::path h = home();
    return h.empty() ? std::filesystem::path() : h / "Documents";
}

} // namespace ts2::platform
