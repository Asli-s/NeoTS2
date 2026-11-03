#include "crash_catcher.hpp"

#include "diag/upload.hpp"

#ifdef NEOTS2_CRASHPAD
#include "client/crash_report_database.h"
#include "client/crashpad_client.h"
#include "client/settings.h"
#include "util/misc/uuid.h"
#include "util/net/http_body.h"
#include "util/net/http_transport.h"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <system_error>

namespace app::crash {

namespace fs = std::filesystem;

fs::path ownExecutable() {
    std::error_code ec;
#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return n > 0 && n < MAX_PATH ? fs::path(exe) : fs::path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (size == 0 || _NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    if (const auto end = path.find('\0'); end != std::string::npos) {
        path.resize(end);
    }
    const fs::path resolved = fs::weakly_canonical(path, ec);
    return ec ? fs::path(path) : resolved;
#else
    const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : resolved;
#endif
}

#ifdef NEOTS2_CRASHPAD

namespace {

constexpr double kTimeoutSeconds = 30;

base::FilePath crashpadPath(const fs::path& path) {
    return base::FilePath(path.string());
}

} // namespace

bool hasCatcher() {
    return true;
}

bool startCatcher(const fs::path& database, const std::string& version) {
    const fs::path handler = ownExecutable().parent_path() / "crashpad_handler";
    std::error_code ec;
    if (!fs::is_regular_file(handler, ec)) {
        return false;
    }
    fs::create_directories(database, ec);
    // Uploads stay off: a dump is only sent when the player chooses, by NeoTS2 itself.
    if (auto reports = crashpad::CrashReportDatabase::Initialize(crashpadPath(database))) {
        if (auto* settings = reports->GetSettings()) {
            settings->SetUploadsEnabled(false);
        }
    }
    // Kept for the whole run: the handler watches this process.
    static crashpad::CrashpadClient client;
    const std::map<std::string, std::string> annotations{{"product", std::string(ts2::diag::kReportApp)}, {"version", version}};
    return client.StartHandler(crashpadPath(handler), crashpadPath(database), crashpadPath(database), std::string(), std::string(),
                               annotations, {}, false, false);
}

std::vector<CaughtDump> caughtDumps(const fs::path& database) {
    std::vector<CaughtDump> out;
    const auto reports = crashpad::CrashReportDatabase::InitializeWithoutCreating(crashpadPath(database));
    if (!reports) {
        return out;
    }
    std::vector<crashpad::CrashReportDatabase::Report> found, completed;
    reports->GetPendingReports(&found);
    reports->GetCompletedReports(&completed);
    found.insert(found.end(), completed.begin(), completed.end());
    for (const auto& report : found) {
        out.push_back({report.uuid.ToString(), fs::path(report.file_path.value()), report.creation_time});
    }
    return out;
}

void forgetDump(const fs::path& database, const std::string& id) {
    crashpad::UUID uuid;
    if (!uuid.InitializeFromString(id)) {
        return;
    }
    if (const auto reports = crashpad::CrashReportDatabase::InitializeWithoutCreating(crashpadPath(database))) {
        reports->DeleteReport(uuid);
    }
}

std::optional<std::string> post(const std::string& url, const std::string& contentType, const std::string& body) {
    auto transport = crashpad::HTTPTransport::Create();
    if (!transport || url.rfind("https://", 0) != 0) {
        return std::nullopt;
    }
    transport->SetURL(url);
    transport->SetMethod("POST");
    transport->SetHeader("Content-Type", contentType);
    transport->SetHeader("Content-Length", std::to_string(body.size()));
    transport->SetBodyStream(std::make_unique<crashpad::StringHTTPBodyStream>(body));
    transport->SetTimeout(kTimeoutSeconds);
    std::string reply;
    if (!transport->ExecuteSynchronously(&reply)) {
        return std::nullopt;
    }
    return reply;
}

#else

bool hasCatcher() {
    return false;
}

bool startCatcher(const fs::path&, const std::string&) {
    return false;
}

std::vector<CaughtDump> caughtDumps(const fs::path&) {
    return {};
}

void forgetDump(const fs::path&, const std::string&) {}

std::optional<std::string> post(const std::string&, const std::string&, const std::string&) {
    return std::nullopt;
}

#endif

} // namespace app::crash
