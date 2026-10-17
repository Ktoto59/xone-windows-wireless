# Background operation

Setup offers independent sign-in startup and start-now options, both enabled by
default on a fresh installation. Upgrades remember the selected options. Sign-in
startup runs an invisible C++ host at the installing user's Windows sign-in.
It runs with limited user privileges and no stored password. It is not a system
service and is unavailable before sign-in. Setup/removal uses UAC when system
driver work is required; ordinary startup and pairing do not.

Start menu shortcuts can start or stop the installed bridge without opening a
console or running scripts manually. If both startup options are disabled, no
host is launched by Setup. A Windows-requested reboot skips start-now; after
restart, use the Start shortcut if sign-in startup was not selected. Manual start
does not add a sign-in
trigger when autostart is off.

The host waits for adapter, power, process and local state notifications. Its
radio child is owned by a Windows job. USBip client commands run briefly to
import/detach each association. No Python, resident PowerShell, control panel,
tray icon or overlay is required. Connecting a headset preserves the user's
Windows audio selection.

The installation waits for a missing adapter/device and retries radio startup
with bounded backoff. Lifecycle recovery and sleep handling exist, but broad
sleep/resume and fresh-PC reliability are not fully verified.

Experimental radio retries share one host-lifetime backoff across child exits,
USB disappear/reappear notifications and startup errors: 4, 8, 16, 30, then
60 seconds (capped). Re-enumeration and system resume cannot erase an existing
retry deadline. Only 60 seconds of uninterrupted child runtime resets the
failure count. `RetryCount`, `RetryDelaySeconds` and `radio_retry_scheduled`
record recovery attempts; this is not a temperature measurement or a fix for
the initiating USB disconnect. The Python development supervisor exits on a
fatal radio failure rather than entering this automatic restart loop.

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
