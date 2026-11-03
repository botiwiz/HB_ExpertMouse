# SPDX-License-Identifier: GPL-2.0-or-later
import os

import bmesh
import bpy
from bpy.props import EnumProperty, IntProperty
from mathutils import Vector
from mathutils.geometry import intersect_point_line

from . import draw, geom

HIT_RADIUS = 12.0


def tag_redraw_all(context):
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


def hit_test(context, region, rv3d, mouse):
    """Return (index, 'START'|'END'|'BOTH') under the mouse, or None."""
    settings = context.scene.hb_caliper
    if not settings.show:
        return None
    mouse = Vector(mouse)
    best, best_d = None, HIT_RADIUS
    lines = []
    for i, m in enumerate(settings.measures):
        if not m.visible:
            continue
        a = geom.to_2d(region, rv3d, Vector(m.start))
        b = geom.to_2d(region, rv3d, Vector(m.end))
        if a is None or b is None:
            continue
        for which, p in (('END', b), ('START', a)):
            d = (p - mouse).length
            if d < best_d:
                best, best_d = (i, which), d
        lines.append((i, a, b))
    if best:
        return best
    for i, a, b in lines:
        if (b - a).length < 1e-3:
            continue
        pt, t = intersect_point_line(mouse.to_3d(), a.to_3d(), b.to_3d())
        if 0.0 < t < 1.0 and (pt.to_2d() - mouse).length < HIT_RADIUS * 0.6:
            return (i, 'BOTH')
    return None


def _mouse(event, region):
    return Vector((event.mouse_x - region.x, event.mouse_y - region.y))


class HB_CALIPER_OT_drag(bpy.types.Operator):
    """Create a measurement, or drag a measurement handle.
Ctrl: snap to vertices, edge midpoints, face centers, origins and other handles.
X/Y/Z: lock to an axis (Shift+X/Y/Z: lock to the plane)"""
    bl_idname = "hb_caliper.drag"
    bl_label = "HB_Caliper"
    bl_options = {'REGISTER', 'UNDO', 'BLOCKING', 'GRAB_CURSOR'}

    @classmethod
    def poll(cls, context):
        return context.area is not None and context.area.type == 'VIEW_3D' and context.region_data is not None

    # --- helpers -----------------------------------------------------------
    def _get(self, m, which):
        return Vector(m.start) if which == 'START' else Vector(m.end)

    def _set(self, m, which, co):
        if which == 'START':
            m.start = co
        else:
            m.end = co

    def _snap(self, context, mouse):
        if self.snaps is None:
            exclude = [(self.index, 'START'), (self.index, 'END')] if self.which == 'BOTH' else \
                [(self.index, self.which)]
            self.snaps = geom.SnapTargets(context, exclude)
        radius = context.scene.hb_caliper.snap_radius * geom.ui_scale(context)
        space = context.space_data
        xray = bool(space and getattr(space, "shading", None) and space.shading.show_xray)
        hit = self.snaps.nearest(self.region, self.rv3d, mouse, radius, xray)
        if hit:
            return hit
        loc = geom.surface_hit(context, self.region, self.rv3d, mouse)
        return (loc, 'SURFACE') if loc is not None else None

    def _solve(self, context, event):
        """Position for the grabbed point under the current mouse/constraint/snap."""
        mouse = _mouse(event, self.region)
        snap = self._snap(context, mouse) if event.ctrl else None
        self.drag["snap"] = snap[0] if snap else None
        self.drag["snap_kind"] = snap[1].lower() if snap else ""
        anchor = self.anchor
        if snap:
            return geom.constrain(anchor, snap[0], self.axis, self.plane)
        origin, direction = geom.mouse_ray(self.region, self.rv3d, mouse)
        if self.axis or self.plane:
            p = geom.ray_to_constraint(origin, direction, anchor, self.axis, self.plane)
            if p is not None:
                return p
        elif self.which != 'BOTH':
            # Like Blender's Measure tool: follow the surface under the mouse.
            p = geom.surface_hit(context, self.region, self.rv3d, mouse)
            if p is not None:
                return p
        return geom.on_view_plane(self.region, self.rv3d, mouse, self.grab_co)

    def _apply(self, context, event):
        m = context.scene.hb_caliper.measures[self.index]
        pos = self._solve(context, event)
        if self.which == 'BOTH':
            delta = pos - self.grab_co
            m.start = self.orig[0] + delta
            m.end = self.orig[1] + delta
        else:
            self._set(m, self.which, pos)
        self._header(context, m)
        tag_redraw_all(context)

    def _header(self, context, m):
        length = geom.format_length(context, (Vector(m.end) - Vector(m.start)).length)
        lock = ""
        if self.axis:
            lock = "  |  Locked: %s axis" % self.axis
        elif self.plane:
            lock = "  |  Locked: %s plane" % "".join(a for a in "XYZ" if a != self.plane)
        context.area.header_text_set(
            "HB_Caliper: %s%s  |  Ctrl: snap  |  X/Y/Z: axis, Shift+X/Y/Z: plane  |  "
            "LMB release: confirm  |  RMB/Esc: cancel" % (length, lock))

    def _finish(self, context):
        draw.state["drag"] = None
        draw.state["hover"] = None
        context.area.header_text_set(None)
        tag_redraw_all(context)

    # --- operator ----------------------------------------------------------
    def invoke(self, context, event):
        self.region = context.region
        self.rv3d = context.region_data
        settings = context.scene.hb_caliper
        mouse = _mouse(event, self.region)
        self.snaps = None
        self.axis = None
        self.plane = None
        self.creating = False

        hit = hit_test(context, self.region, self.rv3d, mouse)
        if hit:
            self.index, self.which = hit
            m = settings.measures[self.index]
            self.orig = (Vector(m.start), Vector(m.end))
            if self.which == 'BOTH':
                self.grab_co = geom.on_view_plane(self.region, self.rv3d, mouse,
                                                  (self.orig[0] + self.orig[1]) / 2)
                self.anchor = self.grab_co.copy()
            else:
                self.grab_co = self._get(m, self.which)
                self.anchor = self.grab_co.copy()
        else:
            # New measurement: first point from snap, surface, or the 3D cursor depth.
            self.index, self.which = -1, 'END'
            p = None
            if event.ctrl:
                self.which = 'START'  # for exclusion only
                snap = self._snap(context, mouse)
                self.snaps = None
                p = snap[0] if snap else None
            if p is None:
                p = geom.surface_hit(context, self.region, self.rv3d, mouse)
            if p is None:
                p = geom.on_view_plane(self.region, self.rv3d, mouse, context.scene.cursor.location)
            m = settings.measures.add()
            m.name = "Measure %d" % len(settings.measures)
            m.start = p
            m.end = p
            self.index = len(settings.measures) - 1
            self.which = 'END'
            self.creating = True
            self.orig = (p.copy(), p.copy())
            self.grab_co = p.copy()
            self.anchor = p.copy()

        settings.active_index = self.index
        self.drag = {"index": self.index, "which": self.which, "anchor": self.anchor,
                     "axis": None, "plane": None, "snap": None, "snap_kind": ""}
        draw.state["drag"] = self.drag
        self._header(context, settings.measures[self.index])
        context.window_manager.modal_handler_add(self)
        tag_redraw_all(context)
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        settings = context.scene.hb_caliper
        if self.index >= len(settings.measures):
            self._finish(context)
            return {'CANCELLED'}

        if event.type in {'MIDDLEMOUSE', 'WHEELUPMOUSE', 'WHEELDOWNMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM'}:
            return {'PASS_THROUGH'}

        if event.type in {'X', 'Y', 'Z'} and event.value == 'PRESS':
            ax = event.type
            if event.shift:
                self.plane = None if self.plane == ax else ax
                self.axis = None
            else:
                self.axis = None if self.axis == ax else ax
                self.plane = None
            self.drag["axis"], self.drag["plane"] = self.axis, self.plane
            self._apply(context, event)
            return {'RUNNING_MODAL'}

        if event.type in {'MOUSEMOVE', 'INBETWEEN_MOUSEMOVE', 'LEFT_CTRL', 'RIGHT_CTRL'}:
            self._apply(context, event)
            return {'RUNNING_MODAL'}

        if event.type in {'LEFTMOUSE', 'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'RELEASE' \
                or event.type in {'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'PRESS':
            m = settings.measures[self.index]
            if self.creating and (Vector(m.end) - Vector(m.start)).length < 1e-6:
                settings.measures.remove(self.index)
                settings.active_index = min(settings.active_index, len(settings.measures) - 1)
                self._finish(context)
                return {'CANCELLED'}
            self._finish(context)
            return {'FINISHED'}

        if event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            if self.creating:
                settings.measures.remove(self.index)
                settings.active_index = max(0, len(settings.measures) - 1)
            else:
                m = settings.measures[self.index]
                m.start, m.end = self.orig
            self._finish(context)
            return {'CANCELLED'}

        return {'RUNNING_MODAL'}


class HB_CALIPER_OT_delete_at_cursor(bpy.types.Operator):
    """Delete the measurement under the mouse"""
    bl_idname = "hb_caliper.delete_at_cursor"
    bl_label = "Delete Measurement Under Mouse"
    bl_options = {'REGISTER', 'UNDO'}

    def invoke(self, context, event):
        if context.region_data is None:
            return {'CANCELLED'}
        hit = hit_test(context, context.region, context.region_data, _mouse(event, context.region))
        if not hit:
            return {'CANCELLED'}
        settings = context.scene.hb_caliper
        settings.measures.remove(hit[0])
        settings.active_index = max(0, min(settings.active_index, len(settings.measures) - 1))
        tag_redraw_all(context)
        return {'FINISHED'}


class HB_CALIPER_OT_remove(bpy.types.Operator):
    """Remove a measurement"""
    bl_idname = "hb_caliper.remove"
    bl_label = "Remove Measurement"
    bl_options = {'REGISTER', 'UNDO'}

    index: IntProperty(default=-1)

    def execute(self, context):
        settings = context.scene.hb_caliper
        i = self.index if self.index >= 0 else settings.active_index
        if 0 <= i < len(settings.measures):
            settings.measures.remove(i)
            settings.active_index = max(0, min(settings.active_index, len(settings.measures) - 1))
        tag_redraw_all(context)
        return {'FINISHED'}


class HB_CALIPER_OT_clear(bpy.types.Operator):
    """Remove all measurements"""
    bl_idname = "hb_caliper.clear"
    bl_label = "Clear All Measurements"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        context.scene.hb_caliper.measures.clear()
        context.scene.hb_caliper.active_index = 0
        tag_redraw_all(context)
        return {'FINISHED'}


class HB_CALIPER_OT_swap(bpy.types.Operator):
    """Swap start and end of the active measurement"""
    bl_idname = "hb_caliper.swap"
    bl_label = "Swap Ends"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        s = context.scene.hb_caliper
        if 0 <= s.active_index < len(s.measures):
            m = s.measures[s.active_index]
            m.start, m.end = Vector(m.end), Vector(m.start)
        return {'FINISHED'}


class HB_CALIPER_OT_from_selection(bpy.types.Operator):
    """Add a measurement between two selected vertices (Edit Mode) or two selected objects"""
    bl_idname = "hb_caliper.from_selection"
    bl_label = "Measure Selection"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        pts = []
        if context.mode == 'EDIT_MESH':
            for obj in context.objects_in_mode:
                bm = bmesh.from_edit_mesh(obj.data)
                hist = [e for e in bm.select_history if isinstance(e, bmesh.types.BMVert)]
                sel = hist if len(hist) >= 2 else [v for v in bm.verts if v.select]
                pts += [obj.matrix_world @ v.co for v in sel]
        else:
            objs = list(context.selected_objects)
            if context.active_object in objs:  # active object last
                objs.remove(context.active_object)
                objs.append(context.active_object)
            pts = [o.matrix_world.translation.copy() for o in objs]
        if len(pts) != 2:
            self.report({'WARNING'}, "Select exactly 2 vertices (Edit Mode) or 2 objects")
            return {'CANCELLED'}
        s = context.scene.hb_caliper
        m = s.measures.add()
        m.name = "Measure %d" % len(s.measures)
        m.start, m.end = pts
        s.active_index = len(s.measures) - 1
        tag_redraw_all(context)
        return {'FINISHED'}


# ----------------------------------------------------------------------------
# Toolbar tool (one per mode so it is available everywhere)

TOOL_MODES = ('OBJECT', 'EDIT_MESH', 'EDIT_CURVE', 'EDIT_CURVES', 'EDIT_ARMATURE', 'EDIT_LATTICE',
              'EDIT_METABALL', 'EDIT_TEXT', 'POSE', 'PAINT_WEIGHT', 'PAINT_VERTEX', 'PAINT_TEXTURE')

_KEYMAP = (
    ("hb_caliper.drag", {"type": 'LEFTMOUSE', "value": 'PRESS'}, None),
    ("hb_caliper.drag", {"type": 'LEFTMOUSE', "value": 'PRESS', "ctrl": True}, None),
    ("hb_caliper.delete_at_cursor", {"type": 'DEL', "value": 'PRESS'}, None),
    ("hb_caliper.delete_at_cursor", {"type": 'BACK_SPACE', "value": 'PRESS'}, None),
)

_tools = []
# Absolute path (without .dat) to the custom toolbar icon.
_ICON = os.path.join(os.path.dirname(__file__), "icons", "hb_caliper.tool")


def _make_tool(mode):
    return type("HB_CALIPER_TL_%s" % mode.lower(), (bpy.types.WorkSpaceTool,), {
        "bl_space_type": 'VIEW_3D',
        "bl_context_mode": mode,
        "bl_idname": "hb_caliper.tool",
        "bl_label": "HB_Caliper",
        "bl_description": ("Measure distances that stay visible in every mode.\n"
                           "Drag: new measurement / move a handle (drag the line to move both).\n"
                           "Ctrl while dragging: snap.  X/Y/Z: lock axis.  Del: delete under mouse"),
        "bl_icon": _ICON,
        "bl_keymap": _KEYMAP,
    })


classes = (HB_CALIPER_OT_drag, HB_CALIPER_OT_delete_at_cursor, HB_CALIPER_OT_remove, HB_CALIPER_OT_clear,
           HB_CALIPER_OT_swap, HB_CALIPER_OT_from_selection)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    for mode in TOOL_MODES:
        tool = _make_tool(mode)
        try:
            bpy.utils.register_tool(tool, separator=True)  # own button at the bottom of the toolbar
        except Exception as ex:  # mode not available in this Blender version
            print("HB_Caliper: no tool for mode %s (%s)" % (mode, ex))
            continue
        _tools.append(tool)


def unregister():
    for tool in reversed(_tools):
        try:
            bpy.utils.unregister_tool(tool)
        except Exception:
            pass
    _tools.clear()
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
