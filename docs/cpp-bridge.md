# Experimental C++ bridge

**Alpha.4 uses this C++ data path as the default**, managed by an invisible C++
background host. End users should follow [INSTALL.md](INSTALL.md); the installer
does not include the old backend-selection panel or a Python runtime.

The manual commands and panel workflow below record the development transition
from alpha.3. They are not the alpha.4 setup instructions. For the current source
build/package order, see [INSTALLER.md](INSTALLER.md).

This development backend moves USB/IP, GIP forwarding and headset PCM conversion
into the radio executable. It replaces the three Python data servers and their
intermediate radio TCP connections. Windows still uses the signed USBip bus and
its existing Xbox GIP/audio drivers, including Xbox Accessories support. This is
a user-mode bridge, not a new kernel driver.

The published alpha.3 installer still contains the previous backend. The C++
implementation has passed descriptor equivalence, real loopback USB/IP transport,
audio clock/fragment, cancellation and generation tests. The 2024 headset was
tested through the physical adapter: audio started after targeted session
recovery, and the user confirmed clean playback initially. A later stutter was
reported while using a heavy IDE project; its cause was not established. A
60-second measurement used about 11 MB working set and 3.15% of one logical CPU
(about 0.20% total on that 16-logical-CPU PC). This is not a matched comparison or
a general performance guarantee. Controller/2021 headset operation on the C++
backend, repeated power cycles and other PCs remain unverified.

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

Complete normal adapter/USBip setup and configure the supported devices. In the
development control panel, open **Start / stop**, choose **C++ bridge (preview)**,
then **Apply and restart**. Choose **Python reference bridge** and apply to return
to the reference implementation. The selection is saved privately and the
supervisor restarts without another UAC prompt or driver change.

The equivalent manual configuration is a top-level field in the private
`config/devices.local.json`, changed while the bridge is stopped:

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
An established USB import stays connected while idle; only incomplete handshakes
have a five-second timeout. Pending interrupt reads are allowed to wait for input.

These changes reduce avoidable work in time-critical threads. They do not prove
that system freezes caused by memory pressure, DPCs or another application are
resolved. Existing alpha.3 limitations, including wired battery presentation,
unsupported firmware updating, and incomplete RF authentication parity, remain.
