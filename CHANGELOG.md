# Changelog

## Unreleased — automatic background bridge and radio reconnection

- Recover configured USB/GIP peers that associate but send no initial GIP data:
  after five seconds, invalidate only that peer's USB generation, clear its
  firmware slot and resend association, up to three attempts. Established idle
  peers and active siblings are preserved. Later reassociations refresh the
  Windows/GIP generation; immediate WLAN retries are debounced for one second.
- Confirm one warm bridge restart on the new headset without USB replug: the
  first targeted recovery resumed audio, and the user confirmed clean playback.
  Two new session regressions pass with the existing checks (35 session tests).
  Repeated restarts, old-headset cycles and sleep/resume remain unvalidated.

- Override inherited BelowNormal scheduling for audio servers and the RF process
  with AboveNormal, and register audio event-loop threads with Windows MMCSS
  Pro Audio. Live priority adjustment reduced reported stutters under system load.
  After applying the complete change and cold-resetting the adapter, the user
  reported that playback returned and stutters appeared to be gone at the same
  load. A later targeted session recovery restored one warm restart without USB
  replug as recorded above; sustained load and repeated recovery remain unvalidated.
- Remove synchronous status-file writes from radio callbacks. Publish snapshots
  on a disk worker at most five times per second, including during idle periods.
  Verify USB requests remain responsive while a status write is blocked.

- Select the existing signed WinUSB/usbip-win2/Windows GIP path for deployment;
  abandon kernel certification/test-signing as a delivery requirement. The local
  development certificate trust was removed without changing boot security.
- Add a limited-privilege, password-free per-user Task Scheduler host with
  sign-in startup, unlimited sessions, waiting for absent adapters and bounded
  worker restart. Add a resume-recovery code path, not yet tested after sleep.
- Add Enable/Disable automatic mode to the panel and installer; coordinate
  discovery, configuration, driver changes, upgrade and uninstall with the host.
- Confirm automatic worker restart and radio restart after physical USB replug.
  A fresh pairing following headset connection reset completed; the user confirmed
  clean ordinary playback on the new headset without USB-C. Longer/simultaneous
  audio, old-device reconnect and warm-start behavior still need validation.

- Respond to configured USB/GIP peers' radio re-enrollment requests outside the
  manual pairing window. Unknown peers still need manual pairing; matching stale
  peer/USB generations are removed without disturbing siblings.
- Open a 60-second association/pairing window at radio startup, with channel scans
  paused while clients are present. Repeated button events refresh scan timing.
- Port cold-start RF patch, MCU startup delays and wake-on-wireless routing reset
  from dlundqvist/xone v0.5.8. No adapter firmware image or driver binding changes.
- Filter invalid association MACs and incomplete beacon-loopback MPDUs before
  client allocation/header parsing. Add software regression checks for these paths.
- Pace complete eight-slot capture blocks by actual radio arrival rather than a
  second host timer. Bound the real-time capture FIFO to 64 one-ms frames, discard
  stale PCM on overrun with diagnostic counters, and keep the command connection
  alive. Oversized capture requests fail explicitly instead of waiting forever.
- Add render peak/nonzero-packet counters without logging PCM or recording voice.

Longer hardware validation exposed capture FIFO drift and a fatal queue-limit
disconnect. Software regressions cover stalls beyond the former 2048-frame bound,
recent-PCM retention, command continuity, device-clock delivery and reset handling.
The corrected change has since received the limited new-headset confirmation
above; broader hardware/audio validation remains pending.
The published alpha.2 assets remain the frozen installer version.

## 0.1.0-alpha.2 — 2026-10-13

- Windows x64 Setup EXE with per-user installation, Start menu/desktop shortcuts
  and an uninstall entry.
- First-run WinForms control panel for dependency preparation, adapter binding,
  association discovery and device mapping, bridge start/stop and stock restoration.
- Isolated official Python 3.13.16 runtime included in Setup with its license.
  USBip and firmware are obtained separately from their official sources and checked.
- Uninstall/upgrade checks for a stopped bridge; removal restores only the saved
  binding owned by that installation and refuses deletion when restoration fails.
- Unlimited launcher sessions with `-Seconds 0`, used by the control panel.
- Setup helper tests for checksum rejection, atomic mapping, radio-address parsing,
  process identities and uninstall guards. No radio/audio protocol changes.

Headset 2021 reconnection and Accessories' Connected battery label remain unresolved.
The installer does not turn this user-mode/virtual USB prototype into a kernel driver.

## 0.1.0-alpha.1 — 2026-10-13

First public snapshot of the working Windows x64 research prototype.

- xone-derived WinUSB/MT76 radio transport, client handling, supported v1/v2
  authentication, protocol parsing and fragment/ACK handling.
- Captured native USB/GIP profiles for Series, Headset 2021 and Headset 2024.
- Windows Xbox Accessories detection/settings opening for tested hardware.
- Native XInput input and short clean stereo headset-tone validation.
- Radio-clock render pacing and eight-slot USB/ISO audio adaptation.
- Per-device brokers and ownership checks for concurrent devices.
- Radio-to-USB fragmentation preserving original ACK boundaries.
- Physical battery observation and read-only WGI battery diagnostics.
- Snapshot locking/recovery fixes, with a live blocking-file regression check.
- Public device configuration without embedded owner MACs or adapter serials.

Known issues include Headset 2021 requiring re-pairing/search recovery after a
power cycle, Accessories' Connected battery label, unimplemented wireless auth v3,
unverified raw-peer RF encryption, no firmware-update forwarding, and incomplete
long-term/audio/reconnection validation. See README for the full scope.

Local hardware observations preceded publication cleanup. The public wrapper uses
private per-user configuration instead of the development machine's fixed IDs.
Protocol/radio/audio core behavior is retained. Software checks validate the
configuration changes; broad hardware compatibility is not claimed.
