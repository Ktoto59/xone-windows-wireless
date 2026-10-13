# Automatic C++ background application

Alpha.4 replaces the panel/Python supervisor with an invisible C++ host for Windows
11 x64. Supported controllers and headsets connect over an external Xbox Wireless
Adapter (`045E:02FE`) and use the existing Windows Xbox GIP/audio stack.

## Install

1. Connect one supported adapter and run
   `xone-windows-wireless-0.1.0-alpha.4-Setup-x64.exe`.
2. Click **Install**, approve Windows UAC for setup, and restart if requested.
   Internet access is needed for missing firmware/dependencies; first-time USBip
   installation can briefly restart USB hubs.
3. Turn on a supported device. If it is not paired with this adapter, use its
   ordinary pairing procedure and briefly press the adapter pairing button.

The host starts immediately and at the installing user's Windows sign-in. Models
are detected automatically, mappings are saved locally and the latest connected
headset becomes the default playback/microphone device. There is no control window,
tray, overlay, manual mapping or normal backend selection. Application-pinned audio
outputs can override Windows defaults.

Full [installation instructions](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.4/docs/INSTALL.md)
and [troubleshooting](https://github.com/Ktoto59/xone-windows-wireless/blob/v0.1.0-alpha.4/docs/TROUBLESHOOTING.md).

## Changes from alpha.3

- C++ host and in-process C++ USB/IP/GIP/audio servers replace the resident Python
  and PowerShell supervision/data path. USBip attach/detach commands run briefly
  on lifecycle changes.
- Automatic model detection for a Series controller and Xbox Wireless Headsets
  2021/2024; one device per model, with independent connection generations.
- Automatic Windows default playback/capture routing and conditional restoration
  of earlier defaults. A later manual default change is respected during restoration.
- No normal text logs or per-packet captures. Small local ownership/error snapshots
  are written only when their state changes.
- Headset-clock audio pacing, bounded queues, per-peer recovery and bounded host
  retries remain in place. The direct synthetic GIP experiment is not used.

## Architecture and limits

This is a user-mode radio/USB-GIP bridge using signed Windows WinUSB and signed
upstream usbip-win2, rather than a new kernel driver. The application/Setup EXEs
are not Authenticode-signed. No Developer Mode, test signing or Secure Boot change
is required. PowerShell is used for setup/removal only.

Supported: external adapter `045E:02FE`, Series profile `045E:0B12`, headset 2021
profile `045E:0B17`, headset 2024 profile `045E:0B32`.

- Xbox Accessories can show **Connected** instead of a wireless battery indicator.
- Firmware updates and controller audio-jack streaming are unsupported. Update
  device firmware only through a real USB cable.
- Wireless authentication v3/stock RF security parity is incomplete; the raw route
  must not be assumed to have the stock driver's authenticated/encrypted RF link.
- Startup is per user at sign-in, not a system service before sign-in.

Both headsets and a Series controller previously worked together on the USB/GIP
route. The new headless host was checked on one development PC: the 2024 headset
played clean ordinary audio with no reported stutters during a short user check.
40 session checks and model/config validation passed, and Setup compiled. This
does not establish long-duration stability or fresh installation on another PC;
those, repeated power cycles and broad sleep/resume behavior remain pending.

## Downloads, upgrade and removal

- **Setup x64 EXE:** automatic installation; no separately installed Python needed.
- **Windows x64 ZIP:** matching binaries/source for development, not an automatic installer.
- **SHA256SUMS.txt:** checksums for both downloads.

Run Setup as the same user to upgrade. Same-folder local data are retained; a
recognized older background installation can be stopped/migrated. Keep older
recovery data. Uninstall with the adapter connected restores only the stock binding
owned by this installation. Shared USBip and private recovery data are retained.

Device addresses, local configs, firmware, Microsoft drivers, driver backups,
captures, recordings and test certificates are excluded from these assets.
Source is GPL-2.0-or-later with third-party notices. Alpha.3 assets remain unchanged.
