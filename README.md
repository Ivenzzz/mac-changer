# mac-changer

A small Windows desktop tool, written in C++ with the plain Win32 API, that overrides the MAC
address of an Ethernet or Wi-Fi adapter and restores the hardware address again.

## How it works

Windows has no "set MAC address" API. What every MAC changer (and Device Manager itself) does is:

1. Find the adapter's **driver key** in the registry:
   `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}\NNNN`
2. Write the REG_SZ value **`NetworkAddress`** = `AABBCCDDEEFF` (12 hex digits, no separators).
   Deleting the value restores the burned-in address.
3. **Restart the adapter**, because NDIS only reads the value when the miniport starts.

This project does it with the supported APIs instead of hand-scanning the registry:

| Step | API | File |
|------|-----|------|
| Enumerate adapters | `SetupDiGetClassDevs(GUID_DEVCLASS_NET)` + `GetAdaptersAddresses` | `src/macaddr.cpp` |
| Locate the driver key | `SetupDiOpenDevRegKey(..., DIREG_DRV, ...)` | `src/macaddr.cpp` |
| Read the burned-in MAC | `DeviceIoControl(IOCTL_NDIS_QUERY_GLOBAL_STATS, OID_802_3_PERMANENT_ADDRESS)` | `src/macaddr.cpp` |
| Write / delete the override | `RegSetValueEx` / `RegDeleteValue` on `NetworkAddress` | `src/macaddr.cpp` |
| Restart the adapter | `SetupDiSetClassInstallParams` + `SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)` | `src/macaddr.cpp` |
| Elevation | `requireAdministrator` in `app.manifest`, embedded via `app.rc` | `app.manifest` |

The core (`macaddr.h/.cpp`) has no UI dependency, so the GUI (`main_gui.cpp`) and the CLI
(`main_cli.cpp`) are thin layers over it.

## Build

Requires MinGW-w64 g++ (C++20) and `windres`, both in the scoop `gcc` package, plus GNU make
(`scoop install make`).

```
make
```

Produces `macchanger.exe` (GUI, asks for elevation via UAC) and `macchanger-cli.exe`.

Without make, the same build by hand:

```
g++ -std=c++20 -Wall -Wextra -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -c src/macaddr.cpp  -o macaddr.o
g++ -std=c++20 -Wall -Wextra -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -c src/main_gui.cpp -o main_gui.o
g++ -std=c++20 -Wall -Wextra -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -c src/main_cli.cpp -o main_cli.o
windres app.rc -o res.o
g++ -static -municode -mwindows main_gui.o macaddr.o res.o -o macchanger.exe     -lsetupapi -liphlpapi -lcomctl32 -luser32 -lgdi32 -ladvapi32
g++ -static -municode           main_cli.o macaddr.o       -o macchanger-cli.exe -lsetupapi -liphlpapi -lcomctl32 -luser32 -lgdi32 -ladvapi32
```

## Use

```
macchanger-cli list                      # works without admin
macchanger-cli set 1 02:11:22:33:44:55   # admin console
macchanger-cli restore 1                 # admin console
```

Or run `macchanger.exe`, pick an adapter, type an address or press **Random**, then **Apply**.
**Restore original** deletes the override. The adapter is disabled and re-enabled on every change,
so the connection drops for a few seconds.

## Rules and gotchas

- **Administrator is required.** Both the registry write and the adapter restart fail otherwise.
- **Use a locally administered address.** Bit 1 of the first octet must be set and bit 0 clear,
  which means the second hex digit must be `2`, `6`, `A` or `E` (e.g. `02:..`, `X6:..`, `XA:..`, `XE:..`).
  Since Windows 10, most Wi-Fi drivers and several Intel Ethernet drivers silently ignore any other
  value. The code validates this and the GUI warns before applying a non-conforming address.
- **Multicast addresses (odd first octet) are rejected** outright.
- **Some drivers ignore `NetworkAddress` entirely.** `setOverride` checks the live address after the
  restart and reports when the driver did not take it.
- **The override persists across reboots** until it is deleted. If the tool ever leaves an adapter in
  a bad state, Device Manager > adapter > Properties > Advanced > Network Address > "Not Present"
  does the same thing as Restore.
- The tool lists only adapters that are enabled and of type Ethernet or 802.11. Disabled adapters,
  VPN tunnels and WAN miniports are hidden on purpose. Virtual adapters such as Hyper-V switches,
  Wi-Fi Direct and Bluetooth PAN still show up because they are Ethernet-type; they are sorted last
  and marked "virtual". Hyper-V sets `NetworkAddress` on its own vEthernet adapters, which is a
  good sign that this is the mechanism Windows itself relies on.
- Only 64-bit builds are supported. A 32-bit process on 64-bit Windows cannot call
  `SetupDiCallClassInstaller` for device state changes (it fails with `ERROR_IN_WOW64`).

## Ideas for next steps

- A ListView with columns instead of a ListBox.
- Remember previously used addresses, or offer vendor prefixes from an OUI table.
- Per-monitor DPI awareness (`WM_DPICHANGED`) instead of system DPI.
- Package the GUI with an installer or a signed binary so SmartScreen stops complaining.
