# xone-windows-wireless

Xbox Wireless Adapter support for Windows 11 x64, based on
[medusalix/xone](https://github.com/medusalix/xone). Controllers and Xbox Wireless
Headsets connect over the adapter's radio and use the existing Windows Xbox GIP
and audio drivers.

The alpha.4 development installer provides an invisible C++ background host:
install once, approve adapter setup, and turn on your devices. It starts at sign-in,
recognizes the supported models, reconnects them and selects the connected headset
for playback and microphone input. There is no control window, tray icon, overlay,
Python runtime or continuously running PowerShell process. Normal operation writes
no text logs or per-packet captures. Small local state files change only when the
connection, selected audio device or error state changes.

This is a user-mode radio/USB-GIP bridge, not a new kernel driver. WinUSB and the
separately installed signed USBip driver provide transport; Windows handles GIP,
XInput and audio. Direct XboxGipSynthetic remains an experiment: it currently has
audio instability and exposes a synthetic gamepad parent for a headset. It is not
used by this installer.

## Install

1. Connect the external Xbox Wireless Adapter to the PC.
2. Run `xone-windows-wireless-0.1.0-alpha.4-Setup-x64.exe` and approve Windows UAC
   for setup. Internet access is needed for dependencies if they are not installed.
3. Turn on your controller or headset. For a device that has never been paired
   with this adapter, use the ordinary pairing buttons on the device and adapter.

Setup imports the exact Microsoft adapter RAM firmware, installs the verified
USBip dependency if needed, backs up the stock adapter binding and selects WinUSB.
USB hubs may briefly restart while USBip is first installed. A Windows restart can
be required by its driver installation. The application starts automatically in
your logged-in user session and runs without administrator privileges.

The portable archive is intended for source/development use; use Setup for automatic
installation. The published [alpha.3 release](https://github.com/Ktoto59/xone-windows-wireless/releases/tag/v0.1.0-alpha.3)
uses the earlier panel/Python supervisor and remains the previous checkpoint.

## Supported models

| Device | Windows USB/GIP profile |
| --- | --- |
| External adapter `045E:02FE` | WinUSB transport |
| Xbox Series controller | `045E:0B12` |
| Xbox Wireless Headset 2021, green rim | `045E:0B17` |
| Xbox Wireless Headset 2024, no green rim | `045E:0B32` |

The current catalog supports one device per model. Addresses are discovered and
saved privately on the PC; no manual MAC/profile selection is needed. The most
recently connected headset becomes the default playback/capture device. When it
disconnects, the host restores the previous active default where applicable and
respects a later manual default-device change.

Both headset revisions and a Series controller previously worked together with
clean audio on the USB/GIP route. The new background implementation is a prerelease,
not a promise of years of unattended operation on every PC. Testing on other Windows
builds, firmware versions and computers remains necessary.

## Limits

- Xbox Accessories may show **Connected**, since the transport is presented as USB.
- Firmware updates and controller audio-jack streaming are not implemented. Use a
  real USB cable to update firmware.
- Wireless authentication v3/CCMP is not implemented for the raw GIP route; this
  does not reproduce the stock driver's wireless security behavior.
- Application-pinned audio outputs can ignore changes to the Windows default.
- Microsoft firmware/drivers, device MACs, stock-driver backups and diagnostic
  captures are not included in the repository or installer.

## Uninstall

Use Windows **Installed apps**. Uninstall stops this installation's background host
and restores the stock binding if this installation owns its backup. Keep the
adapter connected. Shared USBip and private recovery data are retained.

## Build and source

Use Visual Studio 2022 C++ Build Tools and `scripts/Build-Appliance.cmd`.
`Build-Installer.ps1 -IsccPath <ISCC.exe>` packages the C++ executables with Inno Setup.
PowerShell is used during installation/removal, not during ordinary operation.
Historical Python research tools remain in source control but are excluded from
this application package. See [architecture](docs/ARCHITECTURE.md),
[installer details](docs/INSTALLER.md), and [upstream attribution](docs/UPSTREAM.md).

Project code: GPL-2.0-or-later. Third-party notices and licenses are retained.