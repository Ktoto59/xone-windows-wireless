# Upstream and licenses

The Setup EXE also contains the official Python 3.13.16 embeddable runtime under
the Python Software Foundation license and its included third-party notices.
`runtime/python/LICENSE.txt` is preserved during installation. See
[runtime provenance](INSTALLER.md#runtime-provenance) for the pinned archive.
Inno Setup builds the installer; its runtime notices are retained by the compiler.
USBip installers and Microsoft firmware are downloaded on the user's machine,
not bundled in the project release. All project GPL source is included in Setup.

- [medusalix/xone](https://github.com/medusalix/xone), base commit
  `3484f603484782dd7551c64e5a33fc602b127051`; GPL-2.0-or-later.
- Imported MT76 sources retain their original xone/mt76 copyright notices.
  `scripts/import-upstream.py` reproduces the transport import and compatibility
  adjustments using a local upstream checkout at the expected revision.
- Pairing beacon/channel changes are based on [dlundqvist/xone](https://github.com/dlundqvist/xone/tree/f2aa9fe01103d7600553b505b298ff0bd47ff280),
  commit `f2aa9fe01103d7600553b505b298ff0bd47ff280`.
  The development reconnect work also carries its cold RF startup patch
  (`e0ab753`), warm/cold MCU startup delays (`53894a4`, `525a27e`), normal traffic
  routing after warm reset (`98fcd81`), a 60-second startup pairing window
  (`3f07071`), button scan-timestamp refresh (`940cf37`), and invalid association /
  truncated loopback filtering (`ff37408`, `83bd053`).
  MT76 changes remain reproducible through `scripts/import-upstream.py` against
  the pinned medusalix base. Linux suspend/URB/workqueue infrastructure is not copied.
  The fork answers wireless `PAIR_REQ` without requiring an active pairing window.
  This port limits that automatic behavior to radio MACs explicitly configured for
  USB/GIP; unknown peers retain the manual pairing-button requirement. Re-enrollment
  removes only the matching peer's old WCID and virtual USB generation.
- Protocol, fragmentation, client/session handling and authentication derive from
  xone's bus/driver/auth modules, retaining attribution. Windows CNG implements
  RSA and ECDH; the ECDH secret byte-order conversion has a reference vector.
- ViGEmClient headers are from commit
  `b66d02d57e32cc8595369c53418b843e958649b4`, MIT license retained at
  `third_party/vigem/LICENSE`. No ViGEm client DLL or installer is bundled.
- The native synthetic backend is an independent implementation of the system
  XboxGipSynthetic export ABI. [GipSyntheticEx](https://github.com/InvoxiPlayGames/GipSyntheticEx)
  and [FestivalInstrumentMapper](https://github.com/InvoxiPlayGames/FestivalInstrumentMapper)
  were references; their hook implementation is not included.
- [usbip-win2](https://github.com/vadimgrn/usbip-win2) is a separately installed
  dependency. This project does not redistribute its installers or kernel drivers.
- Microsoft firmware, Xbox system DLLs/SYS files, public PDBs, driver-store backups and
  reverse-engineering dumps are not included. Public USB profiles contain generic
  model descriptors and synthetic serials, not the captured physical serials.

The project source and corresponding prebuilt user-mode tools are distributed
under GPL-2.0-or-later. The complete GPL v2 text is in the root `LICENSE` file.
Third-party notices and licenses remain with their sources/headers.
Microsoft and dependency names identify compatibility targets, not affiliation.
