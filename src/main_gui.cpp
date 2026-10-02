// main_gui.cpp - Win32 front end. All real work happens in macaddr.cpp; this file only
// draws controls, validates input, and runs the slow adapter restart on a worker thread.
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <memory>
#include <thread>

#include "macaddr.h"

namespace {

enum : int { ID_LIST = 100, ID_EDIT, ID_RANDOM, ID_APPLY, ID_RESTORE, ID_REFRESH };
constexpr UINT WM_WORK_DONE = WM_APP + 1;  // lParam = WorkResult*, posted by the worker thread

struct WorkResult {
    bool ok;
    std::wstring message;
};

struct App {
    HWND wnd{}, list{}, edit{}, random{}, apply{}, restore{}, refresh{};
    HWND current{}, hardware{}, overrideLabel{}, status{};
    HFONT font{};
    int dpi = 96;
    std::vector<mac::Adapter> adapters;
    bool busy = false;
} g;

int px(int v) { return MulDiv(v, g.dpi, 96); }

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h),
                             g.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return c;
}

void setStatus(const std::wstring& s) { SetWindowTextW(g.status, s.c_str()); }

const mac::Adapter* selected() {
    int i = static_cast<int>(SendMessageW(g.list, LB_GETCURSEL, 0, 0));
    return (i >= 0 && i < static_cast<int>(g.adapters.size())) ? &g.adapters[i] : nullptr;
}

void setBusy(bool busy) {
    g.busy = busy;
    const mac::Adapter* a = selected();
    for (HWND h : {g.list, g.edit, g.random, g.refresh}) EnableWindow(h, !busy);
    EnableWindow(g.apply, !busy && a != nullptr);
    EnableWindow(g.restore, !busy && a != nullptr && a->overrideValue.has_value());
}

void showDetails() {
    const mac::Adapter* a = selected();
    std::wstring current = L"-", hardware = L"-", override_ = L"-";
    if (a) {
        current = mac::format(a->currentMac);
        hardware = a->permanentMac ? mac::format(*a->permanentMac) : L"not reported by this driver";
        override_ = a->overrideValue ? *a->overrideValue : L"none (hardware address in use)";
    }
    SetWindowTextW(g.current, current.c_str());
    SetWindowTextW(g.hardware, hardware.c_str());
    SetWindowTextW(g.overrideLabel, override_.c_str());
    setBusy(g.busy);
}

void refreshList() {
    const mac::Adapter* prev = selected();
    std::wstring keep = prev ? prev->netCfgInstanceId : L"";
    try {
        g.adapters = mac::enumerateAdapters();
    } catch (const mac::Error& e) {
        g.adapters.clear();
        setStatus(L"Could not list adapters: " + e.message);
    }
    SendMessageW(g.list, LB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < g.adapters.size(); ++i) {
        const auto& a = g.adapters[i];
        std::wstring row = a.connectionName + L"  -  " + a.description + (a.physical ? L"" : L"  (virtual)") +
                           (a.overrideValue ? L"   [changed]" : L"");
        SendMessageW(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        if (a.netCfgInstanceId == keep) select = static_cast<int>(i);
    }
    SendMessageW(g.list, LB_SETCURSEL, select, 0);
    if (g.adapters.empty()) setStatus(L"No enabled Ethernet or Wi-Fi adapters found.");
    showDetails();
}

// Runs `work` on a background thread so the window stays responsive while the adapter restarts.
void runAsync(std::function<void()> work, std::wstring successMessage) {
    setBusy(true);
    setStatus(L"Working... the adapter is being restarted, this takes a few seconds.");
    std::thread([work = std::move(work), successMessage = std::move(successMessage)] {
        auto* r = new WorkResult{true, successMessage};
        try {
            work();
        } catch (const mac::Error& e) {
            r->ok = false;
            r->message = e.message;
        }
        PostMessageW(g.wnd, WM_WORK_DONE, 0, reinterpret_cast<LPARAM>(r));
    }).detach();
}

void onApply() {
    const mac::Adapter* a = selected();
    if (!a) return;
    wchar_t text[64] = {};
    GetWindowTextW(g.edit, text, 64);
    auto m = mac::parse(text);
    if (!m) {
        MessageBoxW(g.wnd, L"Enter 12 hex digits, for example 02:1A:2B:3C:4D:5E.", L"Invalid MAC address", MB_ICONWARNING);
        return;
    }
    if (!mac::isUnicast(*m)) {
        MessageBoxW(g.wnd, L"The first octet must be even. An odd first octet is a multicast address and cannot be assigned to an adapter.",
                    L"Invalid MAC address", MB_ICONWARNING);
        return;
    }
    if (!mac::isLocallyAdministered(*m) &&
        MessageBoxW(g.wnd,
                    L"This address does not have the locally administered bit set (second hex digit 2, 6, A or E).\n\n"
                    L"Many Windows drivers, especially Wi-Fi, silently ignore such addresses. Apply it anyway?",
                    L"Warning", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;

    mac::Adapter adapter = *a;  // copy: the list may be refreshed while the worker runs
    mac::Mac value = *m;
    runAsync([adapter, value] { mac::setOverride(adapter, value); },
             L"Applied " + mac::format(value) + L" to " + adapter.connectionName + L".");
}

void onRestore() {
    const mac::Adapter* a = selected();
    if (!a || !a->overrideValue) return;
    std::wstring question = L"Remove the override on " + a->connectionName + L" and go back to the hardware address?";
    if (MessageBoxW(g.wnd, question.c_str(), L"Restore", MB_ICONQUESTION | MB_YESNO) != IDYES) return;

    mac::Adapter adapter = *a;
    runAsync([adapter] { mac::clearOverride(adapter); },
             L"Restored the hardware address on " + adapter.connectionName + L".");
}

void onCreate() {
    g.dpi = static_cast<int>(GetDpiForWindow(g.wnd));
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);

    make(L"STATIC", L"Network adapters", 0, 12, 10, 300, 18);
    g.list = make(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, 12, 30, 536, 140, ID_LIST, WS_EX_CLIENTEDGE);

    make(L"STATIC", L"Current MAC:", 0, 12, 182, 110, 18);
    g.current = make(L"STATIC", L"-", 0, 126, 182, 420, 18);
    make(L"STATIC", L"Hardware MAC:", 0, 12, 204, 110, 18);
    g.hardware = make(L"STATIC", L"-", 0, 126, 204, 420, 18);
    make(L"STATIC", L"Override:", 0, 12, 226, 110, 18);
    g.overrideLabel = make(L"STATIC", L"-", 0, 126, 226, 420, 18);

    make(L"STATIC", L"New MAC:", 0, 12, 262, 110, 18);
    g.edit = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP, 126, 258, 200, 24, ID_EDIT, WS_EX_CLIENTEDGE);
    g.random = make(L"BUTTON", L"Random", WS_TABSTOP, 334, 257, 90, 26, ID_RANDOM);

    g.apply = make(L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, 126, 296, 100, 28, ID_APPLY);
    g.restore = make(L"BUTTON", L"Restore original", WS_TABSTOP, 234, 296, 130, 28, ID_RESTORE);
    g.refresh = make(L"BUTTON", L"Refresh", WS_TABSTOP, 372, 296, 90, 28, ID_REFRESH);

    g.status = make(L"STATIC", L"", 0, 12, 338, 536, 36);

    refreshList();
    if (!mac::isElevated())
        setStatus(L"Not running as administrator: changes will fail. Right-click the exe and choose Run as administrator.");
}

LRESULT CALLBACK wndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.wnd = w;
        onCreate();
        return 0;

    case WM_COMMAND:
        if (g.busy) return 0;
        switch (LOWORD(wp)) {
        case ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) showDetails();
            return 0;
        case ID_RANDOM:
            SetWindowTextW(g.edit, mac::format(mac::randomLocallyAdministered()).c_str());
            return 0;
        case ID_APPLY: onApply(); return 0;
        case ID_RESTORE: onRestore(); return 0;
        case ID_REFRESH:
            refreshList();
            setStatus(L"");
            return 0;
        }
        return 0;

    case WM_WORK_DONE: {
        std::unique_ptr<WorkResult> r(reinterpret_cast<WorkResult*>(lp));
        setBusy(false);
        refreshList();
        setStatus(r->message);
        if (!r->ok) MessageBoxW(w, r->message.c_str(), L"MAC change failed", MB_ICONERROR);
        return 0;
    }

    // Let IsDialogMessage treat Enter as a click on Apply.
    case DM_GETDEFID: return MAKELONG(ID_APPLY, DC_HASDEFID);
    case DM_SETDEFID: return TRUE;

    case WM_SETCURSOR:
        if (g.busy) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (g.busy) {
            MessageBoxW(w, L"Please wait until the adapter has restarted.", L"Busy", MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(w);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"MacChangerMain";
    RegisterClassW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(560, dpi, 96), MulDiv(384, dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"MAC Address Changer", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    ShowWindow(w, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return static_cast<int>(m.wParam);
}
