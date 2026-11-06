# SPDX-License-Identifier: GPL-2.0-or-later
"""Retopo Brush: tweak (grab) and relax the retopo mesh, keeping it on the surface."""

import math

import bmesh
import bpy
import gpu
import numpy as np
from bpy.props import BoolProperty
from bpy_extras import view3d_utils
from gpu_extras.batch import batch_for_shader
from mathutils import Vector

from . import patch, pen, surface

from .brushprops import brush_props  # noqa: F401  (settings live with the other tool settings)


def _falloff(t, kind):
    t = min(1.0, max(0.0, t))
    if kind == 'CONSTANT':
        return 1.0
    if kind == 'LINEAR':
        return 1.0 - t
    return (1.0 - t * t) ** 2


# ----------------------------------------------------------------------------
# Brush circle (drawn while stroking, and while hovering via the tool's mouse-move key)

_cursor = {"pos": None, "radius": 0, "active": False, "handle": None}


def _draw_cursor():
    pos = _cursor["pos"]
    if pos is None or not _cursor["active"]:
        return
    r = _cursor["radius"]
    pts = [(pos[0] + r * math.cos(a), pos[1] + r * math.sin(a), 0.0)
           for a in [2 * math.pi * i / 48 for i in range(49)]]
    seg = []
    for a, b in zip(pts, pts[1:]):
        seg += [a, b]
    shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    region = bpy.context.region
    gpu.state.blend_set('ALPHA')
    batch = batch_for_shader(shader, 'LINES', {"pos": seg})
    shader.bind()
    shader.uniform_float("viewportSize", (region.width, region.height))
    shader.uniform_float("lineWidth", 1.5)
    shader.uniform_float("color", (1.0, 1.0, 1.0, 0.8))
    batch.draw(shader)
    gpu.state.blend_set('NONE')


class HB_RETOPO_OT_brush_hover(bpy.types.Operator):
    """(internal) track the mouse for the brush circle"""
    bl_idname = "hb_retopo.brush_hover"
    bl_label = "Retopo Brush Hover"
    bl_options = {'INTERNAL'}

    def invoke(self, context, event):
        _cursor["pos"] = (event.mouse_region_x, event.mouse_region_y)
        _cursor["radius"] = context.scene.hb_retopo.brush_radius
        _cursor["active"] = True
        if context.area:
            context.area.tag_redraw()
        return {'PASS_THROUGH'}


# ----------------------------------------------------------------------------
# The stroke

class HB_RETOPO_OT_brush(bpy.types.Operator):
    """Tweak (drag) or relax (Shift+drag) the retopo mesh under the brush; it stays on the surface"""
    bl_idname = "hb_retopo.brush"
    bl_label = "Retopo Brush"
    bl_options = {'REGISTER', 'UNDO', 'BLOCKING'}

    invert: BoolProperty(name="Other Mode", default=False, options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return context.area is not None and context.area.type == 'VIEW_3D' and context.mode in {'EDIT_MESH', 'OBJECT'}

    # --- helpers -------------------------------------------------------------------
    def _screen(self):
        """Region coordinates of all vertices (N,2) and an 'in front of the camera' mask."""
        m = np.array(self.rv3d.perspective_matrix)
        co = self._world()
        clip = co @ m[:3, :3].T + m[:3, 3]
        w = co @ m[3, :3] + m[3, 3]
        ok = w > 1e-6
        w = np.where(ok, w, 1.0)
        xy = np.empty((len(co), 2))
        xy[:, 0] = (clip[:, 0] / w + 1) * 0.5 * self.region.width
        xy[:, 1] = (clip[:, 1] / w + 1) * 0.5 * self.region.height
        return xy, ok

    def _world(self):
        mw = np.array(self.mw)
        co = np.array([v.co for v in self.verts], dtype=np.float64).reshape(-1, 3)
        return co @ mw[:3, :3].T + mw[:3, 3]

    def _facing(self):
        """Mask of vertices whose normal faces the viewer."""
        nm = self.mw.to_3x3().inverted_safe().transposed()
        mask = np.ones(len(self.verts), dtype=bool)
        if not self.st.brush_front_only:
            return mask
        eye_dir = self.rv3d.view_rotation @ Vector((0, 0, -1))
        persp = self.rv3d.is_perspective
        origin = view3d_utils.region_2d_to_origin_3d(self.region, self.rv3d,
                                                     Vector((self.region.width / 2, self.region.height / 2)))
        for i, v in enumerate(self.verts):
            n = nm @ v.normal
            if persp:
                d = (self.mw @ v.co) - origin
            else:
                d = eye_dir
            mask[i] = n.length < 1e-9 or n.dot(d) <= 0
        return mask

    def _weights(self, mouse):
        xy, ok = self._screen()
        d = np.hypot(xy[:, 0] - mouse.x, xy[:, 1] - mouse.y)
        r = float(self.st.brush_radius)
        inside = ok & (d <= r) & self._facing()
        out = {}
        for i in np.nonzero(inside)[0]:
            w = _falloff(d[i] / r, self.st.brush_falloff) * self.st.brush_strength
            if w > 1e-4:
                out[self.verts[i]] = w
        return out

    def _is_corner(self, v):
        """Open-border vertex where the border turns sharply (kept in place)."""
        b = [e.other_vert(v) for e in v.link_edges if e.is_boundary]
        if len(b) != 2:
            return True
        a1, a2 = b[0].co - v.co, b[1].co - v.co
        if a1.length < 1e-12 or a2.length < 1e-12:
            return True
        return math.degrees(a1.angle(a2)) < 150.0

    def _project(self, v):
        w = self.conform(self.mw @ v.co)
        v.co = self.mw_inv @ w

    # --- strokes -------------------------------------------------------------------
    def _tweak(self, mouse):
        for v, w in self.grab.items():
            if not v.is_valid:
                continue
            if v.is_boundary and (self.st.brush_pin_border or self._is_corner(v)):
                continue
            start_world = self.start_co[v]
            a = view3d_utils.region_2d_to_location_3d(self.region, self.rv3d, self.press, start_world)
            b = view3d_utils.region_2d_to_location_3d(self.region, self.rv3d, mouse, start_world)
            target = start_world + (b - a) * (w / max(self.st.brush_strength, 1e-6) if self.st.brush_strength else 0)
            v.co = self.mw_inv @ self.conform(target)

    def _relax(self, mouse):
        weights = self._weights(mouse)
        new = {}
        for v, w in weights.items():
            if v.is_boundary:
                if self.st.brush_pin_border or self._is_corner(v):
                    continue
                nb = [e.other_vert(v).co for e in v.link_edges if e.is_boundary]
            else:
                nb = [e.other_vert(v).co for e in v.link_edges]
            if not nb:
                continue
            avg = sum(nb, Vector()) / len(nb)
            new[v] = v.co.lerp(avg, min(1.0, w))
        for v, co in new.items():
            v.co = co
            self._project(v)

    # --- operator -------------------------------------------------------------------
    def invoke(self, context, event):
        self.st = context.scene.hb_retopo
        self.obj = pen.retopo_object(context, create=False)
        if self.obj is None:
            self.report({'WARNING'}, "No retopo mesh yet: draw a patch with the Retopo Pen first")
            return {'CANCELLED'}
        pen.ensure_edit(context, self.obj)
        self.region, self.rv3d = context.region, context.region_data
        self.bm = bmesh.from_edit_mesh(self.obj.data)
        self.bm.normal_update()
        self.verts = [v for v in self.bm.verts if not v.hide]
        if not self.verts:
            return {'CANCELLED'}
        self.mw = self.obj.matrix_world.copy()
        self.mw_inv = self.mw.inverted_safe()
        surf = surface.Surfaces(context, exclude=(self.obj,))
        p = patch.Params.from_props(self.st)
        self.mode = self.st.brush_mode
        if self.invert or event.shift:
            self.mode = 'RELAX' if self.mode == 'TWEAK' else 'TWEAK'
        self.press = Vector((event.mouse_region_x, event.mouse_region_y))
        self.grab = self._weights(self.press) if self.mode == 'TWEAK' else {}
        if self.grab:
            p.projection = 'NORMAL'
            p.direction = patch.patch_direction(surf, [self.mw @ v.co for v in list(self.grab)[:32]])
        self.conform = patch.make_conform(surf, p) if surf else (lambda co: co.copy())
        self.start_co = {v: self.mw @ v.co for v in self.grab}
        _cursor.update(pos=tuple(self.press), radius=self.st.brush_radius, active=True)
        if self.mode == 'RELAX':
            self._relax(self.press)
            bmesh.update_edit_mesh(self.obj.data, loop_triangles=False, destructive=False)
        context.window_manager.modal_handler_add(self)
        context.area.header_text_set("Retopo Brush: %s  |  radius %d px, strength %.2f"
                                     % (self.mode.title(), self.st.brush_radius, self.st.brush_strength))
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        mouse = Vector((event.mouse_region_x, event.mouse_region_y))
        if event.type in {'MOUSEMOVE', 'INBETWEEN_MOUSEMOVE'}:
            _cursor["pos"] = tuple(mouse)
            if self.mode == 'TWEAK':
                self._tweak(mouse)
            else:
                self._relax(mouse)
            self.bm.normal_update()
            bmesh.update_edit_mesh(self.obj.data, loop_triangles=False, destructive=False)
            context.area.tag_redraw()
            return {'RUNNING_MODAL'}
        if event.type == 'LEFTMOUSE' and event.value == 'RELEASE':
            context.area.header_text_set(None)
            bmesh.update_edit_mesh(self.obj.data)
            return {'FINISHED'}
        if event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            for v, co in self.start_co.items():   # undo a tweak in progress
                if v.is_valid:
                    v.co = self.mw_inv @ co
            context.area.header_text_set(None)
            bmesh.update_edit_mesh(self.obj.data)
            return {'CANCELLED'}
        return {'RUNNING_MODAL'}


# ----------------------------------------------------------------------------
# Toolbar tool

def _tool_settings(context, layout, tool):
    st = context.scene.hb_retopo
    layout.prop(st, "brush_mode", text="")
    layout.prop(st, "brush_radius")
    layout.prop(st, "brush_strength", slider=True)
    layout.prop(st, "brush_falloff", text="")
    layout.prop(st, "brush_pin_border")
    layout.prop(st, "brush_front_only")


def _make_tool(mode):
    return type("HB_TL_retopo_brush_%s" % mode.lower(), (bpy.types.WorkSpaceTool,), {
        "bl_space_type": 'VIEW_3D',
        "bl_context_mode": mode,
        "bl_idname": "hb_retopo.brush_tool",
        "bl_label": "Retopo Brush",
        "bl_description": "Drag: tweak (or relax, see Tool Settings), Shift+drag: the other one. [ ]: radius",
        "bl_icon": "ops.sculpt.mesh_filter",
        "bl_keymap": (
            ("hb_retopo.brush", {"type": 'LEFTMOUSE', "value": 'PRESS'}, None),
            ("hb_retopo.brush", {"type": 'LEFTMOUSE', "value": 'PRESS', "shift": True},
             {"properties": [("invert", True)]}),
            ("hb_retopo.brush_hover", {"type": 'MOUSEMOVE', "value": 'ANY'}, None),
            ("wm.context_scale_int", {"type": 'LEFT_BRACKET', "value": 'PRESS'},
             {"properties": [("data_path", "scene.hb_retopo.brush_radius"), ("value", 0.9)]}),
            ("wm.context_scale_int", {"type": 'RIGHT_BRACKET', "value": 'PRESS'},
             {"properties": [("data_path", "scene.hb_retopo.brush_radius"), ("value", 1.1)]}),
        ),
        "draw_settings": _tool_settings,
    })


_tools = []
classes = (HB_RETOPO_OT_brush_hover, HB_RETOPO_OT_brush)


def _cursor_visibility():
    """Hide the circle when another tool is active (checked a few times a second)."""
    try:
        tool = bpy.context.workspace.tools.from_space_view3d_mode(bpy.context.mode, create=False)
        active = tool is not None and tool.idname == "hb_retopo.brush_tool"
        if _cursor["active"] != active:
            _cursor["active"] = active
            for area in bpy.context.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()
    except (AttributeError, ReferenceError, TypeError):
        pass
    return 0.25


def register():
    for c in classes:
        bpy.utils.register_class(c)
    _cursor["handle"] = bpy.types.SpaceView3D.draw_handler_add(_draw_cursor, (), 'WINDOW', 'POST_PIXEL')
    for mode in ('OBJECT', 'EDIT_MESH'):
        tool = _make_tool(mode)
        try:
            bpy.utils.register_tool(tool, after={"hb_retopo.pen_tool"})
            _tools.append(tool)
        except Exception as ex:
            print("HB_RetopoPen: brush tool not added for %s (%s)" % (mode, ex))
    bpy.app.timers.register(_cursor_visibility, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_cursor_visibility):
        bpy.app.timers.unregister(_cursor_visibility)
    for tool in _tools:
        try:
            bpy.utils.unregister_tool(tool)
        except Exception:
            pass
    _tools.clear()
    if _cursor["handle"] is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_cursor["handle"], 'WINDOW')
        _cursor["handle"] = None
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
