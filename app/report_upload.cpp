#include "report_upload.hpp"

#include "crash_dialog.hpp"
#include "diag/report.hpp"
#include "diag/upload.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <bcrypt.h>
#include <commctrl.h>
#include <winhttp.h>
// clang-format on

namespace app::crash {

namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxText = 4ull << 20;
constexpr std::size_t kMaxResponse = 1 << 20;
constexpr int kTimeoutMs = 30'000;

std::wstring wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::optional<std::vector<std::uint8_t>> readFile(const fs::path& file, std::uintmax_t most) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > most) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        return std::nullopt;
    }
    return data;
}

// Closes a WinHTTP handle when it goes.
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) {
            WinHttpCloseHandle(h);
        }
    }
};

// The request being made, so Cancel can close it: WinHTTP then ends the call that is waiting on it.
// Whoever takes it out of here closes it, so it is closed once.
std::atomic<HINTERNET> inFlight{nullptr};

struct InFlight {
    Handle& request;
    explicit InFlight(Handle& r) : request(r) { inFlight.store(r.h); }
    ~InFlight() {
        HINTERNET mine = request.h;
        if (!inFlight.compare_exchange_strong(mine, nullptr)) {
            request.h = nullptr;  // Cancel closed it
        }
    }
};

void abortRequest() {
    if (HINTERNET h = inFlight.exchange(nullptr)) {
        WinHttpCloseHandle(h);
    }
}

struct Response {
    DWORD status = 0;
    std::string body;
};

// One HTTPS request (nothing else is allowed). Nullopt when it couldn't be made at all.
std::optional<Response> request(const wchar_t* method, const std::string& url, const std::wstring& headers, std::string_view body) {
    const std::wstring link = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(link.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return std::nullopt;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                              (parts.lpszExtraInfo ? std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength) : std::wstring());
    Handle session{WinHttpOpen(L"NeoTS2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) {
        session.h = WinHttpOpen(L"NeoTS2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        return std::nullopt;
    }
    WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    Handle connection{WinHttpConnect(session.h, host.c_str(), parts.nPort, 0)};
    Handle req{connection.h ? WinHttpOpenRequest(connection.h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr};
    const InFlight cancellable(req);
    if (!req.h || body.size() > MAXDWORD ||
        !WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        return std::nullopt;
    }
    Response r;
    DWORD size = sizeof r.status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    for (DWORD available = 0; WinHttpQueryDataAvailable(req.h, &available) && available > 0;) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req.h, chunk.data(), available, &read) || read == 0) {
            break;
        }
        r.body.append(chunk, 0, read);
        if (r.body.size() > kMaxResponse) {
            break;  // not what these endpoints send
        }
    }
    return r;
}

std::string md5(const std::vector<std::uint8_t>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[16] = {};
    const bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) == 0 &&
                    BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                    BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0 &&
                    BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    std::string hex;
    for (const unsigned char b : digest) {
        static const char digits[] = "0123456789abcdef";
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return ok ? hex : std::string();
}

// The version a crash.txt names ("Version: ..."), or `fallback`.
std::string versionOf(const std::vector<std::uint8_t>& text, const std::string& fallback) {
    const std::string s(text.begin(), text.end());
    const auto at = s.find("\nVersion: ");
    if (at == std::string::npos) {
        return fallback;
    }
    const auto end = s.find_first_of("\r\n", at + 10);
    const std::string v = s.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 10);
    return v.empty() || v.size() > 64 ? fallback : v;
}

struct Sent {
    bool ok = false;
    bool reached = false;  // BugSplat answered (so a failure wasn't the connection)
    std::string crashId;
};

// The three steps BugSplat asks for: an upload link, the zip put there, then the upload committed.
Sent send(const fs::path& folder, const std::string& fallbackVersion, const std::atomic<bool>& cancelled) {
    Sent sent;
    auto dump = readFile(folder / "neots2.dmp", kMaxSentDump);
    const auto text = readFile(folder / "crash.txt", kMaxText);
    if (!dump) {
        return sent;
    }
    const std::string version = text ? versionOf(*text, fallbackVersion) : fallbackVersion;
    std::vector<ts2::diag::ZipFile> files{{"neots2.dmp", std::move(*dump)}};
    if (text) {
        files.push_back({"crash.txt", *text});
    }
    const auto zipped = ts2::diag::zip(files);
    if (zipped.empty()) {
        return sent;
    }
    using ts2::diag::linkEncoded;
    const std::string site = "https://" + std::string(ts2::diag::kReportDatabase) + ".bugsplat.com/api/";
    const auto asked = request(L"GET", site + "getCrashUploadUrl?database=" + linkEncoded(ts2::diag::kReportDatabase) +
                                           "&appName=" + linkEncoded(ts2::diag::kReportApp) + "&appVersion=" + linkEncoded(version) +
                                           "&crashPostSize=" + std::to_string(zipped.size()),
                               L"", "");
    sent.reached = asked.has_value();
    const std::string upload = asked && asked->status == 200 ? ts2::diag::jsonString(asked->body, "url") : std::string();
    if (upload.rfind("https://", 0) != 0 || cancelled) {
        return sent;
    }
    files.clear();
    const auto put = request(L"PUT", upload, L"Content-Type: application/octet-stream\r\n",
                             std::string_view(reinterpret_cast<const char*>(zipped.data()), zipped.size()));
    if (!put || put->status != 200) {
        sent.reached = put.has_value();
        return sent;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));  // BugSplat asks for a second between requests
    if (cancelled) {
        return sent;  // uploaded but not committed, so BugSplat doesn't process it
    }
    const std::string boundary = "----NeoTS2Report" + md5(zipped).substr(0, 16);
    const std::string form = ts2::diag::formBody({{"database", std::string(ts2::diag::kReportDatabase)},
                                                   {"appName", std::string(ts2::diag::kReportApp)},
                                                   {"appVersion", version},
                                                   {"crashType", "Windows.Native"},
                                                   {"crashTypeId", "1"},
                                                   {"s3key", upload},
                                                   {"md5", md5(zipped)}},
                                                  boundary);
    const auto committed = request(L"POST", site + "commitS3CrashUpload",
                                   wide("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"), form);
    sent.reached = committed.has_value();
    if (!committed || committed->status != 200) {
        return sent;
    }
    sent.ok = true;
    // crashId is a number in the reply ("crashId":123). jsonString reads strings, so it's read here.
    const std::string& reply = committed->body;
    const auto at = reply.find("\"crashId\"");
    const auto colon = at == std::string::npos ? at : reply.find(':', at);
    if (colon != std::string::npos) {
        std::size_t i = colon + 1;
        while (i < reply.size() && (reply[i] == ' ' || reply[i] == '"')) {
            ++i;
        }
        for (; i < reply.size() && reply[i] >= '0' && reply[i] <= '9' && sent.crashId.size() < 20; ++i) {
            sent.crashId += reply[i];
        }
    }
    return sent;
}

// Sends on a worker thread behind a small "Sending..." message that closes itself when done.
// Cancel stops before the next step, so a cancelled report is never committed.
struct Sending {
    std::atomic<bool> done{false}, cancelled{false};
    Sent sent;
};

void sendShowingProgress(const fs::path& folder, const std::string& version, Sending& state) {
    std::thread worker([&] {
        state.sent = send(folder, version, state.cancelled);
        state.done = true;
    });
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"NeoTS2";
    config.pszMainIcon = MAKEINTRESOURCEW(1);
    config.pszMainInstruction = L"Sending the report";
    config.pszContent = L"This takes a few seconds.";
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&state);
    config.pfCallback = [](HWND window, UINT note, WPARAM button, LPARAM, LONG_PTR data) -> HRESULT {
        auto& s = *reinterpret_cast<Sending*>(data);
        if (note == TDN_CREATED) {
            SendMessageW(window, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
        } else if (note == TDN_TIMER && s.done) {
            SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
        } else if (note == TDN_BUTTON_CLICKED && button == IDCANCEL && !s.done) {
            s.cancelled = true;
            abortRequest();
        }
        return S_OK;
    };
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    worker.join();  // after a cancel this is quick: the request's handle was closed
}

std::string issuePage(const std::string& title, const std::string& body) {
    return std::string(ts2::diag::kIssuesPage) + "?title=" + ts2::diag::linkEncoded(title) + "&body=" + ts2::diag::linkEncoded(body);
}

// A report already sent keeps a note of it, so it isn't sent twice.
const fs::path kSentNote = "sent.txt";

} // namespace

bool canSend(const fs::path& folder) {
    std::error_code ec;
    const fs::path dump = folder / "neots2.dmp";
    return !folder.empty() && fs::is_regular_file(dump, ec) && fs::file_size(dump, ec) <= kMaxSentDump && !ec;
}

bool startSending(const fs::path& folder) {
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
        return false;
    }
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --send-report \"" + folder.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int sendAndTell(const fs::path& folder, const std::string& version) {
    std::error_code ec;
    if (fs::exists(folder / kSentNote, ec)) {
        choose("This report was already sent", "Thank you, it reached us before.", {"Close"});
        return 0;
    }
    if (!canSend(folder)) {
        const int choice = choose("This report can't be sent from here",
                                  "Its crash dump is missing or too big. It is still saved on this computer.",
                                  {"Report on GitHub instead\nOpens a new report in your browser, and the saved files to add to it.", "Close"});
        if (choice == 0) {
            open(wide(issuePage("Crash report", "**What I was doing:**\n\n\nPlease attach crash.txt from the folder that opened.\n")));
            open(folder.wstring());
        }
        return 1;
    }
    for (int tries = 0; tries < 5; ++tries) {
        Sending state;
        sendShowingProgress(folder, version, state);
        if (state.sent.ok) {
            std::ofstream(folder / kSentNote) << "crashId=" << state.sent.crashId << "\n";
            const std::string id = state.sent.crashId.empty() ? std::string() : " (report " + state.sent.crashId + ")";
            const int choice = choose("Thank you", "The report was sent" + id + ". It helps fix the problem.",
                                      {"Tell us more on GitHub\nOpens a page in your browser to say what you were doing. Optional.", "Close"});
            if (choice == 0) {
                open(wide(issuePage("Crash" + id, "NeoTS2 sent a crash report" + id + ".\n\n**What I was doing:**\n\n")));
            }
            return 0;
        }
        if (state.cancelled) {
            return 1;  // the tester chose not to send it
        }
        const std::string why = state.sent.reached ? "The report service didn't take it just now; it may be busy."
                                                   : "Maybe there's no internet connection right now.";
        const int choice = choose("The report couldn't be sent", why + " The report is still saved on this computer.",
                                  {"Try again", "Report on GitHub instead\nOpens a new report in your browser, and the saved files to add to it.",
                                   "Close"});
        if (choice == 1) {
            open(wide(issuePage("Crash report", "**What I was doing:**\n\n\nPlease attach crash.txt and neots2.dmp from the folder that opened.\n")));
            open(folder.wstring());
        }
        if (choice != 0) {
            return 1;
        }
    }
    return 1;
}

} // namespace app::crash

#else

namespace app::crash {
bool canSend(const std::filesystem::path&) { return false; }
bool startSending(const std::filesystem::path&) { return false; }
int sendAndTell(const std::filesystem::path&, const std::string&) { return 1; }
} // namespace app::crash

#endif
