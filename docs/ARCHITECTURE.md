# Architecture

```text
Physical Xbox Wireless Adapter (WinUSB)
    invisible C++ background host (sign-in startup, recovery, audio defaults)
        xone-derived MT76 radio transport and client/session manager (C++)
            per-model C++ USB/IP servers and radio/USB audio adaptation
                signed usbip-win2 virtual USB controller
                    Windows dc1-controller / xboxgip / usbaudio
                        XInput, WGI, Windows audio, Xbox Accessories
```

The radio/protocol implementation is user mode. Existing signed drivers provide
WinUSB, the virtual USB bus and the Windows Xbox/audio stack. This is not yet a
native wireless NDIS miniport or a replacement kernel implementation of Xbox GIP.

The installation/reliability target and the feasibility checks for a direct
adapter driver are specified in [PRODUCTION.md](PRODUCTION.md).

## Device paths

Each configured MAC has an independent broker, USB instance, generation and PCM
queues. `RawGipRouter` checks WCID ownership, isolates equal completion tokens and
rejects stale requests when a WCID is reused. USB/GIP peers bypass local GIP
identification and authentication processing: the wired Windows GIP stack owns
their commands. No fake radio keys are installed to claim completed authentication.
Wireless auth v3 is not implemented, and raw peers must not be assumed to have
the stock wireless authentication/encryption guarantees.

The alternative native-gip/ViGEm outputs authenticate supported v1/v2 radio
clients locally and publish decoded input. They are separate backends, with
different identity, audio and Accessories behavior.

## GIP packet adaptation

Controller metadata radio fragments can reach 128 bytes while the USB interrupt
endpoint is 64 bytes. The bridge divides existing GIP chunks into USB-sized chunks,
preserving sequence, logical offsets and total length. Only the final subchunk
requests the original ACK, retaining the radio acknowledgement byte count.
Oversized unfragmented transfers and unsupported interfaces fail explicitly.

## Audio timing

For the observed formats, radio uses 8-ms blocks: 24-kHz mono capture and 48-kHz
stereo render. The Windows USB path uses eight 1-ms GIP/ISO fragments. Capture PCM
is split without resampling, render PCM is combined without lossy encoding.

In AudioBridge mode the virtual USB/IP speed is presented as High-Speed to
preserve interval 1 through usbip-win2. The captured reference descriptors remain
unchanged. This fixes the observed one-of-eight fragment loss in the Windows
driver's packing. Render sends one 1536-byte block per headset request/credit;
credits and buffers are bounded, generation changes clear queues.

Clean short left/right tones were confirmed on both headset revisions. The newer
headset's microphone signal was measured without saving voice PCM. Prolonged
music, speech quality and every simultaneous-use scenario are not certified.

## Lifecycle and status

The selected development path is the [automatic background bridge](BACKGROUND-MODE.md)
on existing signed drivers. It adds per-user sign-in startup and bounded recovery;
the custom kernel experiment is not required or selected for deployment.

The launcher imports a virtual USB device only after its radio peer associates,
detaches its own port on generation/loss, and cleans up its own processes on stop.
It does not detach unrelated USB/IP imports. The frozen alpha.2 manual launcher
has no supervisor; its CLI defaults to a bounded session. Development background
mode uses unlimited sessions and retries after worker exit/adapter return.

The C++ session manager also recovers a configured raw USB/GIP peer whose initial
association remains silent for five seconds. It removes only that firmware slot,
invalidates its USB generation, and sends association again, up to three times.
Receiving valid GIP stops this startup timer, so ordinary inactivity does not
reset a working peer. Later known-peer association requests refresh the session
after one-second debounce. One new-headset warm restart recovered on the first
attempt with clean user-confirmed audio, without physical USB reset; broader
reconnection reliability remains pending.

In the default USB-only route, automatic discovery advertises reconnect availability
after manual pairing closes. The alpha.4 automatic catalog identifies supported
model announcements and claims a free model slot; saved addresses seed reconnect
discovery. Unknown pairing requests still require the physical pairing button.
Scanning continues with an empty table, pauses for a ten-second enrollment grace
after a pair response and stays paused with any occupied peer slot. Shutdown closes
discovery. Manual enrollment and automatic discovery are separate policy states.

Status snapshots use atomic publication and bounded sharing-error retries so a
temporary file lock does not stop all devices. Physical battery status is observed
without rewriting GIP. Xbox Accessories still displays a wired/Connected label;
that UI limitation is separate from receiving battery data in XInput/WGI.

Normal application mode disables text/packet logging and publishes only changed
connection/ownership/error state. Historical Python and direct GIP tools remain
research sources; they are not resident parts of the alpha.4 application.

## Future wireless driver

Local analysis found an NDIS wireless binding and Xbox-specific OIDs in the Windows
GIP driver. A compatible transport might remove virtual USB presentation, but that
contract and the newer headset's authentication require further work. Moving this
code into a `.sys` file alone does not establish native Accessories compatibility.

The separate [native bootstrap](../driver/native/README.md) builds an x64 NDIS/WDM
`.sys` with direct USB transfers and the MT76 radio initialization code. Its GIP
data path is incomplete; it has not been loaded and is excluded from the installer.
