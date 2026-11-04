# Residual RC4 playback loss investigation

Date: 2026-11-04. Branch: `audio-recovery-stock-driver`; base: `0f987f6`.
Hardware acceptance remains pending. No release, main merge, driver installation,
adapter power change or installed-runtime replacement is part of this change.

## Established cause and remaining uncertainty

RC4's 72-ms source lifetime is shorter than the observed transient ten-request
render backlog. Ten continuously queued 8-ms URBs reproduce expiry even when
every physical TX succeeds, the sample clock is exactly 8 ms, and dispatch jitter
is only 0.5 ms. This establishes an insufficient lifetime for that queue depth.
It does not prove that every audible artifact has the same cause.

The working tree already contained the 88-ms candidate and corresponding tests
when this investigation started. Those edits were preserved (the original diff
is saved locally in `work/pre-existing-audio-investigation.patch`) and checked
against new hardware intervals and a controlled 72/88-ms comparison. No further
increase was made. The limit is fixed at `10 * 8 + 8 = 88 ms`: the measured peak
plus one dispatch block. It allows up to 16 ms more source age than RC4, keeps
the 32-frame PCM capacity, and introduces no additional prefill. This is a
latency tradeoff, not a claim of unchanged worst-case latency.

## Measurements before the candidate

The installed `xone-probe-cpp.exe` SHA-256 matched the existing RC4 package:
`c1a25eb1fc4f4f17594e2dd9e5324251d53c54808d6802708dbe4b00a00bb5bd`.
The user confirmed continuous playback. Snapshots were read from runtime RAM;
private snapshots are retained only under ignored `work/rc4-residual-investigation`.
Each row below uses counter differences within the same generation/import.

| Interval | RX | Successful TX | Expired requests | Dropped PCM frames | TX expired | TX failed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Initial 118.078 s | 14762 | 14612 | 167 | 1336 | 3 | 0 |
| Subsequent 60.032 s | 7504 | 7454 | 53 | 424 | 5 | 0 |
| Subsequent 60.046 s | 7506 | 7458 | 56 | 448 | 4 | 0 |

In both minute intervals, deltas for rephases, USB imports/closes, audio resyncs,
stale audio events and mailbox audio drops were zero. Predicted packets increased
by 51 and 47. Pending/PCM high-water marks were 10 requests / 8 one-ms frames.
The last learned periods were 7,962,099 and 7,968,455 ns. These instantaneous
estimates are not interval averages or evidence of a changed sample rate.
Lifetime maxima at the end were: mailbox delay 2,599 us; radio batch 335 us;
socket dispatch 93 us; delivered RX gap 16,646 us; physical USB write 1,055 us;
radio queue wait 714 us. There were no writes over 8 ms in either interval.
Maxima cannot be subtracted to recover interval maxima. Radio/slot publications
are asynchronous, so TX packet deltas need not match across those publishers.

## Full-path audit

1. Windows ISO work arrives through a nonblocking localhost USB/IP socket.
   `Core::submit` starts its source lifetime here; time already spent inside the
   Windows driver before submission is not observable by this core.
2. `render_wait_` retains Windows' backlog. The ten-request high-water mark,
   rather than the usual instantaneous seven/eight requests, determines the
   demonstrated age deficit. PCM is staged only for the head request, with a
   fixed 32-frame capacity and individual owner/expiry metadata.
3. `pump` checks age before staging/sending. The oldest constituent bounds the
   outbound packet. Successful TX does not imply that other queued requests
   avoided expiry; normal RX/TX throughput can coexist with source-frame loss.
4. The native worker uses a high-resolution one-shot waitable timer and MMCSS.
   It drains at most 64 mailbox events, with reliable completions before media,
   and ticks/services the core before calculating the next deadline. New RF
   events can wake it earlier; they do not move the render phase. Socket parsing
   and event batches run on this worker, so their dispatch maxima matter.
5. Rephase occurs only after a full missed period and suppresses unlimited
   replay of missed slots. New blackout tests find a stable residual phase
   (2 ms after a 10-ms stall) rather than ongoing phase accumulation during the
   clean recovery tail. The hardware intervals have zero rephases, so there is
   no evidence for changing this policy. RC3's windowed rate fit is unchanged.
6. Native output, session polling, control-priority RF TX queue and WinUSB form
   separate handoff stages. Media deadlines propagate through them. Expiry
   before WritePipe is deliberate unsent media loss; actual write failure still
   invokes the existing recovery policy. There is one physical OUT at a time.
7. Physical results travel back through session polling and the native mailbox.
   A successful write stays successful even if its callback is dispatched after
   the media deadline. USB/IP replies then flush to TCP. Kernel consumption of
   that reply and over-the-air headset reception are not confirmed by `send()`
   or a successful USB write.

## Minimal RAM telemetry

The candidate adds scalar counters/maxima, no PCM/USB capture or periodic files:

| Observation | Field |
| --- | --- |
| A: request with unsent slots expires in the core | `RenderExpiredBeforeTx` |
| B: expired frames removed from PCM accumulator | `RenderPcmExpiredFrames` |
| Outbound media expires/is rejected after core submission | `RenderTxExpired` |
| C: late core scheduling | `MaxRenderLatenessUs` |
| C: time of actual sender invocation relative to scheduled render slot | Radio `UsbTx.MaxStartLatenessUs` |
| D: non-expiry render transport failure | `RenderTxFailed`, radio `UsbTx.Failed` and native error fields |
| E: physical completion to native-worker dispatch | `MaxTxCompletionDispatchUs` |
| E: queued USB/IP reply to fully flushed TCP output | `MaxUsbReplyFlushUs` |
| Successful callback handled beyond request deadline | `RenderSuccessAfterDeadline` |
| Oldest constituent source age at core send | `MaxRenderPcmAgeUs` |
| Timer-selected wake count, overshoot maxima and threshold counts | `TimerWakes`, `MaxTimerLatenessUs`, `TimerLate1ms`, `TimerLate8ms` |

Physical completion timestamps and render scheduling timestamps survive both
routers. Legacy brokers fall back to their existing completion behavior.
Timer overshoot measures waits where the timer won; event-selected wakes are
instead reflected by render lateness. The PCM-age maximum is at core send,
not the physical transmitter; sender lateness covers the following handoffs.
Reply-flush time covers all replies and stops when Winsock accepts the bytes,
not when Windows completes the URB. These maxima are diagnostic bounds, not
per-packet correlation or histograms.

`RenderExpiredRequests` retains its legacy unique-request meaning;
`RenderTxExpired` counts outbound blocks. The two new expiry counters must not
be summed as unique requests when a multi-block request loses more than once.
`RenderDroppedFrames`/`RenderPcmExpiredFrames` do not count PCM never staged from
an expired waiting request. Unlink/lifecycle invalidation is not classified as
queue-pressure loss. F, genuine RF delivery loss, remains an open hypothesis:
RX gap, stale-event and mailbox counters help isolate host-side disruption,
but this path has no per-render RF ACK counter. A successful WinUSB write alone
must never be reported as proof of reception by the headset.

## Controlled reproduction and regression coverage

The comparison compiles the same instrumented core/simulator twice with only
the 2024 age policy set to 72 or 88 ms. It does not substitute a different pacing
algorithm. Physical completions succeed after 100 us throughout.

| Scenario | 72 ms expired / dropped | 88 ms expired / dropped |
| --- | ---: | ---: |
| 30 s, 8 URBs, 0.5-ms periodic dispatch jitter | 0 / 0 | 0 / 0 |
| 30 s, 10 URBs, 0.5-ms periodic dispatch jitter | 416 / 3328 | 0 / 0 |
| 60 s, 10 URBs, 23 blackouts of 3 ms | 836 / 6688 | 0 / 0 |
| 60 s, 10 URBs, 23 blackouts of 5 ms | 836 / 6688 | 0 / 0 |

With the candidate, the latter two streams reach measured PCM ages of 82.9 and
84.9 ms: both exceed RC4's limit. Ordinary 8-ms, 12/1/11-ms, grouped-packet and
7/1/12/12-ms streams, true 7.95/8.05-ms sample clocks and queues of both depths
remain covered. The periodic dispatch matrix now includes 0.5/1/2/3/5/8/10 ms
and 50/100/400-us completions (42 combinations). Those small callback-oriented
event delays alone do not necessarily cross an actual render deadline.

New 60-second blackout tests therefore hold *all* dispatch across a due timer,
including RX and TX completion: 3/5/8/10 ms, repeated isolated and clustered
stalls, both headset revisions and both queue depths. An uneven 0-10-ms schedule
and sustained delayed TX callbacks exercise recovery to a normal tail. Unique
PCM identities verify increasing source order, no stale duplicate, source age
below its fixed limit, bounded accumulator, at most one outbound packet per
dispatch and at most two sends in any 8-ms window. The final ten seconds require
stable phase within 10 us. Long genuine stalls may lose slots; throughput and
recovery are bounded instead of requiring impossible lossless playback.

Counter-isolation tests distinguish core expiry, PCM expiry, outbound expiry,
transport failure and late successful completion. Route tests preserve physical
completion time; queue tests verify actual sender lateness and completion time.
Existing 10/20/40/80/100/250/500-ms stalls, partial ISO completion, fragment
ownership, cancellation, microphone, association recovery and sibling isolation
remain required. No experimental timer, reset or pacing change was introduced.

## Repeat hardware test

Local validation: `Build-CppBridge.cmd`, `Test-CppBridge.cmd`, the full Python
suite (122 tests), and appliance builds with and without diagnostics passed.
This covers native audio/core/codec, loopback transport and equivalence, both
revisions, physical completion races, route recovery, RAM IPC and host recovery.
The PowerShell observation script was exercised against installed RC4 for three
samples at 60-second intervals. Remote Windows CI is required for the final
commit; its run URL/result is reported with delivery, not inferred from a local
build or the previous RC4 CI run.

The candidate binaries are built locally in `outputs/residual-audio-candidate`;
a second build in `outputs/residual-audio-no-diagnostics` disables journals.
Neither is installed automatically. To test without touching drivers, stop the
existing host with its `--stop` command, wait for exit, back up its three output
executables, copy the candidate `xone-background.exe`, `xone-probe-cpp.exe` and
`xone-bind.exe` into the existing installation's `outputs`, and start the host
again. Keep existing configuration/firmware and the same source/application.
Record which candidate was used. Do not run both hosts simultaneously.

During at least ten minutes of continuous listening, run from the repository:

```powershell
.\scripts\Get-AudioDiagnostics.ps1 -Samples 10 -IntervalSeconds 60
```

The script reads one RAM snapshot per minute, saves private local evidence and
reports deltas only across an unchanged import/generation. Missing candidate
fields are omitted when observing RC4. `Fresh=false` or a changed import makes
that interval unsuitable for steady-stream comparison. Keep diagnostics local;
raw snapshots contain device identifiers and local paths.

Record audible glitch times, test microphone/voice use, then normal reconnect
and (when available) simultaneous 2021/2024 playback. Compare counter deltas and
source ages against RC4. Zero expiry alone is insufficient to declare the sound
fixed. If artifacts persist with zero host loss, investigate actual RF delivery
and kernel completion latency before changing buffers, age limits or resets.
