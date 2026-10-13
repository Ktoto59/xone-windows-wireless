# xone-windows-wireless

Experimental Windows x64 Xbox Wireless Adapter transport based on
[medusalix/xone](https://github.com/medusalix/xone). It connects Xbox controllers
and two Xbox Wireless Headset revisions over the adapter's radio and presents
them to the existing Windows Xbox GIP and audio drivers.

**This release is a working research preview, not a standalone kernel driver
or a one-click driver installer.** The current implementation uses WinUSB,
a user-mode radio bridge, and virtual USB/GIP devices through usbip-win2.
Windows Xbox Accessories can discover the tested devices and open their settings.

## Tested hardware and behavior

| Hardware | Tested behavior |
| --- | --- |
| External Xbox Wireless Adapter, `045E:02FE` | Firmware loading, radio, pairing, multiple clients |
| Xbox Series controller, USB reference `045E:0B12` | Buttons, sticks and triggers through XInput; Xbox Accessories detection and settings |
| Xbox Wireless Headset 2021, `045E:0B17` | Clean short stereo test tones through the native USB/GIP audio path |
| Xbox Wireless Headset 2024, `045E:0B32` | Clean short stereo test tones, microphone signal and reconnection |

Both headset revisions and a Series controller have been connected concurrently.
Tests were performed on one Windows 11 x64 development machine. Compatibility
with other adapters, Windows builds and firmware versions is not established.

## Known limitations

- **The 2021 headset can enter pairing/search mode after a power cycle.** In the
  tested setup it requires another adapter pairing-button press. Reliable
  automatic reconnection of this model is unresolved. The 2024 headset reconnects.
- Xbox Accessories shows **Connected** rather than a wireless battery indicator,
  because these are presented as USB/GIP devices. Physical battery levels are
  available in bridge diagnostics; XInput/WGI also receive the controller battery.
- Wireless authentication v3 is not implemented. The USB/GIP path uses the Windows
  wired binding. RF authentication/encryption is not established for raw GIP peers;
  this preview must not be described as reproducing the stock wireless security path.
- Settings opening is verified; settings writes, firmware updates, prolonged music,
  voice quality, sleep/resume and USB hot-unplug recovery are not fully validated.
- Firmware-update/bulk interfaces and controller audio-jack streaming are not
  implemented. Do not start firmware updates through this bridge.
- One device per captured profile is supported by the public configuration.
  Unconfigured clients have no Windows output with the default `none` fallback.
- There is no background service or login autostart. A launcher session lasts
  30 minutes by default, or at most one hour with `-Seconds 3600`.

## Requirements

- Windows 11 x64, Windows PowerShell 5.1 and Python **3.11 or newer** on PATH.
- Microsoft Visual C++ v14 x64 runtime for the prebuilt executables:
  [official download instructions](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
- The existing Microsoft Xbox GIP/audio drivers and Xbox Accessories application.
- Signed [usbip-win2 0.9.8.1](https://github.com/vadimgrn/usbip-win2/releases/tag/v.0.9.8.1),
  including its client and virtual USB controller.
- The exact Microsoft adapter RAM firmware described in [INSTALL.md](docs/INSTALL.md).

Firmware, Microsoft system drivers, third-party installers, local device MACs,
driver backups and diagnostic captures are **not included**.

## Installation and first run

Download the Windows x64 preview ZIP from [Releases](https://github.com/Ktoto59/xone-windows-wireless/releases)
and extract it to a writable folder. The ZIP includes the executable tools and
their corresponding source code. Building from source is described below.

1. Install the requirements above. Installing usbip-win2 may restart USB hubs;
   follow its installer instructions and restart Windows if requested.
2. Import the adapter firmware while the stock driver is still installed:

   ```powershell
   .\scripts\Import-Firmware.ps1
   ```

3. Inspect the adapter selection, then run the binding command from an
   **administrator PowerShell**. It exports the stock driver and saves recovery
   information under the private `work/` directory:

   ```powershell
   .\scripts\Switch-WinUSB.ps1 -Mode Plan
   .\scripts\Switch-WinUSB.ps1 -Mode Bind
   ```

   Unplug and reconnect the adapter in the same USB port after binding. This
   selects Windows' signed in-box WinUSB driver for the adapter; do not install
   the future-package `driver/xone-winusb.inf`, which has no signed catalog.

4. Discover your devices' **radio MAC addresses**. With other devices off, run
   this command and power on or pair one device at a time:

   ```powershell
   .\outputs\xone-probe.exe controller-test --firmware work\firmware\FW_ACC_00U.bin --seconds 90
   ```

   Record the `MAC=` value on the `ASSOCIATED` line and which model it belongs to.
   Discovery may finish with exit code 3 if authentication/input is unavailable;
   that does not invalidate a successfully observed association address.
   Stop this discovery process before launching the USB/GIP bridge.

5. Copy `config/devices.example.json` to **`config/devices.local.json`**, replace
   the example MAC addresses with yours, and remove entries for devices you do
   not own. The `020000...` addresses are fabricated examples, not usable defaults.
   Leave `AdapterInstanceId` empty for automatic selection of one `045E:02FE`
   adapter. For multiple adapters, explicitly set its PnP instance ID.
   Keep the `.local.json` file private; Git ignores it.

6. Start the native USB/GIP output from an ordinary PowerShell:

   ```powershell
   .\scripts\Run-UsbGip.ps1 -AudioBridge -NativeController -Seconds 3600
   .\scripts\Get-ControllerStatus.ps1
   ```

   Power on your devices. Previously paired devices can connect automatically;
   use their pairing buttons when necessary. The launcher imports each virtual
   USB device when its radio peer appears. Select the created Xbox audio endpoint
   in Windows/app settings. Open Xbox Accessories to inspect device settings.

7. Stop the bridge before changing routes, running discovery again, or restoring
   the stock driver:

   ```powershell
   .\scripts\Run-UsbGip.ps1 -Stop
   ```

This default USB/GIP route requires no Developer Mode or UAC after dependency
installation and adapter binding. The optional `native-gip` synthetic fallback
requires both Developer Mode and administrator privileges; it is a different
backend and does not provide the same Accessories support.

## Restore the stock adapter driver

Stop the bridge. In an administrator PowerShell run:

```powershell
.\scripts\Switch-WinUSB.ps1 -Mode Restore
```

Then unplug/reconnect the adapter. Preserve `work/binding-state.json` and
`work/driver-backup/`: recovery is specific to your device and installed driver.
If that state is unavailable, use Device Manager to select the original Xbox
Wireless Adapter driver. Do not delete unrelated USB or Xbox driver packages.

## Build and test

Install Visual Studio C++ Build Tools (MSVC x64 and Windows SDK), then:

```powershell
cmd /c scripts\build.cmd
cmd /c scripts\Build-UsbReferenceDump.cmd
cmd /c scripts\Build-EtwTraceDump.cmd
python -m unittest discover -s tests -p "*_tests.py"
powershell -NoProfile -ExecutionPolicy Bypass -File tests\device_config_tests.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\launcher_snapshot_tests.ps1
```

The main build runs 14 protocol and 31 session tests. There are 64 Python tests
for USB profiles, radio adaptation, USB/IP transport and URBs. PowerShell checks
cover configuration validation and snapshot locking/recovery. These are software
checks, not evidence that every hardware/Windows combination works.

## Design, privacy and contributing

- [Architecture and protocol boundaries](docs/ARCHITECTURE.md)
- [Installation details and firmware](docs/INSTALL.md)
- [Troubleshooting and safe diagnostics](docs/TROUBLESHOOTING.md)
- [Upstream sources and licenses](docs/UPSTREAM.md)
- [Preview release notes](CHANGELOG.md)

All IPC listeners use loopback. Runtime logs may contain MAC addresses, PnP IDs,
filesystem paths and device settings. Do not upload `work/` or your local config.
Redact diagnostic output before opening an issue. No microphone PCM is saved by
the normal bridge; test/capture utilities should be reviewed before use.

Bug reports should identify Windows build, adapter VID/PID, device model/firmware,
backend and a minimal reproduction. Keep hardware identifiers and recordings out
of public reports. Firmware-update forwarding, additional profiles and a genuine
wireless GIP/NDIS output are future work.

## License and attribution

GPL-2.0-or-later; see [LICENSE](LICENSE). The port retains xone/mt76 notices.
ViGEmClient headers retain their MIT license under `third_party/vigem`.
Windows DLLs/drivers are used from the operating system and are not redistributed.
This is an independent project, not affiliated with Microsoft or the xone authors.
