#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The crash catcher on Linux and macOS: Crashpad (Apache-2.0), whose small crashpad_handler
// program runs beside NeoTS2 and writes a minidump when it crashes. It never sends anything itself.
// The next start finds the dump and asks the player (see unexpected_exit.hpp). Everything that
// talks to Crashpad is here. In builds without it these do nothing.
namespace app::crash {

// The catcher's own folder for its dumps, inside the log folder.
constexpr const char* kCatcherFolder = "crashpad";

// Whether this build has the crash catcher.
bool hasCatcher();

// Starts crashpad_handler (beside the running program) with its dumps in `database`. False when
// it couldn't be started, and crashes then go unreported.
bool startCatcher(const std::filesystem::path& database, const std::string& version);

// A crash dump the catcher saved and nobody has taken yet.
struct CaughtDump {
    std::string id;
    std::filesystem::path file;
    std::time_t when = 0;
};
std::vector<CaughtDump> caughtDumps(const std::filesystem::path& database);
// Removes a dump from the catcher's folder once it is copied into a report.
void forgetDump(const std::filesystem::path& database, const std::string& id);

// One HTTPS POST. The reply's body when the server took it (status 200 to 203), nothing otherwise.
std::optional<std::string> post(const std::string& url, const std::string& contentType, const std::string& body);

// The running program's own file, empty when unknown.
std::filesystem::path ownExecutable();

} // namespace app::crash
