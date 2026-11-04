# Playback stutter in the first recovery RC builds

Date: 2026-11-04. Parent: `33da530` on `audio-recovery-stock-driver`.

## Hardware observations

The user reported continuous crackling and interruptions with diagnostic
`0.1.0-rc.1.33da530`. The same symptoms persisted with diagnostics disabled in
`0.1.0-rc.2.33da530`. The remembered last good installation was alpha.6.1.
Consequently the earlier assumption that the current audio path was already
hardware-validated could not be used to exclude changes after alpha.6.1.

Installed binary hashes matched the RC1 package. During a 124.094-second
observation interval, the same RF association/import accumulated 842 expired
render requests and 6,736 dropped render frames. There were no new USB closes,
imports, audio resyncs, stale audio events or mailbox audio drops. MMCSS was
active. A subsequent RC2 snapshot again showed render expiry without recovery;
its physical transmit counters reported zero failed writes and a maximum write
duration of 376 microseconds. These observations locate sustained loss on the
render path; they do not establish the cause of every audible artifact.

No installed runtime, driver binding, audio setting or adapter power policy was
changed by the investigation. Status snapshots and existing journals were read;
private evidence is retained only in ignored local work directories.

## Deterministically reproduced defect

The 2024 clock estimator introduced after alpha.6.1 smoothed individual RX gaps
between 4 and 12 ms, ignoring shorter intervals and packet batches. A lossless
stream arriving at intervals of 12, 1 and 11 ms has an actual average of 8 ms.
Ignoring the 1-ms intervals trains the estimator toward its upper 8.5-ms limit.
Playback consequently runs at about 117.65 packets/second instead of 125.
The per-arrival phase corrections also turn delivery jitter into TX timing
changes; simply averaging whole windows did not eliminate the queued-URB loss.

The new virtual-time regression keeps eight USB ISO render requests queued,
replaces completed requests immediately, supplies the lossless 12/1/11-ms RX
pattern and completes every physical send successfully after 100 microseconds.
It uses the production `Core`, without sockets, a real adapter or sleeps.

| 30-second scenario | Parent code | Corrected code |
| --- | --- | --- |
| Received audio packets | 3,750 | 3,750 |
| Successfully sent audio packets | 3,530 | 3,750 |
| Expired render requests | 441 | 0 |
| Dropped render frames | 3,528 | 0 |

The previous constant-rate drift tests had no realistic eight-request USB
backlog and did not exercise asymmetric burst delivery. They therefore passed
while this steady-state loss was possible.

## Targeted correction

`RadioRenderTiming` now fits elapsed arrival time against cumulative packet
position over at least 64 packets and 16 observations, including short intervals,
duplicate arrival timestamps and multi-packet arrivals. A fitted slope is less
sensitive than endpoint-only averaging to jitter at a window boundary. Valid
7.5-8.5-ms estimates update the existing frequency gradually. Individual RX
arrival phases no longer move the TX deadline. Out-of-order observations are
ignored; the existing 32-ms gap reset remains.

The default period remains 8 ms. This preserves adaptation to genuine clock
drift without learning only the slow half of delivery jitter. The core's bounded
prediction horizon, no-catch-up policy, PCM age limits, buffer sizes, per-frame
ownership/expiry, microphone path, physical completion/cancellation, MMCSS,
priorities, RF protocol and GIP authentication are unchanged. The host backoff
and partial-attach fixes from `33da530` are retained in full.

## Regression coverage and acceptance

The audio recovery suite additionally checks 30-second queued render streams
with 12/1/11-ms delivery, 7/1/12/12-ms delivery, 16/0-ms coalesced timestamps,
ordinary 8-ms delivery, and two packets per 16-ms arrival. All must have zero
render expiry/dropped frames and normal send throughput. Direct estimator tests
cover nominal frequency, real +/-50-microsecond drift, out-of-order timestamps,
32-ms gap reset and clear/reset. Existing 80-second drift, worker-stall,
prediction-horizon, per-frame expiry, unlink, microphone/transport and independent
2021 clock tests remain required.

Validation includes the baseline C++ suites, rebuilt native bridge/recovery
suites, full Python discovery, a fresh diagnostics-disabled appliance build and
the branch's Windows CI. The new installer is a local RC for an audible hardware
check; it is not a published release. The actual CI result and installer path
are provided with delivery.

The reproduction proves the clock-estimation defect and its correction for the
tested schedules. The user's physical crackling is not marked resolved until
the corrected RC is installed and listened to. Confirm playback first with the
same application/source and topology, then microphone use, a normal reconnect,
two-device isolation and a longer soak. A remaining artifact requires fresh
status evidence; do not mask it with larger buffers or recovery watchdogs.

## RC3 follow-up: render deadline has no dispatch margin

The user initially heard clean playback in RC3, then reported substantially fewer
but recurring bursts of crackling/pauses. The microphone remained enabled without
joining voice chat. This is not accepted as a successful hardware fix.

A further 206.140-second snapshot interval on the same association/import showed
25,768 received audio packets (125.002/s), 25,571 sent packets (124.047/s), 201
expired render requests and 1,608 dropped render frames. No USB close/reimport,
recovery, resync, stale event or mailbox drop occurred. The exact RX mean rate
and remaining expiry justified checking actual dispatch timing rather than
continuing to tune the already corrected frequency estimator.

The queued-URB regression originally delivered callbacks and timer wakes exactly
at their nominal virtual time. Adding only 500 microseconds of dispatch delay
every seventeenth event, while retaining each RX packet's original arrival time
and completing every physical send successfully after 100 microseconds,
reproduces 220 expired requests / 1,760 dropped frames in 30 seconds of a perfectly
regular 8-ms stream. Both RX and TX count 3,750 packets; the discarded requests
advance the source stream and can create audible gaps despite normal throughput.

The 2024 request lifetime was 64 ms, equal to the entire observed Windows queue
of eight 8-ms render URBs. A replacement request can legitimately spend nearly
all that budget waiting for preceding successful sends. Small dispatch jitter
then crosses the age boundary and discards its eight frames. The lower-level
physical write itself is not stalled. The same missing headroom also caused
expiry in a full queue with legitimate +/-50-us clock drift.

The targeted adjustment sets the 2024 lifetime to `8 * (8 + 1) = 72 ms`: eight
observed queued blocks plus one block of bounded dispatch/completion margin.
It introduces no additional prefill or PCM storage; the 32-frame accumulator,
normal 8-ms pacing, clock estimator, prediction horizon, rephase/no-burst rules,
microphone path and all radio/USB recovery safeguards remain unchanged. The 2021
96-ms limit and controller 64-ms limit are unchanged. This deliberately permits
up to 8 ms more source age during backlog than RC3; it is not a claim of unchanged
worst-case latency. Data beyond the new bound is still discarded and expired
unsent packets are not retransmitted.

The enhanced tests cover nine combinations of 0.5/1/2-ms dispatch jitter and
50/100/400-us successful physical completions, plus full queues at 7,950/8,050-us
RX periods. They require zero render expiry/dropped frames. The 500-us case
improves from 220 expired requests to zero. Age-boundary tests now use the actual
configured model lifetime and continue checking stale/fresh fragment ownership,
partial ISO status, unlink and 80/250/500-ms stalls. The Python reference policy
is unchanged; shared differential scenarios are sampled at 80/120 ms, beyond
both the reference and native relevant expiry boundaries.

Two RAM-only fields, `RenderPendingHighWater` and `RenderPeriodNs`, expose the
queue depth and learned period for the next hardware observation without enabling
journals, packet capture or file polling. All remaining real-device artifacts
still require an audible check of the next RC followed by a longer soak.

## RC4 hardware correction: transient queue depth is ten

The new RAM counter on installed RC4 measured `RenderPendingHighWater=10` while
instantaneous render depth was seven/eight. The earlier eight-request assumption
described steady state, not the peak. During the next 110.079-second interval,
38 more requests expired on the same import. RC4 therefore improved the symptom
but did not pass hardware acceptance. Increasing the bound without measuring the
queue peak would not have been sufficient evidence for a further adjustment.

The fixed 2024 lifetime is now `8 * (10 + 1) = 88 ms`, using the measured peak plus
one dispatch block. It is not adjusted upward automatically. The original RC3
64-ms cutoff can now retain up to 24 ms less source age than this candidate's
worst-case limit; the 32-frame PCM capacity, normal pacing and absence of an
additional prefill remain unchanged. Long stalls still discard expired data;
the stall tests now also exercise 100 ms, beyond the new bound.

Regression coverage holds both eight and ten URBs continuously queued, rather
than only visiting the transient peak. All forty-two dispatch/completion timing
combinations and both +/-50-us clock cases at each queue depth require zero
expired requests and zero dropped render frames. Differential stale/fresh tests
are sampled at 100/140 ms to stay beyond both relevant age boundaries while
retaining the unchanged Python baseline policy.

The next candidate still requires the user's longer audible playback test. A
successful CI or an initial clean second is not treated as hardware acceptance.

See [residual diagnosis](AUDIO-RESIDUAL-DIAGNOSIS.md) for the new hardware deltas,
controlled 72/88-ms comparison, full-path audit, separate RAM telemetry and
60-second blackout/recovery tests.
