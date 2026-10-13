# Automatic background bridge

Status: current development branch. These changes are not in the frozen public
alpha.2 download. This path uses the existing signed WinUSB, usbip-win2 and Windows
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
