# Build the automatic C++ installer

See [INSTALL.md](INSTALL.md) for the end-user workflow.

The runtime package contains three executables (`xone-background.exe`,
`xone-probe-cpp.exe`, installation-only `xone-bind.exe`), setup/removal scripts,
licenses and short documentation. Device profiles are compiled into the radio
binary. Python, research probes, source trees, firmware, driver backups, physical
addresses and captured USB traffic are excluded from the installed payload.

Setup runs adapter/dependency preparation with UAC, then registers a limited
interactive-user sign-in task for the C++ host. Its radio child is owned through
a Windows job. Runtime USBip ports and process identities belong to that host;
removal does not kill arbitrary Python processes or detach unrelated USB devices.

## Build

Use Windows x64, Visual Studio C++ Build Tools, a Windows SDK and Inno Setup:

```powershell
# Build the installation/removal helper if outputs/xone-bind.exe is absent.
.\scripts\build.cmd
.\scripts\Build-Installer.ps1 -IsccPath 'C:\path\to\ISCC.exe'
```

The builder compiles candidate binaries into `outputs/appliance-candidate` and
leaves a running copy untouched. `-SkipBuild -LauncherDir <directory>` packages
explicit, already tested binaries. Use a fresh version/staging directory; the
builder refuses to overwrite an existing package stage.

Artifacts in `dist/` are Setup EXE, minimal runtime ZIP and corresponding source
ZIP. Source packaging includes tracked and new non-ignored source files, including
C++ connector headers. Each ZIP has a hash manifest with base commit and whether
the working tree was modified; a modified checkout is not represented as an exact
build of its base commit. The source ZIP must accompany distribution of this GPL
candidate. No firmware or Microsoft driver binaries are redistributed.

## Validation boundaries

Software/loopback tests and live metadata-access fault injection are described in
[cpp-bridge.md](cpp-bridge.md). Installer compilation, payload inspection and an
isolated registration/removal smoke test can be performed without rebinding the
working adapter. Such a smoke test does not verify hardware preparation or actual
stock-driver restoration; those require a separate hardware installation test.
