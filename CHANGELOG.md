# Changelog

## Unreleased

- Validate matching stock-driver recovery metadata, registry snapshots and the
  exported driver package before accepting an existing WinUSB installation or
  completing setup. Migrate the entire backup with hash checks before removing
  the old startup task, preserving its folder reference and original data on
  failure. Unknown WinUSB ownership now requires explicit recovery at setup time.

- Make the Start menu shortcuts switch adapter modes: Stop pauses the owned
  startup task, stops the bridge and restores its saved Microsoft driver; Start
  selects WinUSB and launches the limited user-session task. Restore the saved
  sign-in preference only when returning to bridge mode. Request UAC only for a
  driver change; report cancellation, missing backups and restart/reconnect
  requirements visibly. Keep installer/runtime-only Start/Stop actions separate.
- Test switching with mocked drivers/tasks, guard installation/adapter ownership
  and block upgrade/removal during an active shortcut operation. Hardware mode
  switching still needs validation; a cold adapter reconnect may be necessary.

## 0.1.0-alpha.6 - 2026-10-17

Promote the tested experimental changes below to the main release channel.
Alpha.5.3-alpha.5.7 were local test candidates; alpha.6 is published as a pre-release
with Setup, runtime/source archives and SHA256 checksums. Software validation does
not establish that the remaining audio/reconnect issues are resolved.

- Integrate independent sign-in startup and start-now choices into Setup, remember
  them on upgrade and preserve opt-out during elevated adapter preparation. Add
  Start menu start/stop shortcuts and visible maintenance stages; defer start-now
  when Windows requests a reboot. Test registration without launch and installer
  choice persistence without executing hardware hooks.

- Add the Radio Wave project logo and multi-resolution Windows icon to native
  executables, Setup, Installed apps and the uninstall shortcut. Build all three
  installed EXEs together and include branding source assets in the source archive.

- Remove obsolete PowerShell control panels and C# launchers; keep Python and
  hardware research entry points. Add documentation/script indexes and group
  historical release notes under `docs/releases/`. Include all public developer
  scripts and documentation in the source archive, retaining the separate
  installed-runtime allowlist.

- Move installed native runtime state and stop commands to bounded shared memory
  and Windows events; remove periodic state-file IO and file polling. Keep counters
  in RAM and defer newly learned pairing metadata until radio shutdown.
- Isolate native GIP transmission in a bounded executor using overlapped WinUSB
  OUT, preserving real completion, control priority, media expiry and cancellation
  across URB/USB/RF lifetimes. Leave startup/teardown control operations synchronous.
  These changes need hardware validation before claiming an audio improvement.

- Prepare the alpha.5.4 installer candidate from `experimental`. Installer
  builds disable C++ diagnostic journals and packet tracing at compile time;
  development builds retain diagnostics. Keep release build outputs separate
  from the running development candidate and test that environment overrides
  cannot activate a release journal. Operational recovery/status files remain.

- Add a process-local native USB/IP MMCSS override for controlled development
  comparisons (`Run-Development.ps1 -NoUsbMmcss`). Expose sparse initialization
  progress and aggregate worker scheduling counters without packet logging.
  Default scheduling and protocol behavior remain unchanged.

- Recover failed automatic USB routes through their owning RF session instead
  of leaving an associated peer without a working Windows route. Expose failures
  and per-device recovery state; retry with a 1-to-30-second backoff, preserving
  other devices. Include native protocol-worker failures in the same recovery.
- Seed repeated USB imports with a validated, bounded identity announcement;
  discard stale service replies from the preceding USB consumer. Preserve the
  identity across audio recovery, and clear it on a new radio association.
  Apply the import lifecycle to both the C++ runtime and Python reference server.
- Coalesce capture/render loss reports once per second. Count microphone frames
  received without a USB consumer separately instead of flooding anomaly logs.
  These lifecycle fixes do not establish that keyboard stalls or physical
  adapter removal have been resolved; hardware checks remain necessary.
- Isolate reliable radio/control traffic from microphone media in separate
  bounded queues, prioritize controls before unsent audio, and retain lifecycle
  and per-class ordering. Media pressure/age cannot consume control capacity.
  Check render deadlines again before physical USB TX; deliberately expired PCM
  completes with ISO expiry without resetting the USB/RF session in C++/Python.
  Log control saturation explicitly and aggregate microphone queue-drop reports.

- Add wake-capable idle power for the shared C++ radio runtime: arm firmware
  wake-on-wireless after 30 seconds without clients/pairing, allow WinUSB selective
  suspend, resume before dispatch, and provide a 30-second watchdog receive window.
  Keep IN reads pending without periodic USB re-submission; cancel/drain them on
  stop. Save/restore adapter-specific idle registry settings with driver binding.
  WakeArmed is a requested policy, not proof of USB sleep or a temperature result.

- Preserve radio retry backoff across USB disappearance/re-enumeration and
  resume. Cap repeated startup retries at one per 60 seconds and reset only
  after a continuous stable run; cover rapid USB return with regression tests.
- Default development C++ diagnostics to errors/anomalies and sparse lifecycle
  context; require `-CppPacketTrace` for per-transfer logs.

- Restore a closed USBIP import per slot while retaining a live RF association.
  Audio TX failures/ACK timeouts now invalidate the USB session explicitly and
  allow re-import without a new RF announcement, in both Python and C++.
- Align C++ render expiry with Python per-ISO errors and frame numbers. Preserve
  the oldest accumulated PCM deadline and report staged media loss explicitly.
- Add bounded asynchronous C++ lifecycle/anomaly journals, opt-in USB/URB timing
  metadata and a C++ candidate mode in the development launcher. Cover recovery,
  audio parity, actual loopback re-import and journal faults with regression tests.
- Separate the stable `main` channel from `experimental` development. Add a
  Python development launcher with source-wide call/exception tracing, optional
  line tracing, process/task/URB correlation and independent rotating anomaly
  journals. Record audio loss, queue pressure, scheduling lag, child exits and
  trace losses; keep the installed/release runtime C++.
- Include endpoint, requested byte count and UTC in fatal WinUSB transfer errors
  so physical read failures can be distinguished from shutdown write failures.
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
