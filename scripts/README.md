# Script guide

Run commands from the repository root. Build/setup tools are not interchangeable
with research launchers. The normal installation uses the C++ host, physical
WinUSB radio and signed USBip virtual bus; Python is a development reference.

## Normal build and maintenance

| Entry point | Purpose |
| --- | --- |
| `Build-Appliance.cmd` | Build C++ host/radio candidate and run its synthetic checks |
| `Build-Appliance.cmd --no-diagnostics <output-directory>` | Build without runtime diagnostic writers |
| `Build-Installer.ps1` | Build and package the installer; requires Inno Setup |
| `Package-Release.ps1` | Package existing binaries and corresponding public source |
| `Test-Installer.ps1` | Isolated registration/removal smoke test without hardware hooks |
| `Setup-Action.ps1` | Installer maintenance dispatcher; normally invoked by Setup |
| `Set-AutomaticBridge.ps1` | Manage this installation's sign-in host |
| `Run-MaintainedBridge.ps1` | Compatibility start/stop entry point for the native host |
| `Get-ControllerStatus.ps1` | Read aggregate state; native candidates query memory IPC |
| `Restore-Stock.ps1` | Explicit stock-driver recovery |

`Setup-Helpers.ps1`, `Device-Config.ps1`, `Native-Config.ps1` and
`Adapter-IdlePower.ps1` are shared helpers. `Switch-WinUSB.ps1`,
`Set-AdapterIdlePower.ps1`, `Import-Firmware.ps1` and `Install-UsbIp.ps1` perform
adapter/dependency maintenance; follow the linked guides before invoking them
directly. None of these tools belongs in the per-packet audio path.

See [installer build](../docs/INSTALLER.md), [installation](../docs/INSTALL.md)
and [background operation](../docs/BACKGROUND-MODE.md).
Setup invokes these scripts itself. `Setup-Action.ps1 -Action ConfigureStartup`
applies the saved checkbox choice; `Set-AutomaticBridge.ps1 -Mode Enable -NoStart`
registers sign-in startup without launching the host. The independent start-now
step uses `-Mode Start` after successful preparation and when no reboot is pending.

## Development and checks

| Entry point | Purpose |
| --- | --- |
| `Run-Development.ps1` | Controlled Python or C++ session with separate diagnostics |
| `Build-CppBridge.cmd` | Build the standalone native USB/IP radio |
| `Test-CppBridge.cmd` | C++ codec/core, recovery, loopback and Python parity checks |
| `build.cmd` | Build the reference radio, binding helper and legacy probes |
| `check-public-content.py` | Reject private identities, captures and generated binaries |
| `generate-usb-profiles.py` | Generate compiled model profiles |
| `import-upstream.py` | Import the pinned upstream research source |

Python checks use `python -m unittest discover -s tests -p '*tests.py'`.
PowerShell checks live under `tests/`; the CI workflow names its maintenance
subset. Full prerequisites and commands are in [development](../docs/DEVELOPMENT.md).

## Hardware research and alternative paths

These tools are excluded from the installed runtime. Some change bindings,
install dependencies or start a competing radio owner; use them only for a
deliberate experiment with the normal bridge stopped.

| Group | Entry points |
| --- | --- |
| Python USB/IP reference | `Run-UsbGip.ps1`, `Run-UsbGipAuto.ps1` |
| Synthetic GIP and ViGEm | `Run-Bridge.ps1`, `Run-DirectGip.ps1`, `Build-DirectGip.cmd`, `Bind-UsbGipTest.ps1`, `Install-Vigem.ps1` |
| GIP/audio probes | `build-native-probe.cmd`, `Build-NativeAudioProbe.cmd`, `Build-AudioEndpointTest.cmd`, `Test-NativeGip.ps1`, `Test-NativeAudio.ps1` |
| Stock descriptors and traces | `Get-StockUsbReference.ps1`, `Build-UsbReferenceDump.cmd`, `Trace-StockHeadset.ps1`, `Build-EtwTraceDump.cmd`, `Analyze-StockAuth.py`, `Read-PdbPublicSymbols.py` |
| Manual device experiments | `Hardware-Test.ps1`, `New-DeviceConfig.ps1` |
| Incomplete kernel driver | `Build-NativeDriver.ps1`, `Prepare-NativeTest.ps1`; see [driver research](../driver/native/README.md) |

Local outputs, firmware, captures and recovery data remain ignored under `work/`,
`build/`, `outputs/`, `dist/` and private configuration paths. Repository cleanup
does not imply deleting these local artifacts.
