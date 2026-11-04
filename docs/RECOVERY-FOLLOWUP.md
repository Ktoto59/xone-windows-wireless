# Host USB recovery follow-up

Date: 2026-11-04. Branch: `audio-recovery-stock-driver`.
Verified starting commit: `85bf977`. This is a hardware-test candidate, not a release.

## Normal disconnects and failure backoff

The host previously turned every positive `UsbImportRecovery::detach()` result
into a generic exception after detaching. The per-peer exception handler then
called `PeerRecoveryBackoff::failed()`. Power-off, RF reassociation and generation
changes could consequently grow the USB error delay through 1, 2, 4, 8, 16, 32
and 60 seconds, even when every USB operation succeeded.

`UsbDetachReason` now makes the evidence explicit:

| Reason | Action after successful cleanup | Failure count |
| --- | --- | --- |
| `rf_disconnected` | Wait for an attachable RF snapshot | Unchanged |
| `association_changed` | Reobserve the new association before attach | Unchanged |
| `generation_changed` without recovery evidence | Reobserve before attach | Unchanged |
| `usb_recovery` (generation changed with `RecoveryRequired`) | Schedule a local USB retry | Increment |
| `import_closed` | Schedule a local USB retry | Increment |
| `import_missing` (existing fresh-snapshot/10-second confirmation) | Schedule a local USB retry | Increment |
| An attach, inventory or detach command fails | Preserve cleanup state; schedule retry | Increment |

Association changes and explicit normal RF removal take precedence over USB
close counters. A generation change alone is not an error. Actual recovery
evidence is still an error even if it also changes the generation. The failure
path uses the explicit enum and a typed `UsbImportFailure`; it does not classify
exception text. Real failures are scheduled after synchronous cleanup completes,
so a slow cleanup cannot consume their backoff interval.

Normal disconnects interrupt the stability interval but do not erase earlier
real failures. Otherwise repeated power cycles could bypass protection from
persistent USB errors. Each peer retains its own capped exponential backoff.
The existing 60-second reset algorithm remains unchanged. The host now also
interrupts its interval on an unavailable snapshot and waits for a confirmed
post-attach import before crediting healthy time. The snapshot used to initiate
attach cannot prove that the new import is healthy. A cleanup failure invalidates
the recorded generation before throwing, so cleanup is retried even if no new RF
snapshot arrives.

The host snapshot decision is shared with the regression test. Tests replace
only OS-facing attach/detach callbacks, not the production decision or failure
policy.

## Partial attach: confirmed risk and bounded reconciliation

The pinned upstream
[CLI attach implementation](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/userspace/usbip/attach.cpp)
prints the port only after `vhci::attach` returns. The driver's
[connection completion path](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/drivers/ude/vhci_ioctl.cpp)
imports and plugs the device before completing the request. Therefore an import
can exist when the CLI's stdout, exit result or process lifetime is lost. This
confirms an accounting gap; it does not mean every failing kernel request leaks
an import. The driver already cleans up several internal failure paths.

The new `UsbAttachRecovery` runs synchronously:

1. Query a complete `usbip port` inventory before attach. Failure or malformed
   output prevents attach. An existing import of this exact loopback endpoint
   prevents attach and is never adopted, even with empty CLI serial metadata.
2. Save the pre-attach port numbers and mark the attempt pending before starting
   the unchanged `--once --terse` attach command.
3. A successful command records its port as before; invalid or pre-existing
   returned port numbers enter the failure path.
4. On failure, query the inventory again after the CLI job has closed. Only one
   new port absent from the baseline, with exact `127.0.0.1:<slot-port>/1-1`
   endpoint and empty CLI serial metadata, can become a cleanup record. It is
   never promoted to a healthy/successful attach.
5. Failed inventory queries retain the pending attempt and its baseline. Pending
   reconciliation runs before another attach and during normal host stop.
   Multiple matching candidates block adoption. Once a result was ambiguous,
   reducing it to one candidate does not authorize adoption either.
6. The existing `detach_owned_usb_port` verifies endpoint, bus ID and serial again
   immediately before targeted numeric detach. Reused numbers pointing to a
   different identity are forgotten without detach. Failed cleanup retains its
   record. No detach-all, foreign-port sweep or recursive attach retry exists.

The preflight, attach and immediate reconciliation share the host's existing
15-second operation budget. If attach consumes it, the attempt remains pending
for a later backoff-controlled cleanup. Stop cleanup retains its existing shared
16-second / per-peer 2-second budgets. TCP progress timeout is unchanged.

The inventory parser follows the pinned
[port listing format](https://github.com/vadimgrn/usbip-win2/blob/v.0.9.8.1/userspace/usbip/port.cpp),
including the final mode field, and rejects incomplete or duplicate records.
An empty *successful* query means no imports. Query failure never means empty.
No `--serial` override is added: Windows continues using the existing server
serial/USB descriptors and device identities.

## Changed files

| File | Purpose |
| --- | --- |
| `src/background_host.cpp` | Separate normal lifecycle transitions from failures; integrate pending attach cleanup and testable snapshot decisions |
| `src/usb_import_recovery.hpp` | Explicit detach reasons and typed USB import failure |
| `src/usb_attach_recovery.hpp` | Preflight inventory, failed-attach accounting and fail-closed pending state |
| `tests/host_recovery_tests.cpp` | Host lifecycle, backoff, ownership and actual child-process fault injection |
| `tests/usb_import_recovery_tests.cpp` | Preserve existing snapshot tests and assert each typed reason |
| `docs/RECOVERY-FOLLOWUP.md` | Findings, evidence, validation and hardware procedure |

## Automated validation

| Check | Result |
| --- | --- |
| `scripts/build.cmd` | PASS: all baseline C++ suites, including 17 protocol / 41 session tests |
| `scripts/Build-CppBridge.cmd` | PASS: native bridge build |
| `scripts/Build-Appliance.cmd` | PASS: host/candidate build and appliance C++ suites |
| `scripts/Test-CppBridge.cmd` | PASS: native codec/core/audio/import/route/snapshot/journal suites, 7 parity and 8 native integration tests |
| `python -m unittest discover -s tests -p '*tests.py'` | PASS: 122 tests, no skips |
| Diagnostics-disabled appliance build | PASS: build and C++ suites, including disabled-diagnostics guard |
| Nine maintenance/stock-driver PowerShell suites | PASS: all system driver/task actions mocked |
| Stock bundle preparation and `bundled_stock_driver_tests.ps1` | PASS: four authentic pinned CABs; offline verification only, no driver binding |
| `python scripts/check-public-content.py` and `git diff --check` | PASS |
| GitHub Windows CI | Push-triggered `Public source hygiene` workflow; final run URL/result supplied with delivery |

New coverage includes ten fast normal off/on cycles, reconnect without delay,
AssociationId changes with a composite-generation collision, generation-only
changes, USB recovery/close/missing-import failures, detach failure cleanup,
interrupted and 60-second healthy reset intervals, stale pre-attach snapshots,
and hundreds of polls with a persistently failing peer while a sibling remains
attached. Normal events preserve genuine failure history without incrementing it.

Failed-attach tests cover no import, a created import with a lost result,
unavailable/invalid inventory, pre-existing imports, foreign endpoint/serial,
reused numbers, ambiguous results and successful attach. A real child-process
fixture creates a file representing independent VHCI state, then hangs, exits
nonzero, returns no port, or emits malformed stdout. It exercises the production
process/job/pipe/timeout implementation and subsequent targeted cleanup without
opening USBip or accessing physical hardware.

## Remaining limits and hardware acceptance

* Inventory comparison and numeric detach are separate CLI operations. A
  concurrent external administrator importing the *identical* endpoint with
  identical empty metadata during this transaction cannot be distinguished from
  our attempt. Likewise, an identical-identity replacement between verification
  and detach cannot be detected. The CLI offers no atomic per-attempt ownership
  token. Do not run a second USBip manager against the same slot endpoints during
  testing. Different endpoints/serials and pre-existing port numbers are protected.
* Unknown/ambiguous inventory fails closed. This can leave a resource awaiting
  manual diagnosis; it never justifies detaching arbitrary ports. In-memory
  pending evidence does not survive host termination/crash. A later host refuses
  an already imported endpoint rather than claiming an old/foreign import.
* Late kernel completion after an absent post-failure inventory may leave an
  import outside the recorded attempt. The next preflight refuses that occupied
  endpoint, preventing an accumulating attach loop. Actual driver cancellation
  timing still requires hardware validation.
* The synchronous USBip architecture remains: an unhealthy slot can delay later
  host bookkeeping until its bounded command returns. Existing native audio
  workers are independent. No new watchdog disconnects healthy imports.
* RF protocol, GIP authentication, model recognition, PCM/buffers, 8-ms pacing,
  MMCSS/priorities, adapter handling, TCP progress policy, Microsoft drivers and
  USB descriptors are unchanged relative to `85bf977`. Existing audio/parity
  tests passed, but they cannot establish audible playback/microphone quality
  or physical RF stability.

No real power cycle, pairing, driver installation, physical adapter access,
release or main-branch merge was performed. Use a dedicated hardware test
installation and the existing [stability audit procedure](STABILITY-AUDIT.md#hardware-comparison-procedure-not-executed).
First perform 30 ordinary cycles, including at least ten rapid 1-3-second off/on
cycles. Check that normal `usb_reimport_required.reason` values do not increment
per-device `RetryCount`; prior genuine failures clear after 60 healthy seconds.
Then keep a second device playing/recording while cycling the first. Confirm
targeted port removal, no repeated attach storm and no sibling endpoint removal.
Finish with 8-24 hours of bidirectional audio, silence/mute/resume periods and
normal sleep/resume. Listen to playback and microphone recordings and record
reconnect latency, Windows build, headset firmware and adapter/topology. Automated
validation makes this candidate ready for those trials; hardware acceptance is
still outstanding.
