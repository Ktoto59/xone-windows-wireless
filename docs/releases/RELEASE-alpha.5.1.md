# 0.1.0-alpha.5.1

Fixes a reproducible first-install failure when no local adapter firmware exists.
Setup now automatically installs a missing signed Microsoft adapter driver and
installs/repairs the tested signed USBip dependency. It downloads the official
Microsoft CAB through the catalog HTTPS host,
with normal certificate validation and a pinned SHA-256. The extracted firmware
is verified separately. No Microsoft firmware or driver binaries are bundled.

Setup now shows the failing stage and actual error, and retains a local JSON
result, error text and transcript. A failed alpha.5 installation can be retried
with this installer as the same Windows user, keeping the adapter connected.

The installer, binding helper and background host now consistently recognize the
four adapter IDs from upstream xone: 045E:02FE, 02E6, 02F9 and 091E. Only 02FE
has physical validation here; the other three require hardware testing. Setup
automatically installs a missing Microsoft stock driver before binding
WinUSB; no prior Xbox hardware installation is required. Unknown adapters,
Bluetooth dongles and Xbox 360 receivers are not covered.

Validation includes a full simulated fresh-system preparation sequence (no
firmware, stock adapter driver or USBip), repair/reboot and untrusted-installer
checks, isolated maintenance failure/retry checks, real Microsoft CAB
extraction/import into an empty fixture, adapter selection/malformed-ID tests,
native builds and the installer registration/removal smoke test. That smoke test
does not change hardware bindings. Installation on the reported second PC still
needs confirmation.

**Do not update headset/controller firmware through the bridge, including OTA.**
Stop the bridge and use a physical USB data cable for the actual device.

[Installation](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5.1/docs/INSTALL.md)
| [Limitations](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5.1/docs/LIMITATIONS.md)

This remains an experimental Windows 11 x64 prerelease. USBip remains part of the
virtual USB transport. Stock-equivalent radio security, firmware updating,
long-term reliability and broad sleep/resume support are not established.
