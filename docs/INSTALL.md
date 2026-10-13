# Install and use alpha.4

Use the **Setup x64 EXE** from the
[alpha.4 release](https://github.com/Ktoto59/xone-windows-wireless/releases/tag/v0.1.0-alpha.4).
It installs an invisible C++ background host and enables automatic startup.
The ZIP is a source/development package; extracting it does not install the application.

## Requirements

- Windows 11 x64, a logged-in user session and administrator approval for setup.
- One external Xbox Wireless Adapter with hardware ID `045E:02FE`, connected during
  setup. Other adapter variants and built-in Xbox radios are not supported here.
- A supported Series controller, Xbox Wireless Headset 2021 (green rim), or Xbox
  Wireless Headset 2024 (no green rim). One device per model is supported.
- Internet access when the Microsoft adapter RAM firmware or USBip dependency is
  missing. Setup downloads and verifies them; Python does not need to be installed.

The current installer accepts the stock Microsoft `mt7612US` adapter driver or an
existing WinUSB binding. It uses signed Windows drivers and signed upstream
usbip-win2 0.9.8.1; it does not require Developer Mode, TESTSIGNING or a Secure Boot
change. The project's own application/Setup executables are not Authenticode-signed.

## Installation

1. Connect the adapter. Close an older xone control panel if it is open.
2. Run `xone-windows-wireless-0.1.0-alpha.4-Setup-x64.exe` and click **Install**.
3. Approve the Windows administrator prompt for dependency and adapter setup.
   USB devices can briefly disconnect during the first USBip installation because
   it restarts USB hubs. Restart Windows if Setup requests it.
4. Turn on a supported device. If it has not been paired with this adapter, start
   pairing on the device and briefly press the adapter's pairing button.
5. Use the controller or play ordinary audio. The host identifies supported models,
   saves their addresses locally and selects the latest connected headset for
   default playback and microphone input.

Setup obtains the matching firmware, preserves the stock binding when it changes
it, and selects WinUSB for the physical adapter. Firmware is loaded into adapter
RAM; it is not a persistent firmware update. The background host starts immediately
and at the installing user's subsequent Windows sign-ins, without another normal
UAC prompt. It is a user-session task, so it does not run before sign-in.

There is no control window, tray icon, overlay, manual device mapping or normal
backend-selection step. No Python or PowerShell process stays running. Individual
applications with a pinned audio output may need their output changed once; they
can ignore the Windows default. Headset volume, microphone mute and chat/game mix
still follow the physical controls.

If Setup finishes but a device does not appear, follow
[troubleshooting](TROUBLESHOOTING.md). Reconnecting the adapter once after a binding
change may be necessary; regular use should not require changing drivers.

## Upgrade

Run the newer Setup EXE as the same Windows user. Same-folder installation preserves
local configuration/recovery files and stops the owned background task before files
are replaced. Setup enables the new automatic mode afterwards.

For a previous installation in another folder, alpha.4 can stop the recognized
background task and copy validated device configuration and stock-binding metadata
when the new installation has none. An unrecognized task or another running radio
session can block setup; stop it from its own installation first. Recovery files in
that previous folder are retained. Do not delete them until the upgrade works.

The older alpha.3 control-panel/Python workflow is historical and is not required
by this release. A fresh PC learns its own device addresses; do not copy another
PC's local configuration or adapter backup into the installation.

## Remove

Keep the adapter connected to the original USB port and uninstall
**xone-windows-wireless** from Windows **Installed apps**. Removal stops the owned
host, removes its sign-in task and restores the stock adapter binding only when
this installation owns the recovery metadata. Reconnect the adapter afterwards.

Shared USBip and private configuration/recovery files are retained. If removal
cannot verify that the owned processes stopped or the stock binding was restored,
it stops and keeps recovery data. Correct the reported problem and retry removal.

## Local files and privacy

The default application directory is
`%LOCALAPPDATA%\Programs\xone-windows-wireless`.
Private configuration is in `config/*.local.json`; small lifecycle/error snapshots
and driver recovery data are in `work/`. Normal operation does not create text
logs, packet captures or microphone recordings.

These files can contain physical addresses, instance IDs and local paths. They
are not part of the public download. Do not attach the complete folders to an
issue; report the model, Windows build, application version and symptoms instead.

This is an alpha tested on one development PC. Fresh installation on another PC,
long-duration playback and broad sleep/resume behavior remain unverified.
