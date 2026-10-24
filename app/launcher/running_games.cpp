#include "launcher/running_games.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <cwctype>
#include <string>
#endif

namespace ts2::launcher {

#ifdef _WIN32

namespace {

bool isOriginalGame(std::wstring name) {
    for (wchar_t& c : name) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    if (name == L"sims2.exe") {
        return true;
    }
    if (name.size() < 12 || name.compare(0, 5, L"sims2") != 0 || name.compare(name.size() - 4, 4, L".exe") != 0) {
        return false;
    }
    // Sims2EP<n>.exe or Sims2SP<n>.exe, with a pack number of one or two digits.
    const std::wstring middle = name.substr(5, name.size() - 9);
    if (middle.size() > 4 || (middle.compare(0, 2, L"ep") != 0 && middle.compare(0, 2, L"sp") != 0)) {
        return false;
    }
    for (std::size_t i = 2; i < middle.size(); ++i) {
        if (!std::iswdigit(middle[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool originalGameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        found = isOriginalGame(entry.szExeFile);
    }
    CloseHandle(snapshot);
    return found;
}

bool otherLauncherOpen() {
    // Windows closes the handle when the launcher ends, so the name is free again then.
    static const bool other = [] {
        const HANDLE mark = CreateMutexW(nullptr, FALSE, L"Local\\NeoTS2Launcher");
        return mark != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    }();
    return other;
}

#else

bool originalGameRunning() {
    return false;
}

bool otherLauncherOpen() {
    return false;
}

#endif

} // namespace ts2::launcher
