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
  remain nonfatal. The normal background host does not retain radio stderr;
  capture it explicitly during a bounded diagnostic run. For a USB ETW trace,
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
