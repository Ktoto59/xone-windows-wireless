# C++ bridge: alpha.5 candidate

The runtime consists of `xone-background.exe`, its owned `xone-probe-cpp.exe`
radio process and the signed upstream USBip client/bus. Python and PowerShell
are used for development or setup only; neither remains in the audio/input path.
The physical adapter uses Microsoft's WinUSB driver. Windows sees virtual USB
GIP devices through USBip and loads its existing Xbox/audio drivers.

## Per-device isolation

Eight automatic slots are available. A radio announcement chooses an immutable
connector before USB export: 2021 headset (RF 0B31 / USB 0B17), 2024 headset
(0B32), or Series controller (RF 0B13 / USB 0B12). Unknown identities are rejected.
Every association owns its USB server/thread, control state, PCM buffers,
completion tokens and clocks. USB serials include the physical address, allowing
separate instances of one supported model. Four concurrent synthetic imports,
including two 2024 instances, are covered by the loopback test; multiple physical
units of the same model have not been tested.

The 2021 connector uses an independent eight-millisecond render clock and a
96 ms render age limit. Its exact observed USB-only mode-6 initialization is
consumed to preserve wireless pairing. The 2024 connector uses fresh radio
credits and a 64 ms render age limit. Capture is bounded to 32 ms; old-headset
silence concealment stops after 128 ms without RF. Expired audio is discarded,
not replayed as an indefinitely growing backlog. Round-robin radio submission
keeps devices separate at the shared physical transmitter.

The host respects the user's Windows audio selection. It does not select the
last connected headset. Losing access to a status file does not remove a device;
only an explicit disconnect/generation transition permits detaching that slot.
Status writes run on bounded asynchronous workers, outside PCM/radio processing.
Normal radio counters are considered every two seconds (100 ms with `--verbose`).
Repeated battery, audio-control and ignored GIP messages retain that cadence;
identification, changed battery/authentication state and audio negotiation
transitions request an immediate update.
Lifecycle changes, the first observed runtime-file error and final shutdown bypass
that interval. Per-device USB import, closure and error transitions also publish
immediately through the asynchronous writer.
No audio samples or packet capture files are recorded by normal operation.

## Validation and limits

On the development PC, both physical headset revisions and a Series controller
connected to the C++ bridge. The user confirmed clean sound in both headsets.
A subsequent regression where connecting one headset removed the other was
observed. The host incorrectly treated unreadable state files as disconnects;
this was fixed and automatic global audio selection was removed. The user then
confirmed that both devices remained present with clean sound. A six-second
exclusive lock on a live slot status file preserved its USBip port and generation.
These observations do not prove the cause of every historical RF dropout.

Software checks cover descriptor equivalence with the Python reference, actual
loopback USBip framing/imports, audio, cancellation, bounded buffers, real TX
completion, model-specific clocks, slot reuse and isolation. Long-duration use,
fresh-PC installation, voice quality and broad sleep/resume coverage remain to
be verified. USBip is still part of the transport; this port does not replace its
kernel bus or establish a numeric latency improvement over the Python reference.

## Build and test

Use Visual Studio C++ Build Tools and a Windows SDK on Windows x64:

```powershell
.\scripts\Build-Appliance.cmd
.\scripts\Build-CppBridge.cmd
.\scripts\Test-CppBridge.cmd
```

The appliance binaries go to `outputs/appliance-candidate`. Development tests
use Python, synthetic RF and real loopback TCP; they do not switch physical
adapter drivers. See [INSTALLER.md](INSTALLER.md) for packaging and
[INSTALL.md](INSTALL.md) for installation/removal.
