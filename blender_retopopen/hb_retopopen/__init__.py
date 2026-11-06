# SPDX-License-Identifier: GPL-2.0-or-later
"""HB_RetopoPen: draw patch outlines on the surface and get quad patches."""

bl_info = {
    "name": "HB_RetopoPen",
    "author": "Boti Harko",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Toolbar > Retopo Pen",
    "description": "Draw a closed outline on the surface (snaps to any visible meshes) and get a quad patch",
    "category": "Mesh",
}

from . import pen


def register():
    pen.register()


def unregister():
    pen.unregister()
