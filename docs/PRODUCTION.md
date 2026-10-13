# Production target and native driver feasibility

Status: design requirements, not a production reliability certification.
The selected delivery path is the [automatic user-session bridge](BACKGROUND-MODE.md)
on existing signed drivers. The custom kernel/signing path is not selected.
Alpha.3 includes the background bridge, audio scheduling and limited peer recovery;
the alpha.2 assets remain unchanged. Clean playback and individual old/new-headset
warm restarts were confirmed on one development PC. Broader lifetime, repeated
restarts and behavior on other computers remain unvalidated.

## User experience

One Windows x64 installer prepares the supported adapter and its dependencies.
After ordinary first pairing, powering on a device is enough. Reboots, sign-out,
adapter hotplug and sleep must not require opening the control panel, editing
JSON, starting scripts, repeating UAC or pairing again. No session expiry is
permitted. Windows audio, XInput and Xbox Accessories must work with the devices'
identities. Being installed as a driver does not by itself establish reliability.

## Driver installed directly on the adapter

A custom adapter driver is technically possible. Its package would contain a
.sys implementation, .inf installation description, signed .cat and supporting
files. The INF matches supported adapter hardware IDs. The package can be selected
manually in Device Manager or installed by Setup. Manual selection does not exempt
the driver from Windows code-signing requirements.

The existing `driver/xone-winusb.inf` describes a future WinUSB binding, not a
custom radio/GIP kernel driver. It is not an installable signed package.

A native wireless implementation must handle USB transfer cancellation, adapter
firmware loading, radio setup, association, reconnection, authentication and
transport security. It must provide the transport contract expected by Windows'
wireless Xbox GIP binding: identification, input/output, settings and audio. It
also needs per-peer fault isolation, audio clock synchronization and correct PnP
and power lifecycle behavior.

Local analysis found an NDIS wireless binding and Xbox-specific OIDs in the stock
GIP stack. Their complete contract and compatibility across Windows builds remain
unproven. A generic NDIS miniport or KMDF child device is not sufficient evidence
of Xbox Accessories compatibility.

There is a separate authentication obstacle. On the examined Windows binaries
version 10.0.26100.8972, the newer headset answered with authentication version 3,
while the examined DevAuthE dispatcher supported versions 1 and 2. Stock reconnect
cycles coincided with rejection of that response. This finding is specific to
those binaries, not every Windows release. Our local radio authentication also
lacks version 3. Replacing only the adapter transport while retaining that path
would not resolve the observed rejection. Relabeling a version byte does not
implement the missing handshake.

The prototype's wired GIP binding has different authentication behavior. Its
success does not demonstrate native wireless authentication, encryption or
battery presentation. No modification of Windows binaries is proposed here.

### Native feasibility gate

1. Specify the observed wireless GIP transport contract and its version checks.
2. Build a minimal WDK transport experiment exposing one genuine controller to
   stock GIP without virtual USB. Verify XInput, Accessories and device teardown
   using actual Windows device topology.
3. Establish a complete compatible authentication path for the newer headset;
   prove repeated connection through the intended wireless binding.
4. Verify audio clocking and cancellation before adding concurrent devices.

A compiling .sys, an accepted INF or a Device Manager name is not success. Kernel
experiments need a recovery/debug environment. No test-signing or Secure Boot
setting has been changed for this design work.

## Installed bridge alternative

An automatic Windows service can supply the same installation/startup experience
using the present virtual USB/GIP path. This is a shorter route to that experience;
it still uses virtual USB. Accessories' wired battery label is a separate issue.

Move ownership of radio, per-peer output and recovery into a native host. Merely
registering the current PowerShell launcher as a service does not fix its mapping
or recovery limitations. Automatic model provisioning must replace manual MAC/
profile registration, with stable distinct identities for multiple units per model.
Keep immutable binaries in Program Files and restricted private state/recovery
data in ProgramData. Verify service-account/device permissions during development.
Normal control-panel operation must not request elevation on every start.

## Lifecycle requirements for either path

- Wait normally when the adapter is absent; initialize on arrival.
- On removal/failure, invalidate the transport generation, cancel I/O and finish
  releasing owned resources before a new owner initializes the adapter.
- Ignore stale completions; never detach unrelated devices or USB/IP imports.
- Retry transient failures with bounded backoff; report permanent errors clearly.
- Rebuild valid I/O after resume without losing pairing.
- Isolate peer failures; bounded audio overruns must not kill command transport.
- Distinguish running host, ready adapter, connected peer, GIP and working audio.
- Keep private identifiers, keys and PCM out of public routine diagnostics.

## Release acceptance gates

| Area | Required evidence |
| --- | --- |
| Installation | Clean supported Windows machine, one Setup flow, no manual runtime/profile edits; rollback on failure |
| Startup | Reboot and sign-out/sign-in with automatic operation and no expiry timer |
| Power/PnP | Repeated hotplug, port changes, sleep/resume and device power cycles without repair or repeated pairing |
| Hardware | Both headset revisions and Series controller separately and together; multiple units per model |
| Accessories | Identity, settings read/write and battery behavior checked against physical devices |
| Audio | Long music/speech/microphone operation; measured drift, queue depth, dropouts and latency |
| Endurance | Initial 72-hour mixed-use soak with fault injection and stable memory/handle counts, then wider testing |
| Recovery | Interrupted install, upgrade, uninstall and stock restoration preserve unrelated devices |
| Distribution | Kernel packages meet Windows signing policy without development-only security settings |

Short tones and software tests do not satisfy these gates. A 72-hour soak is an
initial gate, not a guarantee of years of operation. Firmware-update forwarding
remains unavailable until independently implemented and validated.

## Microsoft references

- [Driver package components](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/components-of-a-driver-package)
- [INF hardware models and IDs](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-models-section)
- [Kernel driver signing policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-)
- [Service Control Manager](https://learn.microsoft.com/en-us/windows/win32/services/service-control-manager)
- [Device notifications](https://learn.microsoft.com/en-us/windows/win32/devio/device-notifications)
