# alpha.5.4 test candidate

This candidate is built from `experimental` for local hardware validation before
promotion to `main` or publication. It is not yet a validated release.

- Separate reliable control traffic from expiring playback/microphone media.
- Restore failed USB routes with bounded per-device recovery and repeated-import
  identity initialization.
- Add wake-capable adapter idle policy and bounded radio startup retries.
- Build the installed C++ runtime without diagnostic journal writers, anomaly
  files or packet tracing. Keep development diagnostics available separately.
- Preserve operational state/recovery files and installation diagnostics.

Software tests cannot establish headset stability. Validate actual audio,
microphone, connect/disconnect, idle wake and cold adapter reconnect before
publication. Physical USB removal/error 31 and audio recovery after re-import
remain investigation items until the corresponding hardware checks pass.
