# Automatic discovery and pairing

The installed C++ host requires no pre-filled addresses or per-device model list.
An RF announcement selects one of the supported connectors before a virtual USB
device is exported: headset 2021, headset 2024 or Series controller. Unknown models
are not assigned a captured profile.

For a first pairing, activate pairing on the device and briefly press the adapter
button. The pairing window lasts 30 seconds. Once paired, use ordinary power-on;
do not hold the device button long enough to start a new pairing operation.
The local registry is learned from associations and is not distributed.

Each association gets its own route, USB server, completion tokens, queues and
generation. Reusing a slot closes the previous import; it must not disconnect
neighboring devices. Virtual serials are derived locally from model and address.
The pool has eight slots, subject to the limits in [LIMITATIONS.md](LIMITATIONS.md).

A single adapter cannot scan other RF channels while preserving active streams.
Pair devices on its active channel. If pairing fails, stop/restart the bridge and
try with other devices off. Do not erase Windows driver/recovery information as a
pairing step. The 2021 headset's USB mode adaptation is described in
[HEADSET-RECONNECT.md](HEADSET-RECONNECT.md).

The older Python automatic launcher is a development reference; it is not needed
by the installer and is not part of normal runtime. See [DEVELOPMENT.md](DEVELOPMENT.md).
