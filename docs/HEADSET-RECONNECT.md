# Headset 2021 wireless-mode compatibility

In development, a 2021 headset could work during a session but require pairing
again after its own power cycle. Keeping the original channel and scanning idle
channels did not restore ordinary reconnect in those observations.

Windows' wired GIP path sends a parent POWER command (`0x05`), mode 6, with this
observed 15-byte payload:

```text
06 00 00 00 00 00 00 55 53 00 00 00 00 00 00
```

The 2021 connector consumes only that exact initialization form and the known
zero-parameter equivalent. It matches an unchunked parent command without a GIP
ACK request. Unknown variants, child commands, ACK-requesting forms and the other
models are forwarded normally. A consumed command is counted locally; the bridge
does not claim it was transmitted over RF.

With this filter enabled, re-pairing followed by a physical off/on cycle returned
the headset without another pair request in one independently logged cycle.
Later short joint-device checks also succeeded. This is evidence for a narrow
compatibility workaround, not proof of the headset's NVM layout or a general
pairing-memory specification.

Tests check exact matching, unrelated packet preservation, real RF transmit
errors and model isolation. The C++ connector enables the workaround only for
the 2021 model (`0B17`, RF alias `0B31`). The 2024 connector has its own behavior.
See [AUTO-PAIRING.md](AUTO-PAIRING.md) for normal use.
