# Background operation

Setup enables an invisible C++ host at the installing user's Windows sign-in.
It runs with limited user privileges and no stored password. It is not a system
service and is unavailable before sign-in. Setup/removal uses UAC when system
driver work is required; ordinary startup and pairing do not.

The host waits for adapter, power, process and local state notifications. Its
radio child is owned by a Windows job. USBip client commands run briefly to
import/detach each association. No Python, resident PowerShell, control panel,
tray icon or overlay is required. Connecting a headset preserves the user's
Windows audio selection.

The installation waits for a missing adapter/device and retries radio startup
with bounded backoff. Lifecycle recovery and sleep handling exist, but broad
sleep/resume and fresh-PC reliability are not fully verified.

On experimental builds, an explicit `UsbImported=false` snapshot after the
two-second attach grace detaches only that slot's owned port and schedules a
retry. Missing/unreadable snapshots do not detach devices. A failed audio TX or
ACK timeout marks `RecoveryRequired=true`, increments the generation and makes
`Ready=false`, while `RadioAssociated=true` retains a live RF association. The
host can re-import that route without a new RF association; successful import
clears recovery and Windows reinitializes its USB/audio session. True RF removal
clears the association and prevents such an import. Late ACKs from the previous
USB session cannot complete requests in the new one.

Expired media is a local per-ISO loss, preserving the request's frame number;
it does not trigger RF/USB recovery. Fatal physical WinUSB failures still leave
the radio process and reach host retry, with the original error in the C++
anomaly journal. These distinctions do not establish the cause of historical
adapter error 31 or prove hardware recovery without physical testing.

To stop this installation from PowerShell:

```powershell
& "$env:LOCALAPPDATA\Programs\xone-windows-wireless\scripts\Run-MaintainedBridge.ps1" -Stop
```

To start it again:

```powershell
& "$env:LOCALAPPDATA\Programs\xone-windows-wireless\scripts\Set-AutomaticBridge.ps1" -Mode Start
```

Uninstall through Windows Installed apps with the adapter connected. Shared USBip
and private recovery files remain; see [INSTALL.md](INSTALL.md).
