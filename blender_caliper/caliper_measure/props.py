# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatVectorProperty,
                       IntProperty, StringProperty)
from bpy.types import PropertyGroup


def _redraw(self, context):
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


class CaliperMeasure(PropertyGroup):
    name: StringProperty(name="Name", default="Measure")
    start: FloatVectorProperty(name="Start", size=3, subtype='TRANSLATION', unit='LENGTH', update=_redraw)
    end: FloatVectorProperty(name="End", size=3, subtype='TRANSLATION', unit='LENGTH', update=_redraw)
    visible: BoolProperty(name="Visible", default=True, update=_redraw)


UNIT_ITEMS = (
    ('SCENE', "Scene", "Use the scene's unit settings"),
    ('MILLIMETERS', "mm", ""),
    ('CENTIMETERS', "cm", ""),
    ('METERS', "m", ""),
    ('INCHES', "in", ""),
    ('FEET', "ft", ""),
)


class CaliperSettings(PropertyGroup):
    measures: CollectionProperty(type=CaliperMeasure)
    active_index: IntProperty(name="Active", default=0, update=_redraw)

    show: BoolProperty(name="Show Measurements", default=True, update=_redraw)
    show_ticks: BoolProperty(name="Ruler Ticks", default=True, update=_redraw)
    show_deltas: BoolProperty(name="Axis Deltas", description="Also show the X/Y/Z components",
                              default=False, update=_redraw)
    unit: EnumProperty(name="Unit", items=UNIT_ITEMS, default='SCENE', update=_redraw)
    precision: IntProperty(name="Decimals", default=2, min=0, max=6, update=_redraw)
    color: FloatVectorProperty(name="Color", subtype='COLOR', size=4, min=0.0, max=1.0,
                               default=(1.0, 0.55, 0.1, 1.0), update=_redraw)
    snap_radius: IntProperty(name="Snap Radius", description="Snap distance in pixels",
                             default=24, min=4, max=200)


classes = (CaliperMeasure, CaliperSettings)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.caliper = bpy.props.PointerProperty(type=CaliperSettings)


def unregister():
    del bpy.types.Scene.caliper
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
