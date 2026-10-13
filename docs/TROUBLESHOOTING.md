# Troubleshooting

## No device output

Check that the adapter is `045E:02FE`, bound to WinUSB and reconnected after the
binding change. Confirm `python --version`, `usbip.exe`, the exact firmware hash
and `config/devices.local.json`. The radio MAC must correspond to the selected
model/profile. Ensure another radio test or bridge is not running.

`Get-ControllerStatus.ps1 -Json` reports radio state and observed battery levels.
Raw USB peers can appear as Kind=unknown and Authentication=not-started in the
local session snapshot because Windows owns their GIP conversation. Those fields
must not be interpreted as proof of successful RF authentication/encryption.

The `none` fallback deliberately provides no Windows output for unconfigured MACs.
Include the Series profile and `-NativeController` for native controller output.
The synthetic alternative does not supply the same Accessories configuration path.

## Older headset reconnects into pairing/search

This is a known issue on the tested Xbox Wireless Headset 2021. Automatic recovery
after a power cycle is not reliable in this preview. If it is already searching
for pairing, a short press of the adapter pairing button can reconnect it. If it
is not in pairing mode, follow the headset's normal pairing procedure. Do not
reset firmware or repeatedly change USB drivers to diagnose this issue.

## Connected instead of a battery indicator

The current Windows presentation is USB/GIP. Xbox Accessories has shown Connected
for both headsets and the controller, even when XInput and WGI provide controller
battery information. Bridge diagnostics expose the physical GIP power type and
four coarse levels. WGI's numeric capacity representation is not a measured exact
percentage. Fixing the Accessories label is unresolved.

## Silence or audio distortion

Use `-AudioBridge`, select the correct headset output in Windows/application
settings and verify the physical headset volume/mix controls. Separate profiles
have different command/audio endpoint addresses; do not substitute one model's
profile for another. The virtual High-Speed presentation and headset-clock pacing
are required for the observed clean test tones.

Inspect queue sizes and RX/TX deltas in private `work/usb-gip-*-radio-status.json`.
Do not treat an endpoint's existence or a successful render call as proof that
sound is clean at the headset. This release's validation is limited to stated
hardware tests, not long-term playback certification.

## After USB unplug, session timeout or a failed launch

Stop the bridge, inspect its local logs, reconnect the adapter if needed and
restart. Power-cycle the affected wireless devices if the previous GIP session
persists. Launcher sessions expire after the requested duration, at most one hour.
Do not run multiple instances or detach every USB/IP port as a recovery shortcut.

## Read-only battery check

```powershell
.\outputs\native-gip-probe.exe --battery
```

This mode queries WGI RawGameController battery reports. It does not create a
synthetic device, change drivers or start motor tests. Headsets may not be listed
as WGI raw controllers. `--live-rumble` is a separate, interactive motor test.

## Reporting issues without personal data

Report model, adapter VID/PID, Windows build, firmware version, chosen backend and
steps to reproduce. Remove MACs, adapter serials, PnP instance IDs, audio endpoint
IDs, usernames, absolute paths and unrelated device information from logs.
Never upload local configs, driver backups, firmware, ETL files, memory dumps,
raw USB captures or voice recordings without reviewing their contents.
The normal bridge keeps metadata/status locally and does not save microphone PCM.
