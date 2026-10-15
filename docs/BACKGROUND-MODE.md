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
