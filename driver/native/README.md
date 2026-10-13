# Native adapter driver — bootstrap stage

This starts a direct NDIS/WDM driver for the physical `045E:02FE` adapter.
Its implementation uses no WinUSB, Python, USB/IP or virtual USB device.
It is a separate development path, not a functional replacement release.

Implemented in source:

- NDIS 6.30 driver registration and lifecycle entry points.
- Direct USB URBs, descriptor selection and bulk/vendor transfers.
- Synchronous bootstrap timeouts; cancellation is joined before freeing requests.
- Kernel memory/delay/firmware adapters for the existing xone-derived MT76 C code,
  including the previously ported radio startup fixes.
- Firmware loading from `System32/drivers/FW_ACC_00U.bin` and radio initialization.
- Basic NDIS metadata and the observed adapter MAC-address query.
- Development INF matching the adapter, `ndisgip` upper binding,
  `xboxwirelessinput` lower binding and inbox `xboxgip` filter.

**Incomplete:** Xbox operational OIDs, association/status indications,
asynchronous receive, GIP send/receive, key installation and power recovery.
New-headset auth v3 remains unresolved. This build does not provide controller
input, headset audio or Accessories devices. Unsupported operations fail
explicitly; there are no fabricated successful sends or working-peer reports.

Next: wire one controller's GIP data path to the observed Windows wireless
transport contract. This directory does not recreate the Python USB/IP path.

## Build

Use Visual Studio C++ x64 Build Tools, Windows SDK 10.0.26100.0 and an extracted
matching Microsoft x64 WDK NuGet package. No system WDK installation is needed.

```powershell
.\scripts\Build-NativeDriver.ps1 -WdkRoot 'C:\path\to\extracted-wdk'
```

Outputs: `outputs/native/xone-native.sys` and `xone-native.inf`. The script only
compiles/links and copies the INF. No tests, driver installation, certificates,
boot-security changes, adapter switching or live-bridge changes are performed.

The first x64 binary was compiled on 2026-10-13. Subsequent INF validation found
and fixed the minimum Windows version decoration: this experiment targets Windows
11 x64, build 22000 or newer. InfVerif /w and catalog generation then passed.
The local .sys and .cat were separately test-signed for an explicitly authorized
development trial. This is not Microsoft release signing; normal builds remain
unsigned. No kernel load or hardware validation has occurred yet.

The privileged `scripts/Prepare-NativeTest.ps1` handles explicit local certificate
trust and boot-setting backup/rollback for that trial. It does not disable Secure
Boot, change HVCI, suspend BitLocker, reboot or install the adapter driver. It
rejects enabling TESTSIGNING while Secure Boot or unreviewed boot encryption
protection is active. Preparation state is private and excluded from Git.

The package remains excluded from the public installer. Compilation and package
validation do not prove kernel lifetime, radio or GIP compatibility.

Microsoft references: [NDIS registration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismregisterminiportdriver),
[NDIS bus device objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismgetdeviceproperty).
