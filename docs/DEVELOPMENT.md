# Development

## Branches and release path

`main` is the stable/release channel. Develop and diagnose on `experimental`;
promote reviewed changes to `main` after synthetic checks and explicit hardware
validation. A channel name is not evidence of hardware stability: the existing
alpha.5.3 headset disconnect remains under investigation.

The development runtime is Python for supervision, USBIP URBs, GIP routing,
model connectors and PCM conversion. It shares the C/C++ MT76/WinUSB radio
executable with the production implementation. Python is not automatically
translated to C++: behavior changes must be ported and validated against the
C++ transport/protocol tests before release. A clean Python run cannot establish
that a failure in the C++ native USBIP path has been fixed.

The installed runtime remains C++ (`Build-Appliance.cmd` / `Build-Installer.ps1`).
The runtime package uses an explicit allowlist; Python and development tracing
are only included in the separate source archive.

## Development session

Install Python 3.11+, build `outputs/xone-probe.exe` with `scripts/build.cmd`,
prepare the verified firmware under `work/firmware` using the existing import
script, and install USBip. The adapter must already use WinUSB. This launcher
does not change bindings, power policy, pairing, or installed binaries.

```powershell
# Five-minute foreground-controlled session; restores a running installation.
.\scripts\Run-Development.ps1 -TakeOverInstalled -Seconds 300

# Also trace each executed Python line (much higher overhead).
.\scripts\Run-Development.ps1 -TakeOverInstalled -TraceLevel lines -Seconds 300

# C++ errors/anomalies and sparse lifecycle context, until explicitly stopped.
.\scripts\Build-Appliance.cmd
.\scripts\Run-Development.ps1 -Backend cpp -TakeOverInstalled -Seconds 0

# Explicit short packet/URB trace when individual transfers need investigation.
.\scripts\Run-Development.ps1 -Backend cpp -TakeOverInstalled -CppPacketTrace -Seconds 60
```

`-TakeOverInstalled` stops the owned installed host for the session and requests
its restart during cleanup. Without it, an active installed host is a conflict.
The development supervisor only imports/detaches its own virtual USB ports.
`-Seconds 0` runs until stopped; Ctrl+C requests cleanup. If cleanup cannot finish
within 30 seconds, the launcher reports it and does not start a competing host.
Do not unplug the adapter or stop the host during an unplanned-drop capture:
those deliberate actions would also appear as disconnects in the logs.

Each invocation creates an ignored `work/development/<UTC-session>/` directory:

| File | Contents |
| --- | --- |
| `session.json` | Branch/commit/dirty state, radio binary hash, duration and cleanup result |
| `supervisor-<pid>.jsonl`, `worker-<association>-<pid>.jsonl` | Full Python trace and semantic events |
| `anomalies-<role>-<pid>.jsonl` | Exceptions, protocol/USB failures, unexpected RF EOF/removal, failed imports, loss/expiry/overflow and scheduling warnings |
| `<role>-<pid>-health.json` | Attempted/written/dropped events, anomaly loss, I/O errors, observed functions |
| `supervisor-stdout.log`, `supervisor-stderr.log` | Supervisor console and uncaught traceback |

Radio stdout/stderr and worker console logs remain under `work/auto-usb`; their
paths and child identities are recorded in the trace/state. A radio exit event
also copies the last eight stderr lines into the anomaly journal. WinUSB fatal
errors include endpoint, requested length and UTC, preserving the original
Windows error before formatting/cleanup.

The C++ mode runs `outputs/appliance-candidate/xone-background.exe` and its radio
child, using the checkout's configuration/firmware and selected adapter. It
does not replace installed binaries. The launcher gives Python and C++ children
the same capture directory through `XONE_CPP_LOG_DIR`. C++ packet/URB tracing is
off by default, including ordinary empty-read timeouts and successful transfers.
Errors, anomalies and sparse lifecycle transitions remain enabled. Use the
explicit `-CppPacketTrace` switch only to investigate individual transfers; this
can saturate the ordinary journal and produce `trace_loss` warnings. The switch
also controls the shared C++ radio in Python sessions; Python call/line tracing
is separate and remains controlled by `-TraceLevel`. C++ uses independent 2048-event
ordinary/anomaly queues, 8 MiB journals with three backups and one-second writer
shutdown; see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

Tracing covers calls/resumes, returns/yields and exceptions in **every Python
runtime module under `src/`**, including connectors and telemetry. `lines` adds
executed source lines. The trace writer itself and third-party/standard-library
internals are excluded to avoid recursive logging. Values of function arguments,
return values, locals, PCM, authentication and key buffers are not recorded.
Every file identifies the session, process, thread, asyncio task where available,
UTC, monotonic timestamp and process-local sequence. `resumable=true` means a
coroutine/generator call/return can be a resume/suspension, not a new invocation
or final return. The source manifest includes hashes; the health file lists the
functions actually executed, not a claim that every branch was exercised.

Semantic events record USB submit/complete/unlink headers, endpoint, request
sequence, status, pending URBs, IPC kind/WCID/token/length, command queue waits,
backend duration, status transitions and audio counters. Development events
continue after the legacy 1000-event console cap. Repeated caught exceptions
are useful during diagnosis; normal StopIteration/GeneratorExit/cancellation
control flow does not enter the anomaly journal merely for its exception type.
An unexpected socket EOF is also reported by its owning component.

Audio drops/expiry, concealment and queue overflow are logged at their source;
they do not wait for the next status snapshot. Anomalies share the exact sequence
number with the full trace. Scheduling lag
over 20 ms, a new radio audio gap maximum over 32 ms, a new ACK maximum over
20 ms, and a render wait exceeding the model's configured render budget are
diagnostic thresholds, not established end-to-end latency measurements. Loss,
concealment, expiration and overflow counters generate anomalies on increases.

Each process has independent bounded queues for full trace and anomalies (8192
records each), with a dedicated writer thread. Saturation drops new records
instead of blocking radio/audio; `trace_loss` and health counters disclose losses
in both queues. Storage failures are counted rather than raised into transport
code. Each journal rotates at 16 MiB with seven backups (128 MiB per journal per
process); rotation evicts the oldest events. Console/radio companion logs and
previous sessions are not covered by those JSONL limits. A permanently blocked
writer has a bounded shutdown wait; its health may then be stale. Detailed
tracing adds overhead, especially `lines`: use loop-lag/loss counters and repeat
important results on C++ before drawing performance conclusions.

Logs contain local process/device metadata. Keep them under ignored `work/`;
do not publish raw diagnostics with the code.

## Build and check

### Wake-capable radio idle (experimental)

The shared C++ radio runtime (`run`, including the Python supervisor's radio
child) can arm firmware wake-on-wireless after 30 seconds with no RF clients and
no pairing session. A connecting or not-yet-identified client prevents sleep.
Persistent overlapped IN requests have no USB timeout; waiting on their Windows
events does not re-submit USB reads. Stop cancels and drains the pending reads.
The session loop uses a slower timer while standby is armed.

Firmware wake is armed before enabling WinUSB `AUTO_SUSPEND`. On a meaningful
radio event, USB autosuspend is disabled and firmware resumed before dispatch.
While armed, a cached PnP power-property check also detects a low-power-to-D0
transition, because firmware can wake USB before delivering an association packet.
This property check does not issue USB control/read requests.
An empty radio gets a two-second recovery window every 30 seconds as a fallback
for a missed wake; this is not a guarantee of immediate reconnect on every model.
Active audio/input and pairing keep autosuspend disabled. The default remains
active when no bridge owns the device, to avoid USB sleep without firmware wake
being prepared. This does not implement waking the entire PC from system sleep.

Existing inbox-WinUSB bindings need one adapter-specific configuration step.
Stop installed/development bridges first, then in an administrator PowerShell:

```powershell
.\scripts\Set-AdapterIdlePower.ps1 -Mode Enable
# Unplug/replug the adapter, then launch the development session normally.
```

This requires the matching saved stock binding. It saves original DWORD values
in ignored `work/binding-state.json`, requires remote wake support, and changes
only the selected adapter's hardware key. It does not set `SystemWakeEnabled`,
force suspend without remote wake, or change hubs/global power plans. New WinUSB
bindings do this as part of `Switch-WinUSB.ps1`; stock restoration restores the
original idle settings. Use `-Mode Restore` while bridges are stopped to undo
only the power configuration, then replug. The placeholder unsigned INF is not
used for this experiment.

For diagnosis, `--no-idle-power` disables the feature on a direct radio command;
`"IdlePowerEnabled": false` in private `config/devices.local.json` disables it in
the C++ host. `RadioPower: WakeArmed` in the controller status reports a requested
policy, not an observed D-state or temperature. Journal entries
`idle_power_ready`, `radio_idle_armed`, and `radio_idle_resumed` describe lifecycle;
unsupported wake/configuration and power-policy failures enter the anomaly log.
Validate actual USB suspension and reconnect with each headset/controller model;
synthetic tests cannot establish energy savings or hardware compatibility.

References: [WinUSB power management](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-power-management),
[WinUsb_SetPowerPolicy](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_setpowerpolicy).

Use Windows x64, Visual Studio C++ Build Tools and a Windows SDK. Python 3.11+
is needed for reference/loopback tests, not for the installed runtime.

```powershell
.\scripts\build.cmd
.\scripts\Build-Appliance.cmd
.\scripts\Build-CppBridge.cmd
.\scripts\Test-CppBridge.cmd
python -m unittest discover -s tests -p '*tests.py'
powershell -NoProfile -File tests/device_config_tests.ps1
powershell -NoProfile -File tests/setup_helpers_tests.ps1
powershell -NoProfile -File tests/release_privacy_tests.ps1
python scripts/check-public-content.py
```

The appliance builder writes candidates under `outputs/appliance-candidate` and
does not replace a running copy. See [INSTALLER.md](INSTALLER.md) for installer,
source archive and isolated registration/removal smoke checks. Synthetic/loopback
tests do not access the physical adapter; hardware experiments do.

`Test-CppBridge.cmd` builds the differential probe and compares the same
controlled-time scenarios with Python: successful/failed audio ACK, ACK timeout,
re-import without RF reassociation, ISO expiry/frame/status, and partial PCM
age. Loopback tests also inject a failed TX, close/re-import TCP, verify resumed
audio and inspect the C++ anomaly journal. Recovery policy tests cover initial
attach grace, stale snapshots, missing import metadata and generation changes.
Journal tests cover queue saturation, reserved anomaly capacity, rotation, write
failure and bounded shutdown with a stalled writer. These are synthetic tests;
they are not physical adapter reliability measurements.

## Repository layout

| Directory | Purpose |
| --- | --- |
| `src/` | C++ radio/USBIP/runtime; Python reference; legacy research sources |
| `driver/profiles/` | Sanitized model USB descriptors, without physical serials |
| `scripts/` | Build/setup/removal, checks and developer-only experiments |
| `tests/` | Synthetic identities, protocol, transport and configuration checks |
| `installer/` | Inno Setup source |
| `third_party/` | Retained headers and licenses |
| `config/` | Automatic public template; private generated files are ignored |
| `docs/` | Current English documentation |

Legacy synthetic GIP/ViGEm, control-panel and Python launchers remain as research
references. The historical `driver/native` experiment is not the delivered driver
and must not be installed for normal use. Its signing/trust workflows are unrelated
to the current Setup. Research tools can change hardware state: use the normal
installer unless you are deliberately investigating those paths.

## Publication hygiene

Keep private data under ignored `work/`, `config/*.local.*` or `runtime/` paths.
Never commit firmware, captures, certificates, local bindings or real instance
addresses. Tests must use synthetic identities. Run the public-content check;
GitHub also runs it for pushes and pull requests. Do not randomize public protocol
GUIDs or model IDs: see [PRIVACY.md](PRIVACY.md).

Development machine observations are not a compatibility promise. Document exactly
which checks used synthetic RF, loopback sockets or physical hardware, and do not
present an isolated installer smoke test as a hardware restoration test.
