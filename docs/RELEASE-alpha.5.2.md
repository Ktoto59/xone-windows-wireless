# 0.1.0-alpha.5.2

Fixes Setup stopping at **WinusbBinding** with `pnputil.exe is not recognized`.
The failure is reproducible when maintenance runs in 32-bit PowerShell on x64
Windows: WOW64 redirects System32 to SysWOW64, which has no PnPUtil executable.

Maintenance now switches to native x64 PowerShell before importing modules or
preparing dependencies. PnPUtil and CAB extraction use explicit native system
paths, including the stock-driver backup performed before selecting WinUSB.
No PATH changes or manual component installation are needed.

The automatic preparation from alpha.5.1 remains: missing Microsoft adapter
drivers, verified RAM firmware and the signed USBip dependency are downloaded
and installed by Setup. C++ radio/audio binaries are unchanged.

Validation: real native tools invoked from 32-bit and 64-bit PowerShell with
empty PATH; maintenance bootstrap with spaced arguments and failure propagation;
configuration/prerequisite tests; installer payload and registration/removal
smoke test. Hardware bindings are not changed by these tests. Installation on
the reported second PC still needs confirmation.

Run this Setup EXE as the same Windows user with the adapter connected. A failed
alpha.5/alpha.5.1 installation can be retried directly. Approve UAC and restart
Windows if requested. No prior headset installation is required.

**Do not update headset/controller firmware through the bridge, including OTA.**
Stop the bridge and use a physical USB data cable for the actual device.

[Installation](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5.2/docs/INSTALL.md)
| [Limitations](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5.2/docs/LIMITATIONS.md)
