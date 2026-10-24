#include "crash_handler.hpp"

#include "crash_dialog.hpp"

#include "diag/log.hpp"
#include "diag/report.hpp"

#include <exception>
#include <stdexcept>
#include <thread>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <intrin.h>

namespace app::crash {

namespace {

// Our own codes for the failures that aren't Windows exceptions, raised so that one filter
// handles everything (the names are in diag::exceptionName).
constexpr DWORD kTerminate = 0xE0000001, kAbort = 0xE0000002, kPureCall = 0xE0000003, kInvalidParameter = 0xE0000004,
                kClosingStuck = 0xE0000005;
constexpr DWORD kReportTimeoutMs = 5'000;  // the crashing thread waits this long for the files (they take well under a second)
constexpr DWORD kShowTimeoutMs = 10'000;  // the message must appear by then (it can't, while some lock is held)
constexpr std::size_t kMaxFrames = 48;

std::wstring folder, marker;
std::string version;
bool dialog = true;
HANDLE request = nullptr, written = nullptr, shown = nullptr, finished = nullptr;
DWORD helperThread = 0;
EXCEPTION_POINTERS* pointers = nullptr;
DWORD crashedThread = 0;
std::atomic<bool> crashing{false};
std::wstring reportFolder;  // filled in by the helper
ts2::diag::Crash report;

std::string utf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The user's own folder, which reports don't name.
std::string userFolder() {
    wchar_t profile[MAX_PATH] = {};
    return GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0 ? utf8(profile) : std::string();
}

// The module an address is in and the offset into it, and with symbols its function and line.
ts2::diag::Frame frameAt(HANDLE process, DWORD64 address) {
    ts2::diag::Frame f;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        wchar_t name[MAX_PATH] = {};
        GetModuleFileNameW(module, name, MAX_PATH);
        f.module = ts2::diag::fileName(utf8(name));
        f.offset = address - reinterpret_cast<DWORD64>(module);
    } else {
        f.offset = address;
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        f.function = symbol->Name;
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line) && line.FileName) {
        f.source = ts2::diag::fileName(line.FileName) + ":" + std::to_string(line.LineNumber);
    }
    return f;
}

// The crashed thread's stack, from the registers it crashed with.
void walkStack(HANDLE process, const CONTEXT& crashed, DWORD thread) {
#ifdef _M_X64
    CONTEXT context = crashed;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    HANDLE handle = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, thread);
    for (std::size_t i = 0; i < kMaxFrames && handle; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, handle, &frame, &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        report.stack.push_back(frameAt(process, frame.AddrPC.Offset));
    }
    if (handle) {
        CloseHandle(handle);
    }
#else
    (void)process;
    (void)crashed;
    (void)thread;
#endif
}

bool writeFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD done = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    CloseHandle(file);
    return ok && done == text.size();
}

// The recent log lines, read without taking the log's lock (the crash may have happened inside it).
std::vector<std::string> recentLines() {
    static char buffer[ts2::diag::kRecentLines * (ts2::diag::kLineLength + 1) + 1];
    const std::size_t n = ts2::diag::recentLines(buffer, sizeof buffer);
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (buffer[i] == '\n') {
            lines.emplace_back(buffer + start, i - start);
            start = i + 1;
        }
    }
    return lines;
}

void writeReport() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf_s(name, L"crash-%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    if (folder.empty()) {
        // No log folder could be written: the user's temporary folder, never the drive's root.
        wchar_t temp[MAX_PATH] = {};
        const DWORD n = GetTempPathW(MAX_PATH, temp);
        folder = std::wstring(temp, n > 0 && n < MAX_PATH ? n : 0) + L"NeoTS2";
    }
    reportFolder = folder + L"\\" + name;
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW(reportFolder.c_str(), nullptr);

    // The minidump first: it is the most useful file, and the rest may not get written.
    HANDLE process = GetCurrentProcess();
    HANDLE dump = CreateFileW((reportFolder + L"\\neots2.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{crashedThread, pointers, FALSE};
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(process, GetCurrentProcessId(), dump, type, &info, nullptr, nullptr);
        CloseHandle(dump);
    }
    if (!marker.empty()) {
        DeleteFileW(marker.c_str());  // reported: the next start needn't, even if the rest isn't written in time
    }

    // Symbols from beside the exe, when a build's .pdb files are there.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exeFolder = exe;
    exeFolder = exeFolder.substr(0, exeFolder.find_last_of(L"\\/"));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const bool symbols = SymInitializeW(process, exeFolder.c_str(), TRUE) != FALSE;

    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    report.version = version;
    report.code = code;
    report.kind = ts2::diag::exceptionName(code);
    char time[32];
    std::snprintf(time, sizeof time, "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    report.time = time;
    report.where = frameAt(process, reinterpret_cast<DWORD64>(pointers->ExceptionRecord->ExceptionAddress));
    walkStack(process, *pointers->ContextRecord, crashedThread);
    // The C runtime's terminate calls abort itself (its own handler is kept per thread).
    if (code == kAbort) {
        for (const auto& f : report.stack) {
            if (f.function == "terminate") {
                report.kind = "std::terminate";
            }
        }
    }
    if (symbols) {
        SymCleanup(process);
    }
    writeFile(reportFolder + L"\\crash.txt", ts2::diag::withoutUserFolder(ts2::diag::crashText(report, recentLines()), userFolder()));
}

// Tells the tester, in plain words, and offers to send or show the report.
void ask() {
    ClipCursor(nullptr);  // the game may have held the mouse inside its window
    Message m;
    m.heading = "NeoTS2 had to close";
    m.text = "Something went wrong inside NeoTS2. A report was saved on this computer. Sending it to us helps fix the "
             "problem; nothing is sent unless you choose to.";
    m.details = report.kind + " at " + ts2::diag::describe(report.where) + "\nVersion: " + version + "\nSaved in: " +
                utf8(reportFolder);
    m.folder = std::filesystem::path(reportFolder);
    m.reportLink = ts2::diag::issueLink(report);
    m.shown = [] { SetEvent(shown); };
    show(m);
}

// Waits on its own (healthy) stack for a crash to report.
DWORD WINAPI helper(void*) {
    WaitForSingleObject(request, INFINITE);
    writeReport();
    SetEvent(written);
    if (dialog) {
        ask();
    }
    SetEvent(finished);
    return 0;
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* p) {
    if (crashing.exchange(true)) {
        if (GetCurrentThreadId() == helperThread) {
            TerminateProcess(GetCurrentProcess(), 3);  // the report itself crashed: end now
        }
        Sleep(INFINITE);  // another thread crashed meanwhile: the first crash's handler ends the program
    }
    pointers = p;
    crashedThread = GetCurrentThreadId();
    SetEvent(request);
    // Little stack is used here: after a stack overflow there is little left.
    if (WaitForSingleObject(written, kReportTimeoutMs) == WAIT_OBJECT_0 && dialog) {
        // Once the message is up the tester takes their time. If it never appears, end anyway.
        const HANDLE either[] = {shown, finished};
        if (WaitForMultipleObjects(2, either, FALSE, kShowTimeoutMs) == WAIT_OBJECT_0) {
            WaitForSingleObject(finished, INFINITE);
        }
    }
    TerminateProcess(GetCurrentProcess(), p->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void raise(DWORD code) {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::_Exit(3);
}

void onTerminate() { raise(kTerminate); }
void onAbort(int) { raise(kAbort); }
void onPureCall() { raise(kPureCall); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) { raise(kInvalidParameter); }

// Endless on purpose (the compiler's warning about that is turned off here).
#pragma warning(push)
#pragma warning(disable : 4717)
#pragma optimize("", off)
void overflow(volatile int depth) {
    volatile char page[4096];
    page[0] = static_cast<char>(depth);
    overflow(depth + 1);
    page[1] = page[0];
}
#pragma optimize("", on)
#pragma warning(pop)

} // namespace

void install(const Settings& settings) {
    folder = settings.folder.wstring();
    marker = settings.runningMarker.wstring();
    version = settings.version;
    dialog = settings.dialog;
    request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    written = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    shown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = request && written && shown && finished ? CreateThread(nullptr, 256 * 1024, helper, nullptr, 0, &helperThread) : nullptr;
    if (!thread) {
        ts2::diag::warning("crash reports are off: the crash handler couldn't start");
        return;
    }
    CloseHandle(thread);  // it runs on and nothing waits for it
    // Room for the handler to run on this thread after a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(onCrash);
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
}

void watchClosing(unsigned seconds) {
    std::thread([seconds] {
        Sleep(seconds * 1000);
        ts2::diag::error("closing has taken more than " + std::to_string(seconds) + " s: reported as stuck");
        RaiseException(kClosingStuck, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }).detach();
}

void test(std::string_view kind) {
    ts2::diag::step("crashing on purpose: " + std::string(kind));
    if (kind == "access") {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    } else if (kind == "stack") {
        overflow(0);
    } else if (kind == "abort") {
        std::abort();
    } else if (kind == "terminate") {
        std::terminate();
    } else if (kind == "throw") {
        throw 42;  // nothing catches an int
    } else if (kind == "purecall") {
        _purecall();
    } else if (kind == "parameter") {
        _get_errno(nullptr);
    } else if (kind == "fastfail") {
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);  // Windows ends it at once: only the next start can tell
    } else if (kind == "twice") {
        // Two threads crashing almost together: the first one's report must still be written.
        std::thread other([] {
            volatile int* nowhere = nullptr;
            *nowhere = 3;
        });
        volatile int* nowhere = nullptr;
        *nowhere = 4;
        other.join();
    } else if (kind == "thread") {
        std::thread([] {
            volatile int* nowhere = nullptr;
            *nowhere = 2;
        }).join();
    }
}

} // namespace app::crash

#else

namespace app::crash {

void install(const Settings&) {}
void watchClosing(unsigned) {}
void test(std::string_view kind) {
    if (kind == "access") {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    } else if (kind == "abort") {
        std::abort();
    } else if (kind == "terminate") {
        std::terminate();
    }
}

} // namespace app::crash

#endif
