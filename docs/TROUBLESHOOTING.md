# Troubleshooting alpha.4

These steps apply to the automatic C++ installer. The alpha.3 panel/Python workflow
is historical. Start with the smallest relevant action; do not run a second radio
bridge against the same adapter.

## Setup did not complete

Keep one supported external adapter (`045E:02FE`) connected and approve setup UAC.
Ensure Internet access is available for missing dependencies. If Windows requests
a restart, restart and retry Setup as the same user if setup did not finish.

An existing USBip installation must be the tested 0.9.8.1 version with a working
virtual controller. Setup does not replace/downgrade another version automatically.
Restart Windows or repair that dependency if Setup reports it is not ready.
Only a stock `mt7612US` or WinUSB adapter binding is accepted.

## Device keeps searching

For a device not paired with the adapter, start its ordinary pairing mode and
briefly press the adapter pairing button. The host detects supported models itself;
there is no **Add device** screen or MAC address to enter.

For a previously working device, turn it off for about ten seconds and turn it on
normally. If it still does not connect, reconnect the adapter once and try again.
The host remains in the background and responds to adapter/device changes. Do not
repeatedly swap to the stock driver or reset device firmware as a recovery step.
Older-headset reconnect behavior and repeated cycles still need broader validation.

## Device connects, but there is no output

The host runs in the installing user's logged-in session. Complete Setup and sign
in as that user; no application window needs to be opened. One device per supported
model is allowed. Another device of the same model cannot replace an active slot.

Check the physical headset volume, chat/game mix and microphone mute. Windows
defaults are selected automatically, but an application can retain its own pinned
output. Use its **Default** audio setting or choose the headset in that application.
If output remains absent, power-cycle the affected device, then reconnect the
adapter once if necessary.

## Distortion or stutters

Record whether it affects one headset or all devices, whether it follows heavy
system load, and whether reconnecting the adapter changes it. The audio path uses
headset-clock pacing with bounded queues, and its audio thread uses Windows MMCSS.
Normal packet/text logging is disabled. This does not guarantee uninterrupted sound
during stalls in other applications, storage, system drivers or the scheduler.

Do not substitute another headset's USB profile or run the direct synthetic GIP
experiment to fix audio. The alpha.4 installer already uses the C++ USB/GIP path.
Report reproducible symptoms; endpoint presence alone does not prove clean audio.

## Accessories shows Connected instead of battery

Devices are presented to Windows through USB/GIP, so Xbox Accessories can show
**Connected** instead of a wireless battery indicator. This is a known presentation
limit. Use a real USB cable for firmware updates; updating through the bridge and
controller audio-jack streaming are unsupported.

## Optional local status / stop commands

Routine use requires no terminal. For development/support, open PowerShell in the
installation folder and inspect only the small current state:

```powershell
Get-Content .\work\maintenance-status.json -Raw | ConvertFrom-Json |
    Select-Object Backend, Phase, Error, DevicesReady, AudioSelected
```

`WaitingForDevices` is normal when supported devices are off. `WaitingForAdapter`
means no healthy supported WinUSB adapter was found. `DevicesReady` reports an
imported device; it does not by itself verify audible sound. `SetupRequired` or
`Retrying` provides a current error, rather than a text log.

To stop/start the current session:

```powershell
.\scripts\Set-AutomaticBridge.ps1 -Mode Stop
.\scripts\Set-AutomaticBridge.ps1 -Mode Start
```

The same script's `-Mode Disable` stops the host and removes startup; `-Mode Enable`
restores startup and starts it. Do not detach every USB/IP port as a shortcut. The
host owns and cleans up only its imports. If normal uninstall fails, keep recovery
files and reconnect the adapter to the original port before retrying.

## Report an issue

Include application version, model/revision, adapter VID/PID, Windows build,
steps to reproduce, and whether it concerns connection, input, playback or capture.
Current `Phase`/`Error` fields can help. Review even these fields before sharing.

Do not upload entire local config/work folders, firmware, driver backups, memory
dumps, ETL traces, raw USB captures or recordings. Remove device addresses, serials,
PnP/audio endpoint IDs, usernames and local paths from any excerpts. Normal runtime
does not save microphone PCM.
