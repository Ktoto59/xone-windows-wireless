# SPDX-License-Identifier: GPL-2.0-or-later
from .headset_2021 import Headset2021Connector
from .headset_2024 import Headset2024Connector
from .series_controller import SeriesControllerConnector


CONNECTOR_TYPES=(Headset2021Connector,Headset2024Connector,SeriesControllerConnector)
PROFILES={kind.config.usb_pid:kind.config.profile for kind in CONNECTOR_TYPES}
RADIO_MODELS={pid:kind.config.usb_pid for kind in CONNECTOR_TYPES for pid in kind.config.radio_pids}


def model(vendor,product):
    if vendor!=0x045e or product not in RADIO_MODELS:
        raise ValueError(f'Unsupported radio model {vendor:04x}:{product:04x}')
    return RADIO_MODELS[product]


def connectors():
    return {kind.config.usb_pid:kind() for kind in CONNECTOR_TYPES}
