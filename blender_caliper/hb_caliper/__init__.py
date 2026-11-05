# SPDX-License-Identifier: GPL-2.0-or-later
"""
HB_Caliper: a measuring tool whose measurements stay visible in every mode,
with draggable handles, Ctrl snapping and X/Y/Z axis locking.
"""

bl_info = {
    "name": "HB_Caliper",
    "author": "Boti Harko",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Toolbar (bottom) > HB_Caliper, Sidebar > HB_Caliper",
    "description": "Persistent measurements visible in all modes, with handles, snapping and axis lock",
    "category": "3D View",
}

from . import props, draw, ops, ui

_modules = (props, draw, ops, ui)


def register():
    for m in _modules:
        m.register()


def unregister():
    for m in reversed(_modules):
        m.unregister()
