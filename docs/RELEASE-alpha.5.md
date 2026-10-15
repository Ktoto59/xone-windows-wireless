# Alpha.5 - Dynamic C++ headset bridge

A Windows 11 x64 prerelease for the external Xbox Wireless Adapter `045E:02FE`,
Xbox Wireless Headsets 2021/2024 and supported Series controllers.

**Do not update headset/controller firmware through the bridge, including OTA
updates offered by Xbox Accessories.** Stop the bridge and use a physical USB
data cable with the manufacturer's supported update procedure for the real device.
The virtual wired identity does not provide a firmware-update transport.

## Download and install

- **Setup-x64.exe:** recommended; prepares the adapter/dependencies and registers
  a limited C++ host at Windows sign-in. Administrator approval may be needed.
- **windows-x64.zip:** minimal runtime payload; extracting it does not install it.
- **source.zip:** corresponding source, tests and build instructions.
- **SHA256SUMS.txt:** checksums for the three downloads.

Connect the adapter, stop an older development bridge, run Setup as your normal
Windows user and approve setup UAC. Restart/reconnect if requested. For first
pairing, activate the device's pairing mode and briefly press the adapter button.
Select the desired headset output/input in Windows or your application.

[Installation, upgrade and removal](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5/docs/INSTALL.md)
| [Full limitations](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5/docs/LIMITATIONS.md)
| [Privacy](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.5/docs/PRIVACY.md)

## Changes

- Automatic model selection before USB export, with per-association queues,
  tokens, clocks and stable locally generated virtual serials. No device-address
  allowlist or developer-specific mappings. No resident Python or PowerShell.
- Separate 2021/2024 headset connectors and independent audio pacing. Preserve
  the 2021 headset's wireless mode with a narrowly matched USB initialization filter.
- Bound capture/render age, expire stale work and alternate shared radio submissions.
- Fix device removal triggered by temporarily unreadable status files. Preserve
  the user's audio defaults when another headset connects.
- English documentation and automatic empty configuration template; explicit
  firmware-update restrictions and privacy/source checks in GitHub CI.

## Validation and limits

99 Python/reference/loopback tests passed, along with C++ protocol/session,
codec/core, snapshot/router and configuration/setup checks. The isolated installer
test verified file hashes, Windows Installed apps registration and removal without
executing hardware hooks. Both physical headset revisions and a Series controller
connected on one development PC; short joint playback was reported clean. A live
six-second status-file lock preserved the imported device.

Fresh-PC hardware setup/restoration, long-duration voice/music, repeated lifecycle
stress and broad sleep/resume remain incompletely tested. Eight software slots
are not a validated eight-stream capacity. Controller audio-jack streaming,
firmware updates and stock wireless authentication/security parity are unsupported
or unverified. Xbox Accessories can display Connected instead of a battery level.

USBip remains in the virtual transport. Project EXEs are unsigned; system WinUSB
and upstream USBip kernel drivers are signed. TESTSIGNING, Developer Mode and
disabling Secure Boot are not requirements. The adapter belongs to WinUSB while
the bridge is installed and cannot simultaneously run its stock wireless driver.

Remove through Windows Installed apps with the adapter connected. Removal restores
only an owned stock binding, retains shared USBip/private recovery files and stops
if restoration cannot be verified. Firmware, Microsoft drivers, real device IDs,
private configs, captures and certificates are excluded from release assets.
