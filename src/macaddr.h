// macaddr.h - core logic for reading and overriding network adapter MAC addresses on Windows.
//
// How a MAC override works on Windows
// -----------------------------------
// Every network adapter has a per-driver registry key ("driver key") under
//   HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}\NNNN
// When the adapter starts, NDIS reads the REG_SZ value "NetworkAddress" (12 hex digits, no
// separators) from that key and uses it instead of the burned-in address. Deleting the value
// restores the hardware address. The adapter must be restarted for either change to take
// effect. This is exactly what Device Manager > Advanced > "Network Address" does.
//
// We never scan the Class key by hand: SetupAPI gives us the right driver key for a device
// (SetupDiOpenDevRegKey with DIREG_DRV) and restarts the device (DIF_PROPERTYCHANGE).
// Both need administrator rights, so the GUI manifest requests elevation.
#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mac {

using Mac = std::array<unsigned char, 6>;

struct Adapter {
    std::wstring instanceId;          // PnP device instance id, used to reopen the device later
    std::wstring netCfgInstanceId;    // "{GUID}" the network stack uses for this adapter
    std::wstring description;         // device name, e.g. "Intel(R) Ethernet Connection I219-V"
    std::wstring connectionName;      // name shown in Network Connections, e.g. "Ethernet"
    Mac currentMac{};                 // address the stack is using right now
    std::optional<Mac> permanentMac;  // burned-in address, when the driver reports it
    std::optional<std::wstring> overrideValue;  // the NetworkAddress registry value, if set
    bool wireless = false;
    bool physical = false;  // sits on a hardware bus (PCI/USB). Hyper-V, Wi-Fi Direct, Bluetooth PAN are virtual.
};

// Thrown by every function below on failure. `code` is the Win32 error, or 0 when not applicable.
struct Error {
    std::wstring message;
    unsigned long code = 0;
};

std::wstring format(const Mac& m, wchar_t separator = L':');  // separator 0 gives "AABBCCDDEEFF"
std::optional<Mac> parse(std::wstring_view text);  // AA:BB:CC:DD:EE:FF, AA-BB-.., AABB.CCDD.EEFF, AABBCCDDEEFF
bool isUnicast(const Mac& m);              // bit 0 of octet 0 clear. Multicast addresses cannot be assigned.
bool isLocallyAdministered(const Mac& m);  // bit 1 of octet 0 set. Many drivers (esp. Wi-Fi) require this.
Mac randomLocallyAdministered();

bool isElevated();

std::vector<Adapter> enumerateAdapters();          // enabled Ethernet and Wi-Fi adapters
void setOverride(const Adapter& a, const Mac& m);  // writes NetworkAddress, restarts the adapter, verifies
void clearOverride(const Adapter& a);              // deletes NetworkAddress and restarts the adapter

}  // namespace mac
