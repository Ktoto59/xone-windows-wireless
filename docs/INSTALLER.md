# Automatic C++ installer

The alpha.4 development package installs three executables: `xone-background.exe`,
`xone-probe-cpp.exe` and the installation-only `xone-bind.exe`. Python, the control
panel, synthetic GIP probes and packet-test executables are not shipped as runtime.

Setup performs dependency preparation and adapter binding with Windows UAC, then
registers a limited interactive-user sign-in task. The host is a C++ Windows-subsystem
executable with no visible window. An invisible event receiver handles device and
power notifications; it does not present UI or an overlay. Ordinary runtime starts
only C++ radio and USBip client processes. No elevated scheduled task is registered.

Three model slots are created automatically. RF announcements select the tested
model's USB/GIP profile; physical addresses are saved only in local configuration.
The old headset's RF product 0B31 uses its wired 0B17 profile. Unknown models are
not forwarded to a captured profile, and a second simultaneous device of one
model cannot replace the slot's active peer.

Normal radio operation uses `--quiet`. Transport status publication contains
connection ownership/generation changes rather than continuously increasing packet
counters. The background host writes status only on changes, owns its child radio
through a Windows job, and detaches only USBip ports it imported. Detailed radio
status remains available with an explicit development `--verbose` launch.

Audio routing matches the Windows audio device's USB hardware identity rather than
its display name. The newest connected headset is selected for all default playback
and capture roles. Saved defaults are restored only when still owned by this host;
a later user-selected default is not overwritten during restoration. Application-
specific pinned outputs are outside this policy.

The source checkout's prior direct-GIP audio experiments are not included in the
application runtime. The new installer/runtime is a prerelease; software checks and
single-PC hardware observations do not establish cross-PC or long-term reliability.

Build: `scripts/Build-Appliance.cmd`, then
`scripts/Build-Installer.ps1 -IsccPath <Inno Setup ISCC.exe>`.