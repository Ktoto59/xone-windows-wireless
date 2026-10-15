# Architecture

```text
Physical Xbox Wireless Adapter (system WinUSB)
  C++ radio executable: MT76, radio clients, automatic route factory
    per-association C++ USBIP server and model-specific GIP/PCM connector
      signed upstream USBip virtual USB bus
        existing Windows Xbox GIP/audio drivers
          Windows audio, XInput/WGI and compatible Xbox Accessories operations
```

An invisible C++ host starts at user sign-in, owns the radio process through a
Windows job and imports/detaches only its own USBip ports. PowerShell is used for
setup/removal, not the audio path. Firmware is verified and loaded into adapter
RAM. This is a user-mode bridge using existing kernel drivers.

Announcements select supported models dynamically. Every route owns queues,
control state, tokens, clocks and a generation. Addresses learned locally produce
stable, distinct virtual USB serials; no device instance is baked into a build.
A model's USB descriptors and Windows/GIP interface GUIDs are technical constants.

The 2021/2024 connectors use different audio pacing and render age limits. Radio
blocks are adapted to Windows USB ISO fragments without lossy encoding. See
[USBIP-LATENCY.md](USBIP-LATENCY.md) and [cpp-bridge.md](cpp-bridge.md).

Status disk writes are asynchronous. The host only detaches on an explicit
lifecycle snapshot/generation transition, never on a missing or unreadable file.
Rare radio removals and USB closure errors are recorded locally for diagnosis.

The source retains legacy synthetic GIP/ViGEm and Python reference experiments;
these are not installed or selected by the normal runtime. They do not establish
stock wireless auth v3 support, firmware-update support or kernel-driver status.
See [LIMITATIONS.md](LIMITATIONS.md) and [DEVELOPMENT.md](DEVELOPMENT.md).
