# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty, StringProperty
from bpy.types import PropertyGroup
from mathutils import Matrix


def _redraw(self, context):
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


class HBWorkplaneSettings(PropertyGroup):
    enabled: BoolProperty(name="Auto Constraints", default=True, update=_redraw,
                          description="Constrain Move/Rotate to the workplane and show its grid")
    mode: EnumProperty(name="Workplane", items=(
        ('AUTO', "Automatic", "World plane (XY, XZ or YZ) that faces the view"),
        ('CUSTOM', "Custom", "Plane set from a selection, face under the mouse or the 3D cursor"),
    ), default='AUTO', update=_redraw)
    matrix: FloatVectorProperty(name="Workplane Matrix", size=(4, 4), subtype='MATRIX',
                                default=((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1)), update=_redraw)

    axis_bias: FloatProperty(name="Ground Bias", min=0.0, max=1.0, default=0.7, update=_redraw,
                             description="Automatic mode: how strongly the ground (XY) plane is preferred")
    constrain_move: BoolProperty(name="Move", default=True,
                                 description="G moves along the workplane")
    constrain_rotate: BoolProperty(name="Rotate", default=True,
                                   description="R rotates around the workplane normal")
    sync_orientation: BoolProperty(name="Set Transform Orientation", default=True,
                                   description="Also make the workplane the active transform orientation "
                                               "(gizmo and typed X/Y/Z follow it)")
    prev_orientation: StringProperty(default='GLOBAL', options={'HIDDEN'})

    show_grid: BoolProperty(name="Show Grid", default=True, update=_redraw)
    show_auto_grid: BoolProperty(name="Grid in Automatic Mode", default=True, update=_redraw)
    grid_opacity: FloatProperty(name="Opacity", min=0.0, max=1.0, default=0.35, subtype='FACTOR',
                                update=_redraw)
    grid_lines: IntProperty(name="Grid Size", description="Grid lines on each side of the center",
                            min=4, max=100, default=20, update=_redraw)
    grid_color: FloatVectorProperty(name="Grid Color", subtype='COLOR', size=3, min=0, max=1,
                                    default=(0.85, 0.85, 0.85), update=_redraw)
    occlude: BoolProperty(name="Hide Behind Geometry", default=False, update=_redraw)

    constrain_extrude_vert: BoolProperty(name="Vert/Edge", default=True,
                                         description="Extruded vertices/edges move along the workplane")
    constrain_extrude_face: BoolProperty(name="Face", default=False,
                                         description="Extruded faces move along the workplane normal")
    grid_snap: BoolProperty(name="Grid Snap", default=True,
                            description="Move/Extrude snap to the workplane grid (hold Ctrl to move freely). "
                                        "Object Mode and Mesh Edit Mode")
    spacing_mode: EnumProperty(name="Spacing", items=(
        ('ADAPTIVE', "Adaptive", "Grid spacing follows the zoom level (1/2/5 steps)"),
        ('FIXED', "Fixed", "Use a fixed grid spacing"),
    ), default='ADAPTIVE', update=_redraw)
    spacing: FloatProperty(name="Grid Spacing", min=1e-6, default=0.1, subtype='DISTANCE', unit='LENGTH',
                           update=_redraw)
    subdivisions: IntProperty(name="Snap Subdivisions", min=1, max=10, default=1,
                              description="Snap to this many divisions of each grid square")


def register():
    bpy.utils.register_class(HBWorkplaneSettings)
    bpy.types.Scene.hb_workplane = bpy.props.PointerProperty(type=HBWorkplaneSettings)


def unregister():
    del bpy.types.Scene.hb_workplane
    bpy.utils.unregister_class(HBWorkplaneSettings)
