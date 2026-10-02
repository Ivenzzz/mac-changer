#include "macaddr.h"

#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <initguid.h>  // make DEFINE_GUID in devguid.h emit storage in this translation unit
#include <devguid.h>   // GUID_DEVCLASS_NET
#include <cfgmgr32.h>  // MAX_DEVICE_ID_LEN

#include <algorithm>
#include <cwctype>
#include <map>
#include <random>

// NDIS query that asks the driver for its burned-in address. The official header (ntddndis.h)
// drags in kernel-mode types, so the two constants are repeated here.
#ifndef IOCTL_NDIS_QUERY_GLOBAL_STATS
#define IOCTL_NDIS_QUERY_GLOBAL_STATS 0x00170002
#endif
#ifndef OID_802_3_PERMANENT_ADDRESS
#define OID_802_3_PERMANENT_ADDRESS 0x01010101
#endif

namespace mac {
namespace {

std::wstring win32Message(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = n ? std::wstring(buf, n) : L"unknown error";
    if (buf) LocalFree(buf);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

[[noreturn]] void fail(const wchar_t* what, DWORD code) {
    throw Error{std::wstring(what) + L": " + win32Message(code) + L" (error " + std::to_wstring(code) + L")", code};
}
[[noreturn]] void failLast(const wchar_t* what) { fail(what, GetLastError()); }

std::wstring upper(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

// RAII wrappers --------------------------------------------------------------------------------

struct DevInfoSet {
    HDEVINFO h = INVALID_HANDLE_VALUE;
    ~DevInfoSet() { if (h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h); }
};

struct RegKey {
    HKEY h = nullptr;
    ~RegKey() { if (h) RegCloseKey(h); }
};

// SetupAPI helpers -----------------------------------------------------------------------------

// Opens the driver key of an adapter (the "NNNN" key under the Net class). Returns nullptr on failure.
HKEY openDriverKey(HDEVINFO set, SP_DEVINFO_DATA& dev, REGSAM access) {
    HKEY k = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, access);
    return k == INVALID_HANDLE_VALUE ? nullptr : k;
}

std::optional<std::wstring> readString(HKEY key, const wchar_t* name) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ) return std::nullopt;
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) return std::nullopt;
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::wstring deviceProperty(HDEVINFO set, SP_DEVINFO_DATA& dev, DWORD property) {
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, property, nullptr, nullptr, 0, &bytes);
    if (bytes == 0) return L"";
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, property, nullptr, reinterpret_cast<PBYTE>(value.data()),
                                           static_cast<DWORD>(value.size() * sizeof(wchar_t)), nullptr)) return L"";
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

// Real NICs hang off a hardware bus. Virtual adapters (Hyper-V, Wi-Fi Direct, VPN drivers,
// Bluetooth PAN) are enumerated under ROOT\, SWD\, BTH\ or a GUID-named enumerator instead.
bool isPhysicalBus(const std::wstring& instanceId) {
    const std::wstring id = upper(instanceId);
    for (const wchar_t* bus : {L"PCI\\", L"USB\\", L"PCMCIA\\", L"SD\\"})
        if (id.rfind(bus, 0) == 0) return true;
    return false;
}

std::wstring instanceId(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    wchar_t buf[MAX_DEVICE_ID_LEN + 1] = {};
    if (!SetupDiGetDeviceInstanceIdW(set, &dev, buf, MAX_DEVICE_ID_LEN, nullptr)) failLast(L"SetupDiGetDeviceInstanceId");
    return buf;
}

// Re-opens one specific device by its instance id so we can write to it and restart it.
struct Device {
    DevInfoSet set;
    SP_DEVINFO_DATA data{};

    explicit Device(const std::wstring& id) {
        set.h = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, nullptr);
        if (set.h == INVALID_HANDLE_VALUE) failLast(L"SetupDiCreateDeviceInfoList");
        data.cbSize = sizeof data;
        if (!SetupDiOpenDeviceInfoW(set.h, id.c_str(), nullptr, 0, &data)) failLast(L"SetupDiOpenDeviceInfo");
    }

    HKEY openDriverKeyForWrite(RegKey& key) {
        key.h = openDriverKey(set.h, data, KEY_SET_VALUE);
        if (!key.h) failLast(L"Opening the driver registry key for writing (run as administrator?)");
        return key.h;
    }

    // Asks the class installer to change device state: what Device Manager enable/disable does.
    void changeState(DWORD state, DWORD scope) {
        SP_PROPCHANGE_PARAMS p{};
        p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        p.StateChange = state;
        p.Scope = scope;
        p.HwProfile = 0;
        if (!SetupDiSetClassInstallParamsW(set.h, &data, &p.ClassInstallHeader, sizeof p)) failLast(L"SetupDiSetClassInstallParams");
        if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set.h, &data)) failLast(L"SetupDiCallClassInstaller");
    }

    // NDIS only reads NetworkAddress when the miniport starts, so bounce the adapter.
    // Same sequence Microsoft devcon uses: disable in the current profile, enable globally
    // (failure tolerated) and then in the current profile.
    void restart() {
        changeState(DICS_DISABLE, DICS_FLAG_CONFIGSPECIFIC);
        try { changeState(DICS_ENABLE, DICS_FLAG_GLOBAL); } catch (const Error&) {}
        changeState(DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
    }
};

// IP Helper: what the stack currently knows, keyed by upper-cased "{GUID}" ---------------------

struct Live {
    std::wstring connectionName;
    Mac mac{};
    bool wireless = false;
};

std::map<std::wstring, Live> queryLive() {
    const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG bytes = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(bytes);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &bytes);
    }
    std::map<std::wstring, Live> out;
    if (rc == ERROR_NO_DATA) return out;
    if (rc != ERROR_SUCCESS) fail(L"GetAdaptersAddresses", rc);

    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        // Only LAN-style adapters. Loopback, tunnels and WAN miniports have other types or no MAC.
        if (a->PhysicalAddressLength != 6) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        Live live;
        live.connectionName = a->FriendlyName;
        live.wireless = a->IfType == IF_TYPE_IEEE80211;
        std::copy_n(a->PhysicalAddress, 6, live.mac.begin());
        std::string guid = a->AdapterName;  // "{GUID}", ASCII
        out[upper(std::wstring(guid.begin(), guid.end()))] = live;
    }
    return out;
}

// After a restart the stack needs a moment to bring the interface back. Returns its MAC once visible.
std::optional<Mac> waitForAdapter(const std::wstring& guid) {
    const std::wstring key = upper(guid);
    for (int i = 0; i < 40; ++i) {
        auto live = queryLive();
        auto it = live.find(key);
        if (it != live.end()) return it->second.mac;
        Sleep(250);
    }
    return std::nullopt;
}

std::optional<Mac> queryPermanentMac(const std::wstring& guid) {
    std::wstring path = L"\\\\.\\" + guid;
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    DWORD oid = OID_802_3_PERMANENT_ADDRESS, returned = 0;
    Mac mac{};
    BOOL ok = DeviceIoControl(h, IOCTL_NDIS_QUERY_GLOBAL_STATS, &oid, sizeof oid, mac.data(),
                              static_cast<DWORD>(mac.size()), &returned, nullptr);
    CloseHandle(h);
    if (!ok || returned != mac.size()) return std::nullopt;
    return mac;
}

}  // namespace

// Public API -----------------------------------------------------------------------------------

std::wstring format(const Mac& m, wchar_t separator) {
    static const wchar_t* digits = L"0123456789ABCDEF";
    std::wstring s;
    for (size_t i = 0; i < m.size(); ++i) {
        if (i && separator) s += separator;
        s += digits[m[i] >> 4];
        s += digits[m[i] & 0xF];
    }
    return s;
}

std::optional<Mac> parse(std::wstring_view text) {
    std::wstring hex;
    for (wchar_t c : text) {
        if (c == L':' || c == L'-' || c == L'.' || iswspace(c)) continue;
        if (!iswxdigit(c)) return std::nullopt;
        hex += c;
    }
    if (hex.size() != 12) return std::nullopt;
    Mac m{};
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = static_cast<unsigned char>(wcstoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    return m;
}

bool isUnicast(const Mac& m) { return (m[0] & 0x01) == 0; }
bool isLocallyAdministered(const Mac& m) { return (m[0] & 0x02) != 0; }

Mac randomLocallyAdministered() {
    std::random_device rd;
    std::uniform_int_distribution<int> byte(0, 255);
    Mac m{};
    for (auto& b : m) b = static_cast<unsigned char>(byte(rd));
    m[0] = static_cast<unsigned char>((m[0] & 0xFC) | 0x02);  // clear multicast bit, set locally-administered bit
    return m;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

std::vector<Adapter> enumerateAdapters() {
    auto live = queryLive();

    DevInfoSet set;
    set.h = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
    if (set.h == INVALID_HANDLE_VALUE) failLast(L"SetupDiGetClassDevs");

    std::vector<Adapter> out;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &dev); ++i) {
        RegKey drv;
        drv.h = openDriverKey(set.h, dev, KEY_READ);
        if (!drv.h) continue;
        auto guid = readString(drv.h, L"NetCfgInstanceId");
        if (!guid) continue;  // not a network interface the stack knows about
        auto it = live.find(upper(*guid));
        if (it == live.end()) continue;  // disabled, or not an Ethernet/Wi-Fi adapter

        Adapter a;
        a.instanceId = instanceId(set.h, dev);
        a.netCfgInstanceId = *guid;
        a.description = deviceProperty(set.h, dev, SPDRP_DEVICEDESC);
        a.connectionName = it->second.connectionName;
        a.currentMac = it->second.mac;
        a.wireless = it->second.wireless;
        a.physical = isPhysicalBus(a.instanceId);
        a.permanentMac = queryPermanentMac(*guid);
        a.overrideValue = readString(drv.h, L"NetworkAddress");
        if (a.overrideValue && a.overrideValue->empty()) a.overrideValue.reset();
        out.push_back(std::move(a));
    }
    // Physical adapters first, then by connection name.
    std::sort(out.begin(), out.end(), [](const Adapter& x, const Adapter& y) {
        if (x.physical != y.physical) return x.physical;
        return x.connectionName < y.connectionName;
    });
    return out;
}

void setOverride(const Adapter& a, const Mac& m) {
    if (!isUnicast(m)) throw Error{L"A multicast address (odd first octet) cannot be assigned to an adapter.", 0};

    Device d(a.instanceId);
    RegKey key;
    d.openDriverKeyForWrite(key);
    const std::wstring value = format(m, 0);
    LONG rc = RegSetValueExW(key.h, L"NetworkAddress", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (rc != ERROR_SUCCESS) fail(L"Writing NetworkAddress", rc);
    d.restart();

    auto now = waitForAdapter(a.netCfgInstanceId);
    if (!now) throw Error{L"The adapter did not come back after restarting. Check Device Manager.", 0};
    if (*now != m)
        throw Error{L"The driver did not accept " + format(m) + L" and still reports " + format(*now) +
                        L". Wi-Fi drivers usually only allow addresses whose second hex digit is 2, 6, A or E. "
                        L"The override value was left in the registry; use Restore to remove it.", 0};
}

void clearOverride(const Adapter& a) {
    Device d(a.instanceId);
    RegKey key;
    d.openDriverKeyForWrite(key);
    LONG rc = RegDeleteValueW(key.h, L"NetworkAddress");
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) fail(L"Deleting NetworkAddress", rc);
    d.restart();
    waitForAdapter(a.netCfgInstanceId);
}

}  // namespace mac
