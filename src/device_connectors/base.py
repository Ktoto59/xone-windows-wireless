# SPDX-License-Identifier: GPL-2.0-or-later
"""Immutable model policy; each worker owns its backend, profile and queues."""
from dataclasses import dataclass
from usbip_reference_server import transport_profile, radio_endpoints


@dataclass(frozen=True)
class ConnectorConfig:
    name: str
    usb_pid: int
    radio_pids: tuple
    profile: str
    audio: bool
    render_max_age: float
    independent_clock: bool
    preserve_wireless_mode: bool


class DeviceConnector:
    config: ConnectorConfig

    def load_profile(self,root):
        return transport_profile(root/'driver/profiles'/self.config.profile,self.config.audio)

    def configure(self,backend,profile,preserve_old_wireless_mode=True):
        config=self.config
        if profile.pid!=config.usb_pid:
            raise ValueError('Connector/profile identity mismatch')
        backend.interrupt_endpoints,backend.audio_endpoints=radio_endpoints(profile)
        backend.audio_bridge=backend.audio_probe=config.audio
        backend.render_max_age=config.render_max_age
        backend.independent_clock=config.independent_clock
        backend.preserve_wireless_mode=config.preserve_wireless_mode and preserve_old_wireless_mode
