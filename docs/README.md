# Documentation

Documents describe the current checkout unless explicitly marked as historical
release notes. `main` is the stable/release channel; `experimental` contains
candidates that still require hardware validation. A candidate note does not mean
that its installer has been published.

## Install and use

| Guide | Purpose |
| --- | --- |
| [Installation](INSTALL.md) | Setup, upgrade and removal |
| [Limitations](LIMITATIONS.md) | Supported behavior and outstanding validation |
| [Troubleshooting](TROUBLESHOOTING.md) | Recovery and diagnostic collection |
| [Background operation](BACKGROUND-MODE.md) | Host ownership, startup, stop and retry |
| [Automatic pairing](AUTO-PAIRING.md) | Model detection and pairing |
| [Headset reconnect](HEADSET-RECONNECT.md) | Radio and virtual USB lifecycle |
| [Privacy](PRIVACY.md) | Local identities, private configuration and captures |

## Develop and investigate

| Guide | Purpose |
| --- | --- |
| [Development](DEVELOPMENT.md) | Branches, sessions, checks and repository map |
| [Scripts](../scripts/README.md) | Production entry points and research tools |
| [Installer build](INSTALLER.md) | Runtime/source packaging and isolated smoke test |
| [Architecture diagrams](ARCHITECTURE.md) | Component map, traffic/threads and connection sequence |
| [Runtime IO](RUNTIME-IO.md) | Threads, memory IPC, USB completion and cancellation |
| [C++ bridge](cpp-bridge.md) | Implementation details and validation boundaries |
| [USB/IP latency](USBIP-LATENCY.md) | Scheduling, buffering and expiry |
| [Native driver research](../driver/native/README.md) | Incomplete alternative kernel path |
| [Upstream](UPSTREAM.md) | Source provenance and licenses |

## Version history

See the [changelog](../CHANGELOG.md) for changes in the current branch.
Historical notes describe their specific versions, including older file-based
state handling; they are not instructions for the latest checkout.

- [alpha.5](releases/RELEASE-alpha.5.md)
- [alpha.5.1](releases/RELEASE-alpha.5.1.md)
- [alpha.5.2](releases/RELEASE-alpha.5.2.md)
- [alpha.5.4 test candidate](releases/RELEASE-alpha.5.4.md)
- [alpha.6](releases/RELEASE-alpha.6.md)
