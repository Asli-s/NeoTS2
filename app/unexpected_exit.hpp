#pragma once

#include "diag/report.hpp"

#include <filesystem>
#include <string>
#include <vector>

// Runs that ended without a word (Windows ended them at once, they were killed, the power went)
// can't report themselves. Each run leaves a marker while it runs. At the next start, a marker whose
// run is gone means it ended that way, and what it left behind (its log, what Windows noted) is
// put together into a crash folder like any other.
namespace app::crash {

// Marks this run: "running-<pid>.txt" in `folder`, with its version, start and log file. It goes
// when the program ends normally, or once a crash is reported. Returns its path (empty when it
// couldn't be written).
std::filesystem::path markRunning(const std::filesystem::path& folder, const std::string& version,
                                  const std::filesystem::path& log);

// The runs in `folder` that ended without removing their marker (and aren't still running), each
// put into "crash-<when>-unexpected" with crash.txt and, when Windows made one, its crash dump.
// Their markers go. Runs under a debugger are left out. Oldest first.
struct UnexpectedRun {
    std::filesystem::path folder;
    ts2::diag::UnexpectedExit exit;
};
std::vector<UnexpectedRun> findUnexpectedExits(const std::filesystem::path& folder);

// Tells the tester about one, in plain words, offering to send or show it.
void tell(const UnexpectedRun& run);

} // namespace app::crash
