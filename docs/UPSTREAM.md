# Upstream attribution and licenses

- [medusalix/xone](https://github.com/medusalix/xone), pinned base
  `3484f603484782dd7551c64e5a33fc602b127051`, GPL-2.0-or-later. Imported MT76,
  protocol/session and authentication sources retain their original notices.
  Windows adaptations and subsequent local fixes are present in this repository;
  do not overwrite them with a plain upstream import.
- Pairing/channel and radio startup work was informed by
  [dlundqvist/xone](https://github.com/dlundqvist/xone), including revision
  `f2aa9fe01103d7600553b505b298ff0bd47ff280`. Linux workqueue/URB infrastructure is
  not the Windows runtime. Local pairing and lifecycle policy is implemented here.
- nlohmann/json 3.12.0, MIT: `third_party/json/LICENSE.MIT`. The retained header's
  SHA-256 is `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.
- ViGEmClient headers, MIT, revision
  `b66d02d57e32cc8595369c53418b843e958649b4`: `third_party/vigem/LICENSE`.
  The legacy backend is source research; no ViGEm DLL/installer is bundled.
- [usbip-win2](https://github.com/vadimgrn/usbip-win2) 0.9.8.1 is a separately
  installed signed dependency. Its installer/kernel drivers are downloaded and
  verified on the user's PC, not bundled as project release assets.
- Legacy synthetic GIP ABI research referenced
  [GipSyntheticEx](https://github.com/InvoxiPlayGames/GipSyntheticEx) and
  [FestivalInstrumentMapper](https://github.com/InvoxiPlayGames/FestivalInstrumentMapper).
  Their hook implementation is not bundled or used by the USBIP runtime.

Project source and user-mode tools are GPL-2.0-or-later; see the root LICENSE.
Corresponding source is supplied as a separate release archive and Git tag.
Third-party notices remain with the relevant source/header and installed payload.
Inno Setup builds the installer and retains its runtime notices.

Python is a development/reference dependency, not bundled in the current Setup.
Microsoft firmware, system DLL/SYS files, PDBs, driver-store backups, test
certificates, local data and packet/audio captures are not distributed. Public
USB profiles contain model descriptors with physical serials omitted. The project
is independent of Microsoft/Xbox; names identify compatibility targets.
