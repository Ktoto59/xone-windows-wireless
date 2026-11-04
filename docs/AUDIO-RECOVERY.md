# Experimental audio recovery candidate

This C++ candidate is prepared on `experimental` for a later hardware test.
Synthetic checks exercise bridge policy, not receiver firmware, RF reception or
audible output. A PC memory queue cannot feed the headset while Windows is not
scheduling the bridge worker. No claim of uninterrupted audio during a stall.

## Behavior and limits

- Retain at most 32 aligned 192-byte PCM frames. Each frame keeps its own USB
  arrival deadline: 64 ms for 2024, 96 ms for 2021. These are maximum ages, not
  a new prebuffer or a promise of end-to-end latency.
- Expire individual frames and preserve fresh fragments from other requests.
  Unlink removes only the active request's PCM. Disown completed fragments
  before USB/IP can reuse their old request sequence number.
- Bound a physical send by the oldest of its eight frames. If that unsent
  packet expires, preserve the same request's fresh tail, report affected ISO
  slots as expired, and do not retry the old packet. Count request expiry once.
  Earlier completions for accepted short fragments cannot be retroactively
  changed; the dropped-frame counter still records their later age loss.
- Preserve successfully sent ISO slots when the remaining request expires.
  A delayed callback for a fully successful request is not counted as media
  expiry. Track frame counts even when one ISO slot contains multiple packets.
- Preserve the physical-read timestamp through radio, routing and USB mailboxes.
  Use processing time separately for timers. Old/out-of-order media cannot
  refresh the 32 ms window; accepted microphone frames retain their real age.
- For 2024, continue rendering at the observed 8 ms block period during a short
  gap in incoming microphone events. Start only after a valid layout and the
  first render dispatch. Stop prediction at 32 ms since the last RX audio event.
  Sending, timers and USB requests never refresh that horizon. The existing
  two RX-derived credits remain a bounded local pacing heuristic, not proven
  receiver flow-control grants. Predicted sends need hardware validation.
- Estimate the 2024 period from a fitted slope over at least 64 packets and
  16 arrival observations. Include short and coalesced arrivals in that fit;
  selectively discarding short intervals biases the rate downward. Bound the
  learned period to 7.5-8.5 ms and keep TX phase independent of individual RX
  delivery jitter. A gap of at least 32 ms resets the estimate to nominal 8 ms.
  See [the RC stutter regression](AUDIO-STUTTER-RC.md) for the reproduction.
- Preserve timer phase across ordinary jitter. After missing a whole render
  period, re-anchor in the present instead of replaying missed timer slots in
  a burst. Apply this burst bound to 2021's existing independent clock, keeping
  its 128 ms incoming-clock limit.
- Keep recovery for real transport errors and RF removal. Media expiry alone
  does not reset RF/USB. Preserve control priority and existing cancellation.
- Drop isolated malformed/unsupported media without killing the worker. Eight
  consecutive invalid packets request USB/audio reinitialization while retaining
  RF association; repeat at most once per ten seconds, including across imports.
  Valid media clears the streak. Reliable/control errors are not swallowed.
- Stop waiting indefinitely for the first model announcement. After five seconds,
  the existing per-peer failure/backoff path re-enumerates that client, preserving
  healthy siblings and the physical adapter. Never guess a model from cached MAC.

## Physical transmit completion

Native OUT waits for the remaining media deadline, with a one-second ceiling
for control writes. Waiting for the shared write mutex is bounded too. Deadline
or owner cancellation requests `CancelIoEx`, then always drains completion before
releasing buffers/OVERLAPPED/event state. A successful completion can win that
race and remains success. An aborted in-flight operation is not reported as an
unsent `-18` block and is never blindly replayed. An unresponsive kernel driver
can still delay draining; cancellation is not a hard real-time guarantee.

The sender retains distinct timeout (`-110`), owner cancellation (`-125`),
device removal (`-19`) and other IO/short-write (`-5`) results. The radio RAM
snapshot retains `UsbTx.LastErrorStatus`, `LastNativeError` and `LastErrorBytes`
for the last failed write. Successful later writes do not erase that evidence.
Timeout/uncertain in-flight failure still uses the audio recovery path; changing
error reporting does not make such a packet safe to retry.

This draft adds no adaptive 16-24 ms reserve, replacement render samples or
crossfades. It leaves the microphone clock, thread priorities and USB power
settings unchanged. Receiver capacity and remaining artifacts must be measured
before adding further buffering or concealment.

## Counters in memory

The existing native state publisher exposes these additional lifetime counters:

| Field | Meaning |
| --- | --- |
| `RenderDroppedFrames` | PCM frames removed by age or rejected in an unsent expired packet; excludes unlink/reset cleanup |
| `RenderPredictedPackets` | Sends without an RX-derived credit inside the prediction horizon |
| `RenderRephases` | Timer restarts after missing at least one whole period |
| `RenderQueueHighWater` | Maximum accumulated PCM frames, bounded by 32 |
| `MaxRadioAudioGapUs` | Gap between retained physical-read timestamps; buffering below that boundary is not measurable here |
| `StaleAudioEvents` | Old/out-of-order audio events rejected at the core boundary |
| `MalformedAudioPackets` | Rejected framing/layout on the media path |
| `AudioResyncs` | USB/audio reinitializations caused by sustained bad media |

The installed host still publishes through shared memory. No new journal events,
packet logging or application file IO are introduced. Explicit developer file
snapshots retain their existing behavior.

## Automated checks

After the normal bridge build, `scripts/Test-CppBridge.cmd` includes
`usbip-audio-recovery-tests.exe`. Its virtual-clock scenarios use no sleeps,
physical adapter or driver changes:

- Missing RX for three render periods, stopping at the 32 ms prediction limit.
- Worker/callback pauses of 10, 20, 40 and 80 ms, fresh output after resumption,
  no immediate catch-up bursts and no media-induced RF reset.
- Ordinary timer jitter without cumulative phase drift.
- Old/fresh partial PCM, verifying retained bytes after selective expiry.
- Reused request sequences and unlink ownership.
- A mixed-age expired send with a fresh tail in the same request, checking both
  retained bytes and per-slot USB completion status.
- The 2021 clock and recovery on a real transport failure.
- Eighty virtual seconds with both a faster and slower RX clock, without
  unbounded render lead; ordinary timer jitter and source-age preservation.
- Single bad media, sustained bad media, cooldown across reimport and unchanged
  rejection of corrupt reliable/control messages.
- Successful ISO slots retained when a partially completed request expires.

`native-route-recovery-tests.exe` uses a fake peer factory: no native listener or
adapter is opened. It checks timestamp routing, announce timeout, retry rearm and
sibling isolation. `usb-write-completion-tests.exe` injects wait/cancel/drain and
uses an isolated Windows event to check the stop race, timeout versus successful
completion, native errors and short writes without USB. Both run in existing
build/test entry points and CI.

Python remains the pre-change baseline. Differential tests retain shared
descriptor/control and transport/recovery contracts. The PCM-age test explicitly
asserts the intended C++/Python difference. `XONE_PARITY_PROBE` can select an
isolated test executable instead of the normal output directory.

## Later hardware comparison

Keep the installed version for comparison and build the candidate separately.
Run only one bridge owner against the adapter. First check 2024 playback and
microphone under normal load, then the same game/workload that caused glitches,
using the same port and settings. Observe whether a gap returns directly to
clean audio, whether noise persists, and whether latency grows over time. Query
aggregate memory counters on demand; continuous packet logs are unnecessary.

Also check 2021, disconnect/reconnect and idle wake. If prediction causes noise,
receiver errors or accumulating latency, keep the selective PCM expiry change
and revise pacing before release. A synthetic pass cannot establish that
receiver buffering is safe.
