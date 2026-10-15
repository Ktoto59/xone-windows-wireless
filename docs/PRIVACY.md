# Local data and public identifiers

No developer-specific device address, captured serial number, computer SID,
personal folder, certificate or recovery backup is needed to use this project.
The default configuration is automatic and contains no mapped device instances.

## Data generated on your PC

The host learns each device's radio address from its current association. It uses
that address for routing and a stable virtual USB serial, so reconnecting the same
device retains its Windows identity and two same-model devices remain distinct.
This is generated locally, not a hard-coded developer allowlist.

`config/*.local.json` can contain pairing addresses and installation ownership.
`work/` can contain USB instance IDs, process ownership, local paths, lifecycle
snapshots, verified RAM firmware and stock-driver recovery metadata. Normal audio
operation does not record microphone/playback samples or packet captures. Optional
development captures and crash diagnostics can expose additional local details.

These directories are ignored by Git and excluded from release packages. Keep
them private. Do not copy another computer's adapter binding backup into your
installation. Before attaching a diagnostic excerpt, remove addresses, serials,
usernames, SIDs and personal paths. Model IDs, Windows version and a description
of the failure usually suffice to start an issue.

## Technical constants that remain public

USB VID/PID pairs identify supported models, not an individual unit. Windows and
GIP interface GUIDs are protocol contracts. The installer's AppId identifies the
application for upgrades/uninstall; it is the same for every installation. License
attribution, upstream revisions and file hashes identify sources/artifacts. These
must not be replaced with random values during anonymization.

Tests use explicitly synthetic addresses/instance names. The public-content check
rejects private/generated files, personal path/SID literals, non-synthetic address
literals and Cyrillic text. It does not claim to detect every possible secret.
Older published releases retain their original source/version; current downloads
are linked from the README. No Git history is rewritten by ordinary repository
cleanup.
