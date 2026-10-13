# Automatic background bridge

## Current alpha.4 application

The installer now enables `xone-background.exe`, an invisible C++ user-session
host, automatically. Its only resident child is the C++ radio/USB-GIP executable;
USBip attach/detach commands run briefly on connection changes. PowerShell is used
for setup/removal only. No Python runtime, panel, tray or overlay is installed.

Supported models are identified from their radio announcements and saved in local
configuration. First-time pairing still uses the ordinary device/adapter buttons.
The host imports each model independently, restores its virtual USB generation
after reconnect and chooses the latest connected headset as the Windows default
playback/capture device. A later manual default change is respected on restoration.

The host waits for device, power, process and state-change notifications. Normal
operation writes no text logs; small ownership/error snapshots change on lifecycle
transitions. Retry delays are bounded. Startup runs with limited privileges at
Windows sign-in, rather than as a service before sign-in.

The new 2024 headset was confirmed to play clean ordinary audio in a short check
with this host. Long-term stability, fresh installation on another PC and broad
sleep/resume behavior are still pending. See [INSTALLER.md](INSTALLER.md).

## Historical alpha.3 supervisor

The remainder of this document records the earlier implementation and its hardware
observations. Its panel/Python workflow is not the alpha.4 installation flow.

Status: included in alpha.3. These changes are not in the frozen alpha.2 download.
This path uses the existing signed WinUSB, usbip-win2 and Windows
GIP/audio drivers. No new kernel driver, driver certification, test signing or
Secure Boot change is required by this implementation.

The custom NDIS kernel experiment is not the selected delivery path. It remains
source research only. Trust added for the local kernel experiment was removed;
the trial never changed Secure Boot or TESTSIGNING and never loaded that driver.

## Normal use

Complete adapter/dependency/device setup once. In the control panel, choose
**Enable automatic mode**. It starts the background bridge now and at your next
Windows sign-in. The installer also offers this option by default. If setup is
incomplete, the background host waits/reports the missing setup; it does not
change driver bindings or request administrator rights on its own.

No panel needs to stay open. The host runs as the current user through Task
Scheduler with interactive, limited privileges and no stored password. It is a
user-session background task, not a system service available before sign-in.
The radio sessions are unlimited rather than expiring after 30 minutes.

Audio servers and the radio process explicitly use AboveNormal process priority
instead of inheriting BelowNormal from the scheduled host. The audio event-loop
thread joins the Windows MMCSS Pro Audio task; failure to join is logged and does
not stop devices. This does not modify global scheduler settings. Status snapshots
are written by a separate disk worker at most five times per second, so filesystem
contention does not directly block the audio event loop. These measures reduce
avoidable scheduling delays; they do not guarantee uninterrupted playback under
every system load or driver interrupt stall.

On 2026-10-13, live priority adjustment reduced user-reported stutters. After
installing the full scheduling/status-IO change, the radio associated but never
started GIP exchange, even after headset power cycling. A physical USB replug
restored the session; the user confirmed playback and reported stutters appeared
to disappear at the same load. This is a limited subjective result, not a claim
of reliable warm recovery or sustained performance under every workload.

The subsequent targeted recovery change restored one warm bridge restart without
USB replug: the peer was associated but silent, the first five-second recovery
cleared/reassociated its firmware slot and refreshed its USB generation, then GIP
and audio resumed. The user confirmed clean playback. Silent initial associations
have at most three retries, and an established but idle peer is not reset by this
timer. A later association from a configured peer refreshes its generation after
one-second debounce; siblings retain their WCIDs and connections. Repeated warm
restarts, old-headset power cycles and sleep/resume are still not verified.

Configured-peer discovery now retains radio discovery beacons/RX after the
manual enrollment window closes. The default USB-only route rejects unconfigured
association and pairing requests outside that window. With no connected peer,
channel scanning continues; any active peer stops scanning. After a pair response,
the current channel is held for ten seconds to finish enrollment. Shutdown
explicitly disables discovery. The old headset then connected on ordinary power-on
without an adapter button, but audio initially had small stutters on channel 11.
One subsequent warm restart restored it on channel 165 with user-confirmed clean
ordinary playback. This does not establish repeated-cycle reliability or universal
audio quality on every channel.

**Stop bridge** stops this session; enabled automatic mode returns at next
sign-in. **Disable automatic mode** stops it and removes this installation's
sign-in task. Device discovery/configuration, driver changes, upgrades and
uninstall stop the supervisor before stopping/changing the bridge. Another
installation's task or unrelated USB/IP device is not removed.

## Recovery and status

- A missing adapter is a waiting state. On return, the configured WinUSB adapter
  is opened again without automatically switching to another driver.
- A worker exit is retried with 2–30 second backoff. Thirty seconds of stable
  worker/radio operation resets the retry count.
- A long polling gap triggers a clean bridge restart as a resume-recovery path.
  Actual sleep/resume still needs hardware validation.
- A per-user mutex prevents simultaneous supervisors. Existing unrelated radio
  processes or an orphaned radio owner are reported instead of forcefully killed.
- Private status separates starting, waiting for adapter/devices, imported devices,
  retrying and cleanup errors. A running process is not called a working headset.
- Process ownership uses PID plus start time. Cleanup retains the existing
  per-worker USB/IP ownership rules.

Files: `scripts/Run-MaintainedBridge.ps1`, `scripts/Set-AutomaticBridge.ps1`.
Private state is under `work/maintenance-status.json`; addresses, local mappings,
radio logs and recovery state stay outside the published source/package.

## Observed result, 2026-10-13

Limited-privilege task registration/start succeeded without UAC. After requesting
the worker's normal stop, the supervisor created a different worker and radio
process automatically. After a physical adapter unplug/replug, it also restarted
the radio; a headset that had had its connections reset sent a fresh pairing
request, associated and obtained its Windows USB/GIP output. The user confirmed
clean ordinary computer audio through the newer headset without USB-C.

These observations cover that development machine and session. They do not prove
long-duration audio, Windows reboot, sleep/resume, old-headset power cycles or all
simultaneous-use scenarios. One physical USB reset was needed after the earlier
radio experiments; warm-start behavior remains a separate reliability concern.
Accessories' wired battery label, auth v3/RF security limitations and unavailable
firmware-update forwarding are unchanged. Automatic provisioning of unknown
models/addresses is not implemented; existing local device setup is still needed.
