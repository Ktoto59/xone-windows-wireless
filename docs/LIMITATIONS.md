# Compatibility and limitations

## Firmware updates: use a physical cable

**Firmware updates through the bridge are unsupported. Do not start an update
from Xbox Accessories while a headset or controller is connected through this
bridge. This includes over-the-air headset updates.** The bridge does not provide
or validate a firmware-update transport, rollback or update recovery. It does not
promise to block every update command, so an update button must not be treated as
permission to proceed.

1. Stop the bridge with the installed `scripts/Run-MaintainedBridge.ps1 -Stop`,
   or unplug the wireless adapter so the virtual device is absent.
2. Connect the physical headset/controller to the PC with a USB **data** cable.
3. Follow the manufacturer's update instructions in Xbox Accessories for the
   physical USB device. Do not select a leftover virtual bridge device.
4. After the update completes and the real USB device is disconnected, reconnect
   the adapter/start the bridge. Pair again if the manufacturer reset pairing.

Microsoft's [Xbox Wireless Headset setup guidance](https://support.xbox.com/en-US/help/hardware-network/accessories/setup-xbox-wireless-headset)
is the reference for the physical device. The restriction above is a limitation
of this project, not a claim that a real Xbox console cannot update wirelessly.

Setup's verified `FW_ACC_00U.bin` is adapter **RAM firmware** loaded during radio
startup. It is not a persistent headset/controller firmware update. Microsoft
firmware and driver binaries are acquired locally and are not release assets.

## Hardware and radio

Only the external `045E:02FE` adapter and the three model profiles listed in the
[README](../README.md) are covered. Different manufacturers, older controllers,
internal radios and other adapter revisions are unverified. Model profiles are
shared; they do not contain captured individual serials or radio addresses.

The host detects models automatically and maintains separate per-association
queues and USB identities. Eight slots are a software capacity, not validated
radio bandwidth for eight devices. Two physical headset revisions and one Series
controller have been tested together. Multiple physical units of the same model
have not been tested; separate same-model virtual instances have loopback coverage.

One physical radio stays on one channel while devices are connected. Discovery
cannot scan other channels without affecting an active link. The stock adapter
driver cannot simultaneously handle selected devices on an adapter owned by WinUSB.
Use a separate adapter if independent stock and bridge operation is required.

## Audio, Accessories and security

Each headset is a separate Windows audio device. Connecting one does not change
the default endpoint, and the bridge does not duplicate one application to both
headsets automatically. Select audio destinations in Windows or the application.
Controller audio-jack streaming is unsupported.

Xbox Accessories detection and some settings have worked; firmware updating and
full feature equivalence are not supported. A **Connected** battery label can
appear instead of a wireless battery indicator. Other firmware versions/settings
may behave differently.

USBip is still used between the user-mode server and the virtual USB bus.
Moving processing to C++ does not establish a measured end-to-end latency figure.
Wireless auth v3 and stock-equivalent authenticated/encrypted radio behavior are
not implemented or verified for the raw USB/GIP path. Do not infer stock security
properties from Windows loading its ordinary wired Xbox drivers.

## Installation and reliability

Windows 11 x64 and an interactive user session are required. The limited host
starts at sign-in, not as a system service before sign-in. Setup needs UAC for
driver/dependency preparation and may need internet access. First-time USBip
installation can restart USB hubs; Windows may request a reboot.

Project applications/Setup are unsigned. The bridge uses system WinUSB and signed
upstream USBip drivers; Developer Mode, TESTSIGNING and disabling Secure Boot are
not requirements. Do not install the historical kernel-driver experiment for
normal use.

Software tests and short hardware checks on one PC do not establish production
reliability. Fresh-PC hardware setup/restoration, long-duration playback/voice,
repeated lifecycle stress and broad suspend/resume remain incompletely tested.
Uninstall retains shared USBip and private recovery files and stops if it cannot
verify owned driver restoration. Keep recovery data until the adapter works again.
