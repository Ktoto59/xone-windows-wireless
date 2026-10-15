# Bounded audio queues and latency work

The current runtime and background host are C++. USBip remains the virtual USB
transport; this is not a claim that its kernel bus has been removed or that a
particular end-to-end latency has been achieved.

## Runtime behavior

- Each association owns separate USB/control/audio state and completion tokens.
- Capture retains at most 32 one-millisecond frames and drops data older than
  32 ms. A delayed consumer cannot accumulate seconds of stale capture.
- Render expires after 64 ms on the 2024 connector and 96 ms on the 2021 connector.
  The 2021 model has its own eight-millisecond clock; the 2024 model follows fresh
  RF credits. Credits and partial PCM storage are bounded.
- Old-headset capture concealment ends after 128 ms without RF. It does not keep
  a lost radio session alive with unlimited fabricated samples.
- Radio submission alternates between peers. USB success follows real physical
  transmit completion; expired ISO work reports an error rather than fake success.
- Snapshot writes run asynchronously with coalescing. An unreadable snapshot is
  not an explicit disconnect and must not remove an imported device.

## Evidence and limits

Earlier Python reference tests injected event-loop pauses, delayed completions,
queue pressure and per-device failures. Short physical 100/500 ms pause checks
recovered without a headset restart. These are reference implementation results,
not a matched C++ performance benchmark.

C++ tests cover descriptor/transport equivalence, real loopback imports, bounded
PCM and clock behavior, cancellation, association reuse and sibling isolation.
A six-second exclusive lock on a live slot's status file preserved its port and
generation. Both physical headset revisions were reported clean in a short joint
check after the unreadable-snapshot disconnect bug was fixed.

Sustained RF congestion, long-running voice/music, system DPC delays and other PCs
remain incompletely measured. To report a problem, provide model, Windows build,
app version, approximate failure time and whether the Windows device disappeared.
Do not include raw recordings or unredacted private state by default.
