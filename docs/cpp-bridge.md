# Experimental C++ bridge

This development backend moves USB/IP, GIP forwarding and headset PCM conversion
into the radio executable. It replaces the three Python data servers and their
intermediate radio TCP connections. Windows still uses the signed USBip bus and
its existing Xbox GIP/audio drivers, including Xbox Accessories support. This is
a user-mode bridge, not a new kernel driver.

The published alpha.3 installer still contains the previous backend. The C++
implementation has passed descriptor equivalence, real loopback USB/IP transport,
audio clock/fragment, cancellation and generation tests. **Physical-adapter audio,
reconnection and performance of this backend have not yet been verified.** It is
not the default and no performance improvement is claimed yet.

## Build

Use an x64 Windows machine with Visual Studio 2022 C++ Build Tools and a Windows
SDK. Run from the source checkout:

```powershell
.\scripts\Build-CppBridge.cmd
```

This produces `outputs/xone-probe-cpp.exe` without overwriting the running
`outputs/xone-probe.exe`. End users do not need Python to run this backend; the
descriptor catalog is compiled into the executable. Python is needed only for
the development equivalence checks or to regenerate that catalog.

`scripts\Test-CppBridge.cmd` runs the development checks using synthetic RF and
loopback TCP only. It does not open the physical adapter or change drivers.

## Select the backend

Complete the normal adapter/USBip setup and configure the existing supported
profiles in the private `config/devices.local.json`. Stop the running automatic
bridge from the control panel first, then add a top-level field to that JSON:

```json
"BridgeBackend": "cpp"
```

Start automatic mode again. Selection persists across supervisor recovery and
sign-in. Alternatively, a manually managed diagnostic run can use:

```powershell
.\scripts\Run-UsbGip.ps1 -CppBridge -AudioBridge -NativeController -FallbackOutput none -Seconds 0
```

Stop the bridge before switching back. Remove `BridgeBackend` or set it to
`python` to restore the reference backend. Neither selection changes adapter
binding, driver trust, Secure Boot or Windows boot settings.

The C++ backend supports the same three captured profiles: Series controller
0B12, headset 2021 0B17, headset 2024 0B32, one device per profile. Listeners bind
only to 127.0.0.1 on the configured USB ports. RF packets are delivered in-process.

## Timing and diagnostics

Each headset has an event-driven USB/audio thread registered with MMCSS. Capture
keeps at most 64 one-millisecond fragments; playback follows physical headset
credits, capped at two eight-millisecond blocks. Complete render blocks and
interrupt writes wait for the radio transport's transmit acknowledgement; partial
render fragments are accepted into the bounded accumulator. Pending URBs, mailboxes
and socket buffers are bounded. Reassociation cancels old work, and successful
USB/IP UNLINK replaces the submit completion.

The current Python reference also has reduced diagnostics: audio packet logging
is off by default (`--verbose` enables bounded development logs), logging runs on
an ordinary worker, and statistics are written every two seconds. Link/generation
changes remain promptly visible. Radio status writes use a separate worker with
one coalesced pending snapshot. No PCM or voice recordings are written.
The C++ candidate also checks the stop marker on a separate 100-ms worker instead
of making a filesystem call on every one-millisecond radio tick.

These changes reduce avoidable work in time-critical threads. They do not prove
that system freezes caused by memory pressure, DPCs or another application are
resolved. Existing alpha.3 limitations, including wired battery presentation,
unsupported firmware updating, and incomplete RF authentication parity, remain.
