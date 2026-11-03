# SPDX-License-Identifier: GPL-2.0-or-later
"""
Caliper: a measuring tool whose measurements stay visible in every mode,
with draggable handles, Ctrl snapping and X/Y/Z axis locking.
"""

bl_info = {
    "name": "Caliper Measure",
    "author": "HB",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Toolbar > Caliper, Sidebar > Caliper",
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
