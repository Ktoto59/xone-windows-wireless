# Runtime IO isolation

See the [architecture diagrams](ARCHITECTURE.md#threads-and-traffic) for the
component map, thread/queue flow and connection sequence.

The experimental C++ host keeps runtime state in session-local, pagefile-backed
Windows shared memory. No application state file is created for this mapping.
Windows may page memory according to its own policy; this is not a promise that
the OS performs no disk IO.

## Execution

- Two existing overlapped receive workers own the adapter IN endpoints.
- The radio owner parses events and advances RF/GIP lifecycle state.
- Each virtual device has its own USB/IP/audio event worker.
- A bounded transmit executor serializes outgoing native GIP on endpoint 04.
  It waits for overlapped completion outside the radio owner. Control precedes
  unsent media; an in-flight operation is not preempted merely for priority.
- A bounded snapshot publisher coalesces state in memory. A stalled observer
  cannot hold the shared-memory mutex on a radio/audio producer.
- The background host consumes lifecycle notifications through Windows events.
  It does not poll state/stop files. Setup tools query `xone-background --status`
  and stop the owner with `--stop` using the same memory/events.

Transmit capacity includes unconsumed completions: at most 256 reliable requests
and 64 media requests, with at most four queued media requests per peer. Reliable
overflow is an explicit failure. Media that cannot be admitted or expires before
submission receives an expiry completion. A successful completion requires actual
physical USB completion, not acceptance into a queue.

Cancellation tokens belong to USB requests; unlink, reimport, alternate-setting
changes and detach cancel stale queued/in-flight work. RF epochs additionally
protect reused WCIDs from an old completion. Cancelled OVERLAPPED operations are
drained before freeing their buffers or closing the WinUSB handle.

Startup/teardown MT76 control operations remain synchronous and serialized with
physical OUT. This change isolates normal native GIP streaming waits; it does not
make firmware initialization or every lifecycle command asynchronous. This is not
yet a demonstrated fix for audible micro interruptions.

## Files and compatibility

Configuration and firmware are read during startup. Newly learned pairing
metadata stays in RAM during operation and is saved after radio shutdown. Runtime
state and counters disappear on process exit. Installer/removal results remain
on disk; legacy Python/debug sessions may explicitly request file captures.

`Build-Installer.ps1` keeps diagnostic journals compiled out by default. Explicit
development `--status-file`/`--stop-file` remain supported, but the native host
does not pass them. Legacy installed versions retain their file-based controls;
the setup helpers select the memory protocol only when the built runtime provides
its `outputs/runtime-capabilities.json` marker. That marker is static build
metadata, not a periodically updated runtime state file.

Tests cover cross-process state and shutdown, slow-observer coalescing, request
bounds, priority/expiry, cancellation, real completion timing and reused RF slots.
The isolated query/stop integration test asserts that existing file contents and
timestamps remain unchanged and never opens a physical adapter.
