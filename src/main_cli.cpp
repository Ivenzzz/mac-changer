// main_cli.cpp - command-line front end. Handy for scripting, and for testing the core
// without the GUI. It has no elevation manifest, so `list` works from a normal console and
// `set` / `restore` need a console that was opened as administrator.
#include <windows.h>

#include <cstdlib>
#include <string>

#include "macaddr.h"

namespace {

// Writes UTF-16 to a real console and UTF-8 when redirected, so text is right in both cases.
void print(const std::wstring& s) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), utf8.data(), n, nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

int usage() {
    print(L"usage:\n"
          L"  macchanger-cli list\n"
          L"  macchanger-cli set <N> <mac>    apply <mac> to adapter N from 'list' (administrator)\n"
          L"  macchanger-cli restore <N>      remove the override on adapter N (administrator)\n");
    return 2;
}

const mac::Adapter& pick(const std::vector<mac::Adapter>& list, const wchar_t* arg) {
    wchar_t* end = nullptr;
    long n = wcstol(arg, &end, 10);
    if (*end || n < 1 || n > static_cast<long>(list.size()))
        throw mac::Error{L"No adapter with index " + std::wstring(arg) + L". Run 'list' first.", 0};
    return list[static_cast<size_t>(n - 1)];
}

void requireAdmin() {
    if (!mac::isElevated()) throw mac::Error{L"Changing a MAC address needs a console opened as administrator.", 0};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::wstring cmd = argc > 1 ? argv[1] : L"";
        auto adapters = mac::enumerateAdapters();

        if (cmd == L"list") {
            if (adapters.empty()) {
                print(L"No enabled Ethernet or Wi-Fi adapters found.\n");
                return 0;
            }
            for (size_t i = 0; i < adapters.size(); ++i) {
                const auto& a = adapters[i];
                print(std::to_wstring(i + 1) + L". " + a.connectionName + L"  (" + a.description +
                      (a.wireless ? L", Wi-Fi" : L"") + (a.physical ? L"" : L", virtual") + L")\n");
                print(L"     current:  " + mac::format(a.currentMac) + L"\n");
                print(L"     hardware: " + (a.permanentMac ? mac::format(*a.permanentMac) : L"not reported") + L"\n");
                print(L"     override: " + (a.overrideValue ? *a.overrideValue : L"none") + L"\n");
            }
            return 0;
        }

        if (cmd == L"set" && argc == 4) {
            const auto& a = pick(adapters, argv[2]);
            auto m = mac::parse(argv[3]);
            if (!m) throw mac::Error{L"'" + std::wstring(argv[3]) + L"' is not a MAC address (expected 12 hex digits).", 0};
            requireAdmin();
            if (!mac::isLocallyAdministered(*m))
                print(L"warning: locally administered bit not set (second hex digit should be 2, 6, A or E); many drivers ignore such addresses.\n");
            print(L"Applying " + mac::format(*m) + L" to " + a.connectionName + L"...\n");
            mac::setOverride(a, *m);
            print(L"Done.\n");
            return 0;
        }

        if (cmd == L"restore" && argc == 3) {
            const auto& a = pick(adapters, argv[2]);
            requireAdmin();
            print(L"Restoring the hardware address on " + a.connectionName + L"...\n");
            mac::clearOverride(a);
            print(L"Done.\n");
            return 0;
        }

        return usage();
    } catch (const mac::Error& e) {
        print(L"error: " + e.message + L"\n");
        return 1;
    }
}
