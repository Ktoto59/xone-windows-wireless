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

Installed host/radio/slot state uses bounded shared memory and Windows events,
including shutdown. No periodic state files or stop-file polling are used by
the C++ host. Only explicit lifecycle/generation transitions detach a USB import;
an unavailable observation is not a disconnect. Startup configuration and the
firmware are read before streaming. New pairing metadata is cached until shutdown.
See [RUNTIME-IO.md](RUNTIME-IO.md) for execution and cancellation boundaries.

Radio ingress and each native USB worker isolate reliable control/lifecycle/ACK
messages from microphone PCM. Control messages keep FIFO order and a separate
4096-entry capacity. Microphone queues retain at most four transfers per WCID
and expire at 32 ms; a microphone flood cannot evict a service message. Consumers
process bounded batches, with control messages before media and no media passing
a still-queued lifecycle barrier. Disconnect/reassociation invalidates old media.
Mixed or undecodable GIP messages remain reliable rather than being guessed to
be droppable audio.

Playback has a separate path and model-specific media deadline. Before serialized
physical USB writes, controls precede unsent playback while FIFO/peer fairness
is retained within each class. The render deadline follows each native request
to the physical TX stage: unsent stale PCM gets an explicit ISO expiry, preserving
the RF association and USB generation. Real transport failures retain recovery.
The legacy TCP radio broker likewise prioritizes reliable frames but always
finishes a partially sent frame before switching traffic classes.

Native outgoing GIP uses a dedicated bounded executor with one overlapped WinUSB
OUT request in flight. The radio owner handles reception/lifecycle while the
executor waits for USB completion. USB success is not reported at enqueue time.
URB unlink, USB detach/reimport and RF reassociation invalidate stale work; buffers
and OVERLAPPED remain owned until completion/cancellation has been drained.

These are application queues, not additional physical USB endpoints or parallel
writes. Reliable means no silent media-style eviction or expiry: capacity and
transport failures are explicit errors, not a promise of delivery through a
failed adapter. Commands are not blindly replayed; protocol/Windows sequencing
still owns acknowledgements and retries. Queue-pressure drops are logged with
counts at most once per second, not once per microphone packet.

Malformed radio GIP framing is rejected before dispatching any packet from that
transfer. The associated WCID's `MalformedGipPackets` status counter increases;
other devices and later valid packets continue. Dispatch and physical USB errors
still reach the transport recovery path.

The source retains legacy synthetic GIP/ViGEm and Python reference experiments;
these are not installed or selected by the normal runtime. They do not establish
stock wireless auth v3 support, firmware-update support or kernel-driver status.
See [LIMITATIONS.md](LIMITATIONS.md) and [DEVELOPMENT.md](DEVELOPMENT.md).
