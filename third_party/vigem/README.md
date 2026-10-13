# ViGEm wire headers

Copied without modification from nefarius/ViGEmClient at
`b66d02d57e32cc8595369c53418b843e958649b4`.
Source: https://github.com/nefarius/ViGEmClient
License: MIT, preserved in headers and `LICENSE`.

Only `Common.h` and `km/BusShared.h` are imported. The project implements its
own bounded overlapped IOCTL calls and joined/cancellable notification threads
in `src/vigem_output.cpp`; it does not use ViGEmClient's detached callbacks.
The signed ViGEmBus driver is an external dependency, not bundled in Git.
