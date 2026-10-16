# Build the automatic C++ installer

See [INSTALL.md](INSTALL.md) for the end-user workflow.

The runtime package contains three executables (`xone-background.exe`,
`xone-probe-cpp.exe`, installation-only `xone-bind.exe`), setup/removal scripts,
licenses and short documentation. Device profiles are compiled into the radio
binary. Python, research probes, source trees, firmware, driver backups, physical
addresses and captured USB traffic are excluded from the installed payload.

On a fresh Windows 11 x64 installation, Setup downloads a pinned Microsoft CAB
for the selected adapter model, verifies its catalog, installs the missing stock
driver, saves recovery metadata and selects inbox WinUSB. Verified adapter RAM
firmware and the pinned signed USBip client/virtual bus are prepared automatically.
An incomplete installation of the tested USBip version is repaired by Setup.
The application uses a static C/C++ runtime and Windows inbox GIP/audio drivers;
Python, Visual C++ Redistributable, Accessories and build tools are not prerequisites.
Internet access and UAC approval are required for missing downloaded components.
A Windows-requested reboot is reported by Setup; the registered host starts at
sign-in after reboot, without requiring manual dependency commands.

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

The builder compiles release binaries into `outputs/appliance-release` with
`XONE_DISABLE_DIAGNOSTICS`: no C++ journal writer, anomaly files, journal health
files or packet tracing, even when diagnostic environment overrides are present.
Maintenance results/transcripts and persistent configuration/recovery backups
remain. The native runtime uses shared memory and Windows events for operational
state and stop commands; it creates no periodic state files. See [RUNTIME-IO.md](RUNTIME-IO.md).
Use `-EnableDiagnostics` to build a diagnostic installer into
`outputs/appliance-candidate`; ordinary `Build-Appliance.cmd` development builds
retain diagnostics. `Build-Appliance.cmd --no-diagnostics <output-directory>`
can also build a separate release candidate without replacing a running copy.
`-SkipBuild -LauncherDir <directory>` packages
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

Runtime C++ targets define `NDEBUG` for release builds, so library assertion
messages cannot embed the local build directory. The packager rejects personal
profile paths or user SID literals in executable strings before copying them.
This privacy check does not replace runtime protocol/bounds validation.
