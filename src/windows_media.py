# SPDX-License-Identifier: GPL-2.0-or-later
"""Scoped MMCSS registration for the Windows audio event-loop thread."""
import ctypes
import sys


class MediaScheduling:
    def __init__(self, enabled=True):
        self.enabled = enabled
        self.handle = None
        self.api = None
        self.error = 0

    def __enter__(self):
        if not self.enabled or sys.platform != 'win32':
            return self
        from ctypes import wintypes
        try:
            self.api = ctypes.WinDLL('avrt.dll', use_last_error=True)
            self.api.AvSetMmThreadCharacteristicsW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
            self.api.AvSetMmThreadCharacteristicsW.restype = wintypes.HANDLE
            self.api.AvRevertMmThreadCharacteristics.argtypes = [wintypes.HANDLE]
            self.api.AvRevertMmThreadCharacteristics.restype = wintypes.BOOL
            index = wintypes.DWORD()
            self.handle = self.api.AvSetMmThreadCharacteristicsW('Pro Audio', ctypes.byref(index))
            if not self.handle:
                self.error = ctypes.get_last_error()
        except OSError as error:
            self.error = error.winerror or 1
        return self

    def __exit__(self, *unused):
        if self.handle:
            self.api.AvRevertMmThreadCharacteristics(self.handle)
            self.handle = None
