# Installer and first-run control panel

`xone-windows-wireless-0.1.0-alpha.3-Setup-x64.exe` is the recommended download.
It targets Windows 11 x64 and installs per user under
`%LOCALAPPDATA%\Programs\xone-windows-wireless`. Start menu and optional desktop
shortcuts open a WinForms first-run panel. Install/source files are included;
private configuration and recovery files are created locally only.

## What happens when

- **Setup** copies application files, source/licenses and a private Python runtime.
  It registers an uninstall entry. No adapter or driver change happens at this stage.
- **Prepare dependencies** first looks for the exact adapter RAM firmware in the
  local Windows driver store. If absent, it downloads the Microsoft CAB documented
  by upstream xone and checks the extracted firmware SHA-256. Firmware is not in Setup.
  If USBip is absent, it downloads USBip 0.9.8.1 x64 from the upstream release,
  checks the pinned SHA-256 and valid Scheibling Consulting AB Authenticode signature,
  and invokes its installer with UAC. Only `main,client` components are selected.
  Existing USBip is not downgraded or replaced; unsupported versions require manual
  resolution. USB hubs may restart briefly. A requested PC restart is reported and
  never performed automatically.
- **Use WinUSB** backs up and changes exactly the selected adapter through the
  existing checked binding helper. It requests UAC. A physical USB reconnect follows.
- **Search for device** stops this app's bridge and observes radio association
  addresses for up to 90 seconds, without creating virtual Windows outputs. The
  user chooses the model and address. It supports one controller and one of each
  headset profile; it does not infer a model from an address. Other devices must be off.
- **Start bridge** uses the standard USB/GIP route without elevation and with
  unlimited sessions. A control-panel close does not stop it.
  **Enable automatic mode** also starts the background host at Windows sign-in;
  Setup offers this option by default. It waits for first-run setup to finish.
  **Stop bridge** stops the current session; **Disable automatic mode** also removes
  sign-in startup. This is a user-session task, not a service running before login.
- **Restore stock driver** stops this app's bridge and uses only its saved state.
  **Uninstall** closes no unrelated USBip sessions, stops this app's bridge, and
  restores the binding it owns. It refuses deletion if restoration cannot be
  verified. Connect the adapter to its original USB port and approve UAC. Private
  `work/`, local config and the shared USBip installation are retained.

An existing development/portable WinUSB installation can remain selected, but its
original recovery folder is still needed. Setup does not import another folder's
MAC mapping, firmware or driver backups, or claim ownership of its binding.

## Runtime provenance

Python **3.13.16** is the official x64 embeddable package:

- Source: https://www.python.org/ftp/python/3.13.16/python-3.13.16-embed-amd64.zip
- SHA-256: `97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297`
- Release/license: https://www.python.org/downloads/release/python-31316/
- Original `LICENSE.txt` ships alongside the runtime.

The runtime's `python313._pth` adds only the shipped source/application directories.
It does not load user packages, install pip, modify PATH or register a system Python.
The Python DLLs and their upstream runtime dependencies are the only runtime DLLs
redistributed here. Microsoft Xbox system drivers and firmware are not bundled.

USBip 0.9.8.1 is downloaded at first setup, not redistributed inside Setup:

- Release: https://github.com/vadimgrn/usbip-win2/releases/tag/v.0.9.8.1
- SHA-256: `38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a`

The Setup EXE and project tools are **not Authenticode-signed**. They do not install
an unsigned kernel driver or change test-signing/Secure Boot policy. Use the published
SHA256SUMS file to verify the release. The program remains a research preview with
the same hardware/reconnection/battery limitations as the portable bridge.

## Build and verification

Use the existing C++ builds, Inno Setup 6.7.3 and Windows' .NET Framework C# compiler.
`Build-Installer.ps1 -IsccPath <ISCC.exe>` creates Setup and a matching portable ZIP
from Git-tracked source and explicitly allowed project EXEs. Work logs, firmware,
local configs and third-party installer caches are excluded. The private Python
download is checksum-verified at build time; runtime licenses are preserved.

The software checks cover mapping validation, discovery parsing, download checksum
failure, process identity and uninstall guards. Package extraction and file-only
install/uninstall can be tested without invoking Prepare/Bind/Discover/Start. A
successful packaging test does not certify driver installation on every Windows
build; hardware setup on a fresh machine is still preview validation.
