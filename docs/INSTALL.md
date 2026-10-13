# Installation details

This preview is a portable user-mode transport and virtual USB/GIP bridge.
It does not contain a new signed Xbox radio kernel driver. Its executables are
not Authenticode-signed. The adapter uses Windows' signed in-box WinUSB driver;
the virtual USB controller is supplied by the separately installed usbip-win2.

## Dependencies

Use Windows 11 x64, Python 3.11+ and the Microsoft Visual C++ v14 x64 runtime.
Python must be available as `python.exe` in PowerShell; verify `python --version`.
The Python bridge uses the standard library and needs no pip packages.

Install [usbip-win2 0.9.8.1](https://github.com/vadimgrn/usbip-win2/releases/tag/v.0.9.8.1)
from its upstream release, including the client and virtual USB controller.
The tested installation uses `C:\Program Files\USBip\usbip.exe`. Its installer
can restart USB hubs and temporarily interrupt attached USB devices; follow the
upstream instructions. This project does not change Secure Boot or test-signing.

Windows must have the Microsoft `dc1-controller`, `xboxgip` and `usbaudio`
components. USB/IP presents the captured wired device profiles to those drivers.
Install Xbox Accessories separately from Microsoft's Store if you want its UI.

## Obtain firmware locally

The adapter RAM image is Microsoft's firmware, **not part of this distribution**.
The bridge accepts only `FW_ACC_00U.bin` with SHA-256:

```text
48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66
```

With the stock adapter driver installed, try:

```powershell
.\scripts\Import-Firmware.ps1
```

The helper reads the local Windows driver store, verifies the hash and copies
the matching image to `work/firmware/FW_ACC_00U.bin`. It makes no driver changes.
If you already have that file:

```powershell
.\scripts\Import-Firmware.ps1 -Path 'C:\path\to\FW_ACC_00U.bin'
```

The upstream xone project documents a Microsoft Windows Update CAB containing
the expected image in its [firmware acquisition script](https://github.com/medusalix/xone/blob/3484f603484782dd7551c64e5a33fc602b127051/install/firmware.sh).
As an alternative, download that CAB from Microsoft's server and extract it with
Windows `expand.exe`, then import and verify it:

```powershell
New-Item -ItemType Directory -Force work\downloads | Out-Null
$firmwareCabUrl = 'https://download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab'
Invoke-WebRequest -Uri $firmwareCabUrl -OutFile work\downloads\adapter-driver.cab
expand.exe -F:FW_ACC_00U.bin work\downloads\adapter-driver.cab work\downloads
.\scripts\Import-Firmware.ps1 -Path work\downloads\FW_ACC_00U.bin
```

The helper rejects other firmware hashes. The CAB and extracted image remain
local. See the upstream script's Microsoft terms reference. Do not upload them
with bug reports or source contributions.

## Bind exactly one adapter

The public binding helper supports the external adapter `045E:02FE` with its
stock `mt7612US` package. It refuses other hardware or unexpected driver state.
Inspect the selection with `Switch-WinUSB.ps1 -Mode Plan`. `Bind` and `Restore`
require an administrator PowerShell; changing binding requires a USB reconnect.

Before binding, the script exports the original driver, saves its INF/section and
the adapter's filter/interface registry values under `work/`. Only the selected
instance changes. Keep that directory for restoration, and keep it private.
For multiple matching adapters, supply `-InstanceId` explicitly.

Do not run the stock driver, a discovery process and the new radio bridge
against the same physical adapter simultaneously. Do not install the unsigned
future-package INF found in `driver/`.

## Device mapping

The USB/GIP route needs each device's radio MAC and matching captured profile.
See README for association discovery. The local configuration is:

```json
{
  "Version": 1,
  "AdapterInstanceId": "",
  "Devices": [
    { "Name": "controller", "Mac": "020000000003", "Profile": "controller-0b12-usb.json" }
  ]
}
```

Replace the fabricated MAC. An empty adapter ID selects one present `045E:02FE`
adapter. Device names/MACs/profiles must be unique. Allowed profiles are
`controller-0b12-usb.json`, `headset-0b17-usb.json`, `headset-0b32-usb.json`.
Loopback USB and radio broker ports are assigned in pairs starting at 33240.
Only one instance per captured profile is supported in this preview.

Run `Run-UsbGip.ps1 -AudioBridge -NativeController` for the configured devices.
Changing the configuration requires stopping and restarting the bridge.
`-ConfigPath` can select a different private configuration file. Keep configs out
of public repositories and release archives.

## Alternatives

`Run-Bridge.ps1` and the optional USB/GIP `-FallbackOutput native-gip` use the
undocumented XboxGipSynthetic API. They require Developer Mode and administrator
rights and expose a generic synthetic controller rather than the full native
device configuration path. The older `vigem` option requires ViGEmBus and presents
an Xbox 360 device. These alternatives are not the recommended headset route.
