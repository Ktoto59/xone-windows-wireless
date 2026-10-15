# SPDX-License-Identifier: GPL-2.0-or-later
from .base import ConnectorConfig,DeviceConnector


class SeriesControllerConnector(DeviceConnector):
    config=ConnectorConfig(name='series-controller',usb_pid=0x0b12,radio_pids=(0x0b12,0x0b13),
                           profile='controller-0b12-usb.json',audio=False,render_max_age=.064,
                           independent_clock=False,preserve_wireless_mode=False)
