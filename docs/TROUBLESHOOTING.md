# Troubleshooting the C++ candidate

- **A device is missing:** check that the adapter is connected and the background
  host's `work/maintenance-status.json` has no error. Turn on the device normally;
  pair with both pairing buttons only if its saved association does not return.
- **One headset is silent:** choose its playback endpoint explicitly in Windows
  or the application. Connecting a second headset no longer changes defaults.
- **A device disappears:** report the approximate time and model. Local snapshots
  include USB closure counts, protocol errors and recent radio removal reasons.
  Do not publish the entire private `work`/`config` folders.
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
