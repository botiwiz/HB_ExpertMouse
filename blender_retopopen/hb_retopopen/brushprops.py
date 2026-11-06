# SPDX-License-Identifier: GPL-2.0-or-later
"""Retopo Brush settings (part of the scene's Retopo settings)."""

from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty

MODE_ITEMS = (('TWEAK', "Tweak", "Drag vertices with the mouse (they stay on the surface)"),
              ('RELAX', "Relax", "Even out the vertices under the brush (they stay on the surface)"))
FALLOFF_ITEMS = (('SMOOTH', "Smooth", ""), ('LINEAR', "Linear", ""), ('CONSTANT', "Constant", ""))


def brush_props():
    return dict(
        brush_mode=EnumProperty(name="Brush", items=MODE_ITEMS, default='TWEAK',
                                description="Drag: this mode, Shift+drag: the other one"),
        brush_radius=IntProperty(name="Radius", default=60, min=5, max=1000, subtype='PIXEL',
                                 description="Brush radius ([ and ] while the tool is active)"),
        brush_strength=FloatProperty(name="Strength", default=0.5, min=0.0, max=1.0, subtype='FACTOR'),
        brush_falloff=EnumProperty(name="Falloff", items=FALLOFF_ITEMS, default='SMOOTH'),
        brush_pin_border=BoolProperty(name="Pin Border", default=False,
                                      description="Don't move open-border vertices (otherwise relax slides them "
                                                  "along the border; corners always stay)"),
        brush_front_only=BoolProperty(name="Front Faces Only", default=True,
                                      description="Only affect vertices facing the view"),
    )
