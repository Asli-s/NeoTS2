#include "crash_dialog.hpp"

#include "report_upload.hpp"

#include "diag/log.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <commctrl.h>
#include <shellapi.h>
// clang-format on
#include <SDL3/SDL.h>
#include <objbase.h>

// Task dialogs need version 6 of the common controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace app::crash {

namespace {

constexpr int kSend = 100, kShowFiles = 101, kFirstChoice = 200;

std::wstring wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}


// What a choice does; true when the message is done (the report went off to be sent).
bool act(int choice, const Message& m) {
    if (choice == kSend && canSend(m.folder) && startSending(m.folder)) {
        return true;  // a fresh copy of NeoTS2 sends it and says how it went
    }
    if (choice == kSend) {
        open(wide(m.reportLink));  // no dump to send: a GitHub issue, and the files to drag in
    }
    open(m.folder.wstring());
    return false;
}

// The plain message box, when task dialogs aren't there.
void fallback(const Message& m) {
    if (m.shown) {
        m.shown();
    }
    const std::string text = m.text + "\n\nThe report was saved in:\n" + ts2::diag::utf8(m.folder);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kSend, "Send a report"},
        {0, kShowFiles, "Show files"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDCLOSE, m.close.c_str()},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = "NeoTS2";
    box.message = text.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    for (int asked = 0; asked < 10; ++asked) {
        int choice = IDCLOSE;
        if (!SDL_ShowMessageBox(&box, &choice) || choice == IDCLOSE || act(choice, m)) {
            return;
        }
    }
}

TASKDIALOGCONFIG dialog(const std::wstring& heading, const std::wstring& text) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"NeoTS2";
    config.pszMainIcon = MAKEINTRESOURCEW(1);  // the app's icon
    config.pszMainInstruction = heading.c_str();
    config.pszContent = text.c_str();
    // In front of everything, even a fullscreen game window that is left frozen behind it.
    config.pfCallback = [](HWND window, UINT note, WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
        if (note == TDN_CREATED) {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(window);
            if (const auto* m = reinterpret_cast<const Message*>(data); m && m->shown) {
                m->shown();
            }
        }
        return S_OK;
    };
    return config;
}

} // namespace

void open(const std::wstring& what) {
    // Waits until the browser or Explorer has the request: these programs end right after.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Lets the browser come to the front instead of opening behind our message.
    AllowSetForegroundWindow(ASFW_ANY);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = what.c_str();
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
}

void show(const Message& m) {
    const std::wstring heading = wide(m.heading), text = wide(m.text), details = wide(m.details), close = wide(m.close);
    const std::wstring send = canSend(m.folder)
                                  ? L"Send a report\nSends the saved report to the NeoTS2 developers. Nothing else is sent."
                                  : L"Send a report\nOpens a new report on GitHub in your browser, and the saved files to add to it.";
    const TASKDIALOG_BUTTON buttons[] = {
        {kSend, send.c_str()},
        {kShowFiles, L"Show the saved files\nOpens the folder with the report, to see what it holds."},
        {IDCLOSE, close.c_str()},
    };
    TASKDIALOGCONFIG config = dialog(heading, text);
    config.pszExpandedInformation = details.c_str();
    config.pszCollapsedControlText = L"Show details";
    config.pszExpandedControlText = L"Hide details";
    config.cButtons = 3;
    config.pButtons = buttons;
    config.nDefaultButton = kSend;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&m);
    for (int asked = 0; asked < 10; ++asked) {
        int choice = IDCLOSE;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) {
            fallback(m);
            return;
        }
        if ((choice != kSend && choice != kShowFiles) || act(choice, m)) {
            return;
        }
    }
}

int choose(const std::string& heading, const std::string& text, const std::vector<std::string>& choices) {
    const std::wstring h = wide(heading), t = wide(text);
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> buttons;
    for (const auto& c : choices) {
        labels.push_back(wide(c));
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        buttons.push_back({kFirstChoice + static_cast<int>(i), labels[i].c_str()});
    }
    TASKDIALOGCONFIG config = dialog(h, t);
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) {
        MessageBoxW(nullptr, (h + L"\n\n" + t).c_str(), L"NeoTS2", MB_OK | MB_ICONINFORMATION);
        return -1;
    }
    const int index = choice - kFirstChoice;
    return index >= 0 && index < static_cast<int>(choices.size()) ? index : -1;
}

} // namespace app::crash

#else

namespace app::crash {
void open(const std::wstring&) {}
void show(const Message&) {}
int choose(const std::string&, const std::string&, const std::vector<std::string>&) { return -1; }
} // namespace app::crash

#endif
