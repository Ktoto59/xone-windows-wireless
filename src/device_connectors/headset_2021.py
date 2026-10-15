# SPDX-License-Identifier: GPL-2.0-or-later
from .base import ConnectorConfig,DeviceConnector


class Headset2021Connector(DeviceConnector):
    config=ConnectorConfig(name='headset-2021',usb_pid=0x0b17,radio_pids=(0x0b17,0x0b31),
                           profile='headset-0b17-usb.json',audio=True,render_max_age=.096,
                           independent_clock=True,preserve_wireless_mode=True)
