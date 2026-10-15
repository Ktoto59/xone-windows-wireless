# SPDX-License-Identifier: GPL-2.0-or-later
from .base import ConnectorConfig,DeviceConnector


class Headset2024Connector(DeviceConnector):
    config=ConnectorConfig(name='headset-2024',usb_pid=0x0b32,radio_pids=(0x0b32,),
                           profile='headset-0b32-usb.json',audio=True,render_max_age=.064,
                           independent_clock=False,preserve_wireless_mode=False)
