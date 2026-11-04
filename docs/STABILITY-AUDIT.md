# Connection stability audit

Date: 2026-11-04. Branch: `audio-recovery-stock-driver`.
Audited base: `67cb04436dffe81448566f7976f53a60e93e72f0` (matched origin after fetch).
Scope: experimental Windows 11 connection recovery. No release, driver binding,
physical adapter access, pairing or power transition was performed.

## Findings and changes

| Area | Finding in the audited base | Change / evidence |
| --- | --- | --- |
| Radio stop (P1) | `Host::stop_radio` detached ports before requesting stop; one throwing detach skipped radio termination and all later ports. Eight hung commands could also exceed the stop client's wait. | Request graceful stop first (8 s), close the kill-on-close job, then isolate each owned port. Cleanup has a shared 16 s budget and at most 2 s per port, including ownership lookup and detach. Regular USBip calls observe the host stop event; later slots do not start commands after stop. |
| Process lifetime | Jobs already used `KILL_ON_JOB_CLOSE`; process resume errors were ignored. Descendants of an already exited CLI still needed job cleanup. | Check `ResumeThread`; close the job before releasing the process handle and observe termination. Tests cover a hung process, a live child tree, descendants of an exited process, cancellation and handle counts. |
| Port ownership | Only recorded attach port numbers were detached, but a number could be reused externally after automatic VHCI removal. | Before detach, verify exact loopback endpoint, bus ID and expected CLI serial metadata. Absent/reassigned ports are forgotten without detach; unknown output preserves the record and reports an error. No detach-all or foreign-port adoption. |
| Cleanup errors | A second failed `usbip port` command could mask the detach error. | Retain both failures, resource names and exit codes in diagnostics / `CleanupErrors`. Reconciled detach failures are journaled too. Failed port records survive for a later cleanup attempt, including before a new RF client appears. |
| Per-device failures (P2) | A throw in the first slot aborted the entire host peer loop. | Isolate each slot. Independent 1/2/4/8/16/32/60 s backoff, capped at 60 s, resets after 60 s of continuously imported healthy state. Radio restarts interrupt the stability interval. Other USB workers keep running. |
| Control TX stall (P1) | Five-second outbound deadlines existed, but failed/timed-out interrupt OUT requests only failed the URB. GIP initialization could remain stranded. | An actual failed or expired control TX requests local USB recovery, cancels old work, preserves RF association and reuses the identity-only replay path. Existing PCM-expiry behavior is unchanged. |
| TCP stall (P1) | Imported clients intentionally had no idle timeout; an incomplete request or an unsent response could remain indefinitely below queue limits. | Close only the affected import after 15 s without partial-request byte progress or response-write progress. Empty sockets and waiting IN URBs have no idle deadline. Response enqueue is not write progress. |
| Import observations (P2) | One `UsbImported=false` snapshot older than a two-second grace could trigger detach, including a delayed pre-attach publication. | Add snapshot sequence and Windows uptime publication stamp. Only publications produced after successful attach confirm absence; require three distinct negative publications and at least 10 s for an import never observed. Explicit generation changes / RF removal remain immediate; confirmed TCP close does not wait for that grace. A currently imported snapshot takes precedence over earlier close counters. |
| Generation collision | The legacy public generation is `association*100+core_generation`; after 100 recoveries it can equal a subsequent route's generation. Snapshot sequences and import counts also restart on a new route. | Compare the independent existing `AssociationId` as well. Preserve the public generation representation for existing consumers. Regression uses an exact composite-generation collision. |
| Repeated association (P2) | Every repeated MAC was ignored, including a new attempt after failed authentication or exhausted identification. | Retain 802.11 sequence control and retry bit. Replace only a confirmed failed stage with a distinct, non-retry association received later; enforce a five-second per-MAC cooldown. Healthy repeats and in-progress initialization remain untouched. |
| Old WCID events | TX epochs/tokens were protected, but received events were not checked against the current MAC/association arrival boundary. | Reject queued older events and addressed events from another MAC before dispatch, including disassociation and encryption enable. Existing TX cancellation/epochs remain intact. |

The ownership output format was checked against the pinned dependency's
[usbip-win2 v.0.9.8.1 port implementation](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/userspace/usbip/port.cpp).
The CLI [attach implementation](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/userspace/usbip/attach.cpp)
and [driver property initialization](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/drivers/ude/context.cpp)
show that the serial in this listing is the attach override, not the remote USB
string. Our unchanged attach command has no override: expected CLI serial is
empty, and a nonempty override is a foreign identity. The pinned dependency's
[`SERIAL_BUFSZ`](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/include/usbip/consts.h) is 16, so supplying the server's twenty-character `VirtualSerial`
would be invalid. No override is added and Windows continues to read the same
existing serial from the native server. Empty CLI metadata and a different
nonempty override are both regression-tested.
A successful empty listing means no imports; an error exit is never treated as
an empty listing. The identity check and numerical detach are separate CLI
operations: a hostile concurrent administrator could still race them. There is
no atomic detach-by-identity API in the current integration. Unknown output
fails closed and may require a later cleanup retry.

The pinned driver's [connection completion path](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/drivers/ude/vhci_ioctl.cpp)
completes the synchronous attach request after `import_remote_device`, device
creation and plugging. Host import observation starts only after that successful
CLI return. Slow GIP initialization afterward therefore does not explain
`UsbImported=false`: the TCP import has already completed. For the ambiguous
never-observed case, 10 s allows five existing two-second publication intervals
and up to two missed publications, while still requiring three distinct live
negative snapshots. Explicit command failures, generation changes and confirmed
post-attach TCP closes use their direct evidence rather than that grace.

## Existing protections retained; hypotheses not used as reset signals

* `NativeDeviceRoute` / `AnnounceWatchdog` already detect a missing initial GIP
  announce after five seconds. `AutoGipRouter` contains peer faults; `Session`
  already applies independent RF-route retry delays. These were not replaced.
* `Core::ready()` describes RF/bridge availability, not successful Windows GIP
  initialization. Configuration, interface, interrupt delivery and GIP command
  counters are the relevant existing initialization evidence. A slow Windows
  handshake alone does not justify a new RF reset.
* Idle interrupt IN requests legitimately wait indefinitely. Missing microphone
  packets, stopped playback, mute and silence are not disconnect evidence.
  Neither the new socket guard nor control TX recovery uses audio absence.
* An explicit TCP close already preserved the RF association and allowed a clean
  import; the immutable announcement was already replayed without stale power,
  authentication or audio commands. That path remains the first recovery step.
* PCM remains untranscoded. USB TX/RX topology, separate threads, control
  priority, bounded queues, selective expiry, prediction horizons and the
  cancellation/draining of WinUSB OVERLAPPED operations are unchanged.
* Firmware idle wake, outstanding RX reads, a 30 s fallback wake with a 2 s
  receive window, and host Sleep/Resume notifications already exist.
  `prepare_idle_power` checks wake capability and adapter policy. Idle power is
  not disabled globally or changed on this machine.
* Shared physical USB errors still propagate to radio failure/restart. A local
  USB/IP error does not restart the physical adapter or healthy siblings.

## Deliberate limits and diagnostics

There is no safe RF heartbeat in the current protocol integration. A live but
quiet device cannot be distinguished from an RF wedge solely by missing traffic.
A firmware `lost` notification carries WCID only, without a MAC or epoch. If it
is generated late and arrives after WCID reuse, receiver timestamps cannot prove
it belongs to the old client. Same-MAC late GIP frames and sequence wrap likewise
cannot always be distinguished at the protocol boundary.

Accordingly, a repeated association for a client with no confirmed failure is
retained. A healthy-looking stale RF record after a lost disconnect is not
aggressively reset. A full radio restart is not triggered by stale diagnostics.
This is an intentional remaining limitation, not a claim that every RF wedge is
solved. Unsupported authentication can retry on a distinct explicit association
but is not made protocol-compatible by this patch.

`SessionTicks` exposes main-loop progress. Controller snapshots expose the
internal `SessionGeneration`, `RepeatedAssociations`, `AssociationSequence` and
`LastAssociationSequence` for diagnosing ignored requests without packet logs.
Native snapshots add `SnapshotSequence`,
`PublishedTickMs` and `UsbPending` counts for interrupt IN, control OUT, capture,
render, queued response bytes and partial request bytes. Existing initialization,
worker iteration, mailbox-delay, USB-close, TX-error and generation counters remain.
Counters use the existing publication cadence and memory IPC; no new polling
thread, packet journal or persistent process was added. These fields support
future hardware evidence before enabling a broader watchdog.

Host USBip operations remain synchronous and bounded. A failing command can
delay another slot's supervisor work by its command deadline, while independent
radio/USB/audio workers continue. Error isolation prevents indefinite starvation;
it does not promise a small fixed reconnection latency. Windows scheduling and
non-cancellable kernel failures can exceed user-mode timing budgets.

## Automated coverage and validation

The new tests require no adapter or real USBip executable:

* `host_recovery_tests.cpp`: detach and verification failures, EOF reconciliation,
  foreign/reused port and serial checks, all-slot cleanup, repeated stop, sustained
  first-slot failure, independent retry caps/reset, real hung helper/job trees,
  exited-parent descendants, stop cancellation and repeated handle accounting.
* `usb_import_recovery_tests.cpp`: slow publication/import, unchanged/missing
  snapshots, post-attach uptime boundary, old close vs healthy new import,
  coalesced open/close, generation collision, confirmed disconnection and virtual
  socket write-progress deadlines.
* `session_tests.cpp` / `protocol_tests.cpp`: sequence/retry parsing, healthy
  duplicate preservation, failed-stage reassociation without pairing, omitted
  disconnect, queued delayed events and MAC rejection after WCID reuse. Existing
  async completion and independent route recovery tests remain.
* `usbip_native_core_tests.cpp`: failed and five-second-expired control TX for
  headset 2021, headset 2024 and Series Controller; cancellation and stale token
  rejection, sibling preservation, indefinite IN wait, successful reimport and
  two hours of subsequent virtual-time idle service.
* `usbip_audio_recovery_tests.cpp`: 10/20/40/80/250/500 ms stalls, selective loss,
  continued valid session and no catch-up bursts.
* `usbip_native_transport_tests.py`: actual loopback control failure, diagnostic
  progress publication and a successful new USB import on the same RF session.

Validation on Windows x64 with MSVC 19.44, C++20, `/W4 /WX`:

| Command / suite | Result |
| --- | --- |
| `scripts\build.cmd` | PASS: full baseline C++ build; 17 protocol tests, 41 session tests, 5 runtime fault cases, timeout policy and automatic-router suites |
| `scripts\Build-CppBridge.cmd` | PASS: native bridge build |
| `scripts\Test-CppBridge.cmd` | PASS: native codec/core/audio/import/route/journal/snapshot suites, 7 parity tests and 8 native integration tests |
| `scripts\Build-Appliance.cmd` | PASS: host/driver-tool build and all configured C++ regressions |
| `scripts\Build-Appliance.cmd --no-diagnostics outputs\appliance-release` | PASS: diagnostics-disabled build, regression suites and disabled-diagnostics check |
| `python -m unittest discover -s tests -p '*tests.py'` | PASS: 122 tests |
| Windows CI maintenance scripts plus `stock_driver_tests.ps1` | PASS: nine PowerShell test scripts, all driver/task operations mocked |
| Stock bundle preparation and `bundled_stock_driver_tests.ps1` | PASS: four pinned authentic CABs, signatures/HWIDs and offline simulated recovery; no real driver binding |
| `python scripts/check-public-content.py`, `git diff --check` | PASS |

The maintenance scripts were `system_tool_tests`, `setup_action_tests`,
`automatic_bridge_tests`, `mode_switch_tests`, `recovery_migration_tests`,
`device_config_tests`, `setup_helpers_tests`, `adapter_idle_power_tests`, and
`stock_driver_tests` (all `.ps1`). The idle-policy test uses a new temporary HKCU
fixture; it does not change adapter or system power policy.

Logs remain local under `work/stability-*.log`. No build outputs or private
logs are added to public source.
`Build-Appliance.cmd` now compiles and runs the host regression executable in
both normal and diagnostics-disabled builds. The existing Windows workflow
already invokes this script and `Test-CppBridge.cmd`; no workflow change or
additional dependency is required. Hosted GitHub Actions was not dispatched.

## Coverage of the requested stress scenarios

| Requested scenario | Automated evidence / remaining hardware work |
| --- | --- |
| 1. Normal connect/disconnect | Session, native core and real loopback imports; physical RF still needs hardware |
| 2. Fast headset power cycle | Synthetic removal/reassociation and slot reuse; exact device power timing requires hardware |
| 3. Lost RF disconnect | Missing-disconnect failed-stage reassociation tested; healthy-looking stale association deliberately remains ambiguous |
| 4. Repeated existing MAC | Healthy duplicate, retry-bit and distinct failed-stage attempt tested |
| 5. WCID reuse | TX epoch/token, old queued event and wrong-MAC guards tested; untagged late firmware lost remains a protocol limit |
| 6. Delayed USB TX completion | Existing asynchronous queue/drain tests and new five-second control timeout tests |
| 7. Hung/closed TCP | Virtual write-progress deadline and actual TCP-close/reimport tests; no idle socket timeout |
| 8. Slow import | Virtual fresh/stale snapshots, import grace, old close and publication uptime boundary |
| 9. Detach failure | Injected detach, verification failure, unknown output and successful EOF reconciliation |
| 10. Exited/hung radio child | Actual disposable helper processes, jobs, descendants and cancellation; no physical radio executable is run |
| 11. Adapter removal/return | Retry/model/maintenance mocks and code audit; physical re-enumeration needs hardware |
| 12. Sleep/Resume | Idle policy state-machine faults and host notification audit; real S3/Modern Standby needs hardware |
| 13. Multiple devices | Actual loopback multi-route imports and model/instance isolation |
| 14. One failing device | Persistent first-slot fault and raw-route / core sibling isolation |
| 15. Long silence | IN remains pending for one virtual hour; recovered session remains healthy for two virtual hours |
| 16. 10/20/40/80/250/500 ms pause | Deterministic PCM/command/write-progress cases; no RF reset on ordinary pauses |
| 17. Repeated failed recovery | Independent virtual backoff, cap and interrupted stability window |
| 18. Recovery followed by long session | Reimport, successful later control/audio requests and virtual idle soak; prolonged real duplex audio is a hardware acceptance item |

## Concurrency and resource review

New import observations and retry state are owned by the host thread. Association
metadata is owned by the session thread. Socket write-progress and pending counts
are owned by the existing USB worker; the existing mutex still owns cross-thread
mailboxes and TX vectors. No new production lock, worker thread or shared mutable
pointer was introduced. Job/pipe handles retain RAII ownership on exception paths;
repeated timeout/exit tests compare process handle counts. Existing cancellation
and stale-completion tests exercise teardown while asynchronous TX is pending.
This is code review plus deterministic testing, not a race-detector proof.

## Changed files

* `docs/STABILITY-AUDIT.md`
* `scripts/Build-Appliance.cmd`
* `src/background_host.cpp`
* `src/host_cleanup.hpp`
* `src/main.cpp`
* `src/owned_usb_port.hpp`
* `src/peer_recovery_backoff.hpp`
* `src/protocol.cpp`
* `src/protocol.hpp`
* `src/session.cpp`
* `src/session.hpp`
* `src/usb_import_recovery.hpp`
* `src/usbip_client_progress.hpp`
* `src/usbip_native_core.cpp`
* `src/usbip_native_core.hpp`
* `src/usbip_native_output.cpp`
* `tests/host_recovery_tests.cpp`
* `tests/protocol_tests.cpp`
* `tests/session_tests.cpp`
* `tests/usb_import_recovery_tests.cpp`
* `tests/usbip_audio_recovery_tests.cpp`
* `tests/usbip_native_core_tests.cpp`
* `tests/usbip_native_transport_tests.py`

## Hardware comparison procedure (not executed)

Use a dedicated test installation of the candidate. Save the current app-local
`IdlePowerEnabled` value and use normal Stop / Start to change only this setting
between runs. Do not edit adapter registry policy, swap drivers, pair again or
change system-wide power settings. Restore the saved app setting afterward.
Run the following identically with `IdlePowerEnabled=true` and `false`; record
adapter model, Windows build, headset firmware, USB hub/topology and power source.

| Scenario | Repetitions / duration | Evidence and acceptance |
| --- | --- | --- |
| Baseline connect, power-off/on | 30 normal cycles per headset and controller; then 30 cycles with 1 s and 3 s off/on intervals | Existing pairing reconnects; inspect removal reason, AssociationId, generation and Windows endpoints. No new pairing required for 2021. |
| Empty adapter idle | 1, 10, 60 minutes; power on a paired device at each boundary | Compare `RadioPower`, idle-arm/wake/resume events, first association, first announce and first import. Check remote wake and fallback receive window separately. |
| Connected silence/mute | 2 h per device, playback paused, mic muted and no applications using endpoints; then resume playback/capture | No recovery, detach, RF removal or radio restart caused by silence. Endpoints stay usable afterward. |
| Sleep/Resume | 20 cycles after idle and 20 during bidirectional audio, using the machine's normal sleep mode | One orderly host stop/start per resume, old jobs exit, all peers reconnect without USB replug or pairing. Record Modern Standby vs S3. |
| Adapter removal/reconnect | 20 cycles while idle and during two-device use, including rapid return | Physical-adapter backoff persists, no restart storm, all sessions return. This expected global outage is distinct from a local peer failure. |
| Multiple devices / local failure | Headset 2021, 2024 and controller together; repeatedly power-cycle just one | Other devices retain imports/endpoints and continue audio/input. Record any shared RF outage rather than silently accepting it. |
| Recovery then soak | Cause one ordinary supported device power cycle, then 8-24 h of playback/capture and controller use | Per-peer error/retry state clears after stable operation; no growing handles, child processes or queues. Check playback and recording audibly, not counters alone. |

Collect existing bounded anomaly journals and manual status snapshots at each
transition using `xone-background.exe --status` from the test installation.
Do not run a fast file-polling loop or enable continuous packet capture for the
soak. Compare `SessionTicks`, worker iterations, pending control/response bytes,
initialization counters and TX failures when a failure occurs. A no-wake result
only with idle power enabled is evidence for a subsequent targeted investigation,
not permission to turn off energy saving globally in this patch.

The candidate is intended for this hardware soak after automated validation.
Automatic tests establish isolation and deterministic recovery behavior, not
absence of RF/audio regressions on physical 2021/2024 hardware.
