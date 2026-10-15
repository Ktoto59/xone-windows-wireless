# Development

## Build and check

Use Windows x64, Visual Studio C++ Build Tools and a Windows SDK. Python 3.11+
is needed for reference/loopback tests, not for the installed runtime.

```powershell
.\scripts\build.cmd
.\scripts\Build-Appliance.cmd
.\scripts\Build-CppBridge.cmd
.\scripts\Test-CppBridge.cmd
python -m unittest discover -s tests -p '*tests.py'
powershell -NoProfile -File tests/device_config_tests.ps1
powershell -NoProfile -File tests/setup_helpers_tests.ps1
powershell -NoProfile -File tests/release_privacy_tests.ps1
python scripts/check-public-content.py
```

The appliance builder writes candidates under `outputs/appliance-candidate` and
does not replace a running copy. See [INSTALLER.md](INSTALLER.md) for installer,
source archive and isolated registration/removal smoke checks. Synthetic/loopback
tests do not access the physical adapter; hardware experiments do.

## Repository layout

| Directory | Purpose |
| --- | --- |
| `src/` | C++ radio/USBIP/runtime; Python reference; legacy research sources |
| `driver/profiles/` | Sanitized model USB descriptors, without physical serials |
| `scripts/` | Build/setup/removal, checks and developer-only experiments |
| `tests/` | Synthetic identities, protocol, transport and configuration checks |
| `installer/` | Inno Setup source |
| `third_party/` | Retained headers and licenses |
| `config/` | Automatic public template; private generated files are ignored |
| `docs/` | Current English documentation |

Legacy synthetic GIP/ViGEm, control-panel and Python launchers remain as research
references. The historical `driver/native` experiment is not the delivered driver
and must not be installed for normal use. Its signing/trust workflows are unrelated
to the current Setup. Research tools can change hardware state: use the normal
installer unless you are deliberately investigating those paths.

## Publication hygiene

Keep private data under ignored `work/`, `config/*.local.*` or `runtime/` paths.
Never commit firmware, captures, certificates, local bindings or real instance
addresses. Tests must use synthetic identities. Run the public-content check;
GitHub also runs it for pushes and pull requests. Do not randomize public protocol
GUIDs or model IDs: see [PRIVACY.md](PRIVACY.md).

Development machine observations are not a compatibility promise. Document exactly
which checks used synthetic RF, loopback sockets or physical hardware, and do not
present an isolated installer smoke test as a hardware restoration test.
