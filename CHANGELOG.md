# Changelog

## Unreleased

- Publish fallback GIP status immediately on state transitions rather than on
  every control packet; repeated battery/audio-control and ignored messages no
  longer bypass the normal status-write interval.
- Restore the MT76 startup fixes lost during the C++ bridge migration: allow
  firmware startup to settle after warm reset/cold upload, apply the cold RF
  patch, and clear retained wake-on-wireless routing before enabling beacons.
- Exercise delayed MCU readiness, cold RF setup and retained wake-only routing
  in the protocol tests. These checks use a mock adapter; physical validation of
  the restored startup path is still required.

## 0.1.0-alpha.5.2 - 2026-10-15

- Fix WinUSB binding failure when Setup starts 32-bit PowerShell: relaunch
  maintenance in native x64 PowerShell before loading modules or dependencies.
- Resolve PnPUtil and CAB extraction through the native Windows system directory,
  including the stock-driver export used before binding. Do not depend on PATH.
- Test real 32-bit/64-bit tool execution with empty PATH, bootstrap argument/exit
  propagation and installed-script external command paths in Windows CI.

Validation: reproduced missing PnPUtil under WOW64; fixed paths invoke the real
native PnPUtil help without changing drivers. Hardware setup on the second PC
still needs confirmation. The C++ radio/audio binaries are unchanged.

## 0.1.0-alpha.5.1 - 2026-10-15

- Automatically install a missing signed Microsoft adapter driver, prepare
  verified firmware and install/repair the tested signed USBip dependency. No
  previous Xbox hardware setup, Python, Accessories or VC++ redistributable needed.
- Fix first-install firmware downloads: use Microsoft's catalog HTTPS host with
  valid certificate verification, and pin the CAB SHA-256 before extraction.
- Preserve maintenance stage, error and transcript locally, including helper-load
  failures. Display the actual failure in Setup rather than a generic retry prompt.
- Recognize all four upstream xone adapter IDs consistently in setup, binding and
  the background host. Honor the saved adapter instead of picking another radio.
  Only 045E:02FE is physically validated; other models need hardware testing.

Validation: isolated maintenance failure/retry checks; fresh-directory extraction
and verified import from the official CAB; adapter catalog/configuration checks.
Actual installation on the reported second PC remains to be confirmed.

## 0.1.0-alpha.5 - 2026-10-15

- Move automatic USBIP/GIP, PCM processing and background management to C++.
- Select per-revision connectors from RF announcements before USB export.
- Give every association independent queues, clocks, tokens and stable locally
  derived serials. No developer device allowlist or pre-filled MAC mapping.
- Preserve 2021 headset wireless reconnect with an exact USB mode-6 compatibility
  filter and an independent audio clock; keep the 2024 connector separate.
- Bound capture/render age and stale audio; interleave shared radio submissions.
- Fix host removal of a device when its status file is temporarily unreadable.
- Respect Windows/user audio selection instead of selecting the newest headset.
- Produce a minimal installer/runtime package and separate corresponding source.
- Replace historical front-page instructions with current English installation,
  pairing, privacy and limitation guides. Explicitly prohibit firmware updates
  through the bridge and explain physical USB updating.

Validation: 99 Python/reference and loopback checks; C++ session/codec/core and
snapshot checks; setup/configuration checks; isolated Installed apps registration
and removal. Both physical headset revisions and a Series controller connected
on one PC, with clean short playback reported. Broad hardware setup/restoration,
long-duration voice/music and sleep/resume remain unverified.

## Earlier previews

Alpha.1-alpha.4 explored Python supervision, fixed model slots, C++ transport and
invisible startup. They remain historical releases with version-specific source
and known limitations. Use alpha.5 instructions for the current implementation;
old control-panel, fixed-address and audio auto-selection instructions do not apply.
