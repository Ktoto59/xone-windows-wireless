# Troubleshooting the C++ candidate

- **Setup says `pnputil.exe` is not recognized:** use alpha.5.2 or newer.
  It fixes WOW64 redirection and runs maintenance in native x64 PowerShell; no
  manual PATH edits or extra downloads are needed.
- **Setup fails:** alpha.5.1 displays the failing stage and reason. Local
  `work/setup-AutomaticSetup.json`, `.json.error.txt` and `.json.log` preserve
  details. Share only the relevant error after removing local paths and instance
  IDs. For alpha.5 first-install download failures, use alpha.5.1 or newer; it
  corrects the Microsoft firmware HTTPS host without bypassing certificate checks.
- **A device is missing:** check that the adapter is connected and the background
  host's `work/maintenance-status.json` has no error. Turn on the device normally;
  pair with both pairing buttons only if its saved association does not return.
- **One headset is silent:** choose its playback endpoint explicitly in Windows
  or the application. Connecting a second headset no longer changes defaults.
- **A device disappears:** report the approximate time and model. Local snapshots
  include USB closure counts, protocol errors and recent radio removal reasons.
  Do not publish the entire private `work`/`config` folders.
- **Radio stderr reports `Bulk read: Windows error 31`:** this is a physical
  WinUSB pipe read failure, not a GIP parser result. It does not identify whether
  the device, endpoint, USB controller or power transition caused the failure.
  Diagnostic radio stderr includes the endpoint (`0x84` WLAN or `0x85` MCU),
  requested buffer size and UTC failure time. Ordinary read timeouts (`121`)
  remain nonfatal. The experimental C++ runtime also records these errors in
  `work/diagnostics/cpp/anomalies-radio-<pid>.jsonl`, including the numeric
  Windows error and endpoint. Older installed builds require an explicit stderr
  capture. For a USB ETW trace,
  use `HeadersBusTrace` rather than data-payload keywords and correlate the
  failure with the controller/endpoint and power events. See
  [WinUSB read errors](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_readpipe)
  and [USB ETW capture filters](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/how-to-capture-a-usb-event-trace).
- **Setup reports another radio session:** stop the bridge from the folder that
  owns it before retrying. Do not run two hosts against the same adapter.
- **Setup requests reboot or adapter reconnect:** complete that step before
  pairing. First-time signed USBip installation can restart USB hubs.
- **Uninstall stops:** reconnect the adapter to its original port and retry.
  Keep `work/binding-state.json` and the driver backup until restoration succeeds.
  Shared USBip and unowned WinUSB bindings are deliberately retained.

The adapter belongs to WinUSB while this bridge runs. Old devices cannot bypass
it through the stock wireless driver on the same physical adapter. USBip remains
in the virtual USB transport; replacing Python with C++ does not remove USBip.

Experimental C++ builds retain bounded JSONL lifecycle/error journals under
`work/diagnostics/cpp`. `anomalies-*.jsonl` contains errors, audio loss, stalled
writes and recovery requests; `cpp-*.jsonl` also contains ordinary lifecycle
events. Records share process-local sequence numbers, UTC/monotonic time, process,
thread and session. No PCM/authentication buffers are logged. Set
`XONE_CPP_TRACE=1` before startup for USB/URB metadata and transfer timings;
`XONE_CPP_LOG_DIR` selects a local capture directory. Normal read timeout 121 is
only traced at this verbose level, not classified as an error.

Each journal rotates at 8 MiB with three backups. Ordinary and anomaly queues
each hold at most 2048 records. Saturation/I/O failure is reported through
`trace_loss` and `<role>-<pid>-health.json`. Disk writes run on one worker per
process; health snapshots are limited to once per second plus final shutdown.
Shutdown waits at most one second for that worker. A blocked writer
can lose its final records/health update. Keep captures local and inspect these
counters before treating an absent event as proof it never happened.
