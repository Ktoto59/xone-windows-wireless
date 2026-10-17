# Install, upgrade and remove

This guide describes the alpha.6 source and installer candidate.
Download installers from [published releases](https://github.com/Ktoto59/xone-windows-wireless/releases)
or build the [current candidate](INSTALLER.md). Alpha.5.2 is the latest published
installer until alpha.6 is published; its startup behavior differs as noted below.
Use the Setup-x64.exe; extracting the
runtime ZIP does not register the application or prepare the adapter.

**Do not update firmware through the bridge, including OTA headset updates.**
Stop the bridge and use a physical USB data cable for the actual device.
See [limitations and update instructions](LIMITATIONS.md).

## Requirements

- Windows 11 x64 and a logged-in user session.
- One Xbox Wireless Adapter, connected during setup: `045E:02FE` (tested),
  `045E:02E6`, `045E:02F9` or `045E:091E` (hardware validation pending).
  Setup installs the matching Microsoft stock package automatically if it is missing.
- Xbox Wireless Headset 2021 or 2024, or a supported Series controller.
- Administrator approval for adapter/dependency setup; internet access if verified
  firmware or the signed upstream USBip dependency is missing.

The application executables are unsigned. Kernel components are the system
WinUSB driver and signed upstream usbip-win2 0.9.8.1. TESTSIGNING and a change to
Secure Boot are not needed. The adapter is dedicated to this bridge while bound
to WinUSB; its stock wireless driver does not run alongside it.

## Installation and pairing

1. Connect the adapter and stop a development bridge running from another folder.
2. Run `xone-windows-wireless-<version>-Setup-x64.exe` as your normal Windows
   user. Approve the separate administrator prompt for setup.
3. Allow dependency preparation to finish. Setup downloads and installs missing
   Microsoft adapter drivers, verified RAM firmware and signed USBip automatically.
   You do not need to install Xbox Accessories, Python or Visual C++ separately. First-time USBip installation can
   briefly restart USB hubs. Restart Windows if requested.
4. Turn on your devices. For a device not already paired with this adapter,
   activate pairing on the device and briefly press the adapter's pairing button.
5. Choose the desired headset output/input in Windows or your application's audio
   settings. Connecting another headset does not change defaults automatically.

The bridge identifies supported models without a per-user MAC list. Eight
association slots are available; that is a software limit, not a promise that
eight simultaneous audio streams work. Two physical headset revisions and one
Series controller have been tested together on one PC. Both headsets are separate
audio devices; playing one application in both requires audio routing by the
application/Windows tools and is not automatic duplication by this bridge.

Alpha.6 offers separate start-now and sign-in startup choices and remembers them
on upgrade. Start-now is skipped if Windows requires a reboot; when sign-in
startup is off, use **Start xone Wireless** from the Start menu after restart.
**Stop xone Wireless** stops the owned bridge without changing adapter drivers.
The older alpha.5.2 installer enables immediate and sign-in startup automatically.

The invisible C++ host runs in the installing user's session.
It does not run before sign-in. No Python, persistent PowerShell, control panel,
tray icon or overlay is installed. Firmware is loaded into adapter RAM, not
flashed as a persistent adapter update.

## Upgrade and removal

Run the newer Setup as the same Windows user. Setup stops its owned host and keeps
private configuration/recovery metadata. A recognized earlier installation's
startup task can be migrated; an unrelated active radio session blocks setup.

Keep the adapter in its original USB port and remove **xone Wireless** (or the
older **xone-windows-wireless** entry)
through Windows **Installed apps**. The uninstaller stops its host, unregisters
its sign-in task and restores the stock binding only if this installation owns
the recovery metadata. Reconnect the adapter afterwards. If restoration cannot
be verified, removal stops and preserves recovery files. Shared USBip and private
configuration/recovery data are retained. An existing WinUSB binding not created
or adopted by this installation is left as it was.

## Files and validation

Default directory: `%LOCALAPPDATA%\Programs\xone-windows-wireless`.
Private settings are under `config/*.local.json`; setup results and driver recovery
data remain under `work/`. Current native runtime status/counters use shared memory
and events, with no periodic state-file writes. Query the running installation with
`scripts/Get-ControllerStatus.ps1 -Json`. Normal installer builds disable runtime
diagnostic journals; explicit development sessions can capture diagnostics. Older
releases retain file-based snapshots. Do not publish complete `work/` or `config/`
directories: they can contain addresses, instance IDs and local paths. Normal
operation records no audio.

This candidate is tested on one development PC. Compilation and runtime tests do
not establish fresh-PC installation, hardware restoration on every PC, long-term
reliability or broad sleep/resume support. See [cpp-bridge.md](cpp-bridge.md) and
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).
