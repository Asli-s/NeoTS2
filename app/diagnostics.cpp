#include "diagnostics.hpp"

#include "crash_handler.hpp"
#include "report_upload.hpp"
#include "unexpected_exit.hpp"
#include "diag/log.hpp"
#include "neots2_version.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace app {

namespace {

std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

// Whether files can be made in a folder (made if missing): a program under Program Files can't.
bool writable(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto probe = folder / ".write-test";
    {
        std::ofstream out(probe);
        if (!out) {
            return false;
        }
    }
    std::filesystem::remove(probe, ec);
    return true;
}

// Keeps the newest crash folders (their names sort by date and time), as the log keeps its logs.
void removeOldReports(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> reports;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
        if (it->is_directory(ec) && ts2::diag::isReportFolderName(ts2::diag::utf8(it->path().filename()))) {
            reports.push_back(it->path());
        }
    }
    std::sort(reports.begin(), reports.end());
    for (std::size_t i = 0; i + ts2::diag::kKeptLogs < reports.size(); ++i) {
        std::filesystem::remove_all(reports[i], ec);
    }
}

std::filesystem::path logFolder() {
    if (const char* base = SDL_GetBasePath()) {
        const auto beside = utf8Path(base) / "logs";
        if (writable(beside)) {
            return beside;
        }
    }
    std::filesystem::path folder;
#ifdef _WIN32
    // Logs stay on this computer: the local app data folder, not the roaming one.
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
        return std::filesystem::path(local) / L"NeoTS2" / L"logs";
    }
#endif
    if (char* data = SDL_GetPrefPath(nullptr, "NeoTS2")) {
        folder = utf8Path(data) / "logs";
        SDL_free(data);
    }
    return folder;
}

} // namespace

void startDiagnostics(int& argc, char** argv) {
#ifdef _WIN32
    // Sharp at any display scaling: without this, Windows draws our messages small and stretches
    // them (blurry). SDL asks for the same when it starts; this covers what comes before.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    std::filesystem::path folder;
    bool dialog = true;
    std::string crashTest;
    // Take our options out, keeping the others in order.
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--log-folder" && i + 1 < argc) {
            folder = utf8Path(argv[++i]);
        } else if (arg == "--no-crash-dialog") {
            dialog = false;
        } else if (arg == "--crash-test" && i + 1 < argc) {
            crashTest = argv[++i];
        } else if (arg == "--send-report" && i + 1 < argc) {
            // This copy only sends a saved report (started by a crash message), then ends.
            std::exit(crash::sendAndTell(utf8Path(argv[i + 1]), NEOTS2_VERSION));
        } else {
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    argv[argc] = nullptr;

    if (folder.empty()) {
        folder = logFolder();
    }
    std::string header = std::string("NeoTS2 ") + NEOTS2_VERSION + "\ncommand line:";
    for (int i = 0; i < argc; ++i) {
        header += std::string(" \"") + argv[i] + "\"";
    }
    if (folder.empty() || !ts2::diag::open(folder, header)) {
        ts2::diag::warning("no log file: its folder can't be written");
    }
    // Runs that ended without a word, before this one leaves its own marker.
    const auto unexpected = folder.empty() ? std::vector<crash::UnexpectedRun>{} : crash::findUnexpectedExits(folder);
    const auto marker = folder.empty() ? std::filesystem::path() : crash::markRunning(folder, NEOTS2_VERSION, ts2::diag::file());
    crash::install({folder, NEOTS2_VERSION, dialog, marker});
    ts2::diag::info(std::string("NeoTS2 ") + NEOTS2_VERSION + ", logging to " + ts2::diag::utf8(ts2::diag::file()));
    if (!unexpected.empty() && dialog) {
        crash::tell(unexpected.back());
    }
    removeOldReports(folder);
    if (!crashTest.empty()) {
        crash::test(crashTest);
        ts2::diag::error("unknown crash test: " + crashTest);
    }
}

} // namespace app
