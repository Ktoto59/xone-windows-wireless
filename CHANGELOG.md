# Changelog

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
