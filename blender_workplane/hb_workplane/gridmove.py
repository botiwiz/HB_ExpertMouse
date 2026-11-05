# SPDX-License-Identifier: GPL-2.0-or-later
"""Move the selection on the workplane, snapping to its grid."""

import bmesh
import bpy
from bpy.props import EnumProperty
from bpy_extras import view3d_utils
from mathutils import Vector
from mathutils.geometry import intersect_line_line, intersect_line_plane

from . import draw, ops

SUPPORTED_MODES = {'OBJECT', 'EDIT_MESH'}


# ----------------------------------------------------------------------------
# Pure math (testable without a window)

def ray_point(ref, m, axis, ray_o, ray_d):
    """Point under the mouse ray on the drag surface through `ref`:
    the plane parallel to the workplane (PLANE/X/Y) or the normal line (NORMAL)."""
    n = m.col[2].to_3d().normalized()
    if axis == 'NORMAL':
        res = intersect_line_line(ref, ref + n, ray_o, ray_o + ray_d)
        return res[0] if res else None
    if abs(ray_d.normalized().dot(n)) < 1e-4:
        return None
    return intersect_line_plane(ray_o, ray_o + ray_d, ref, n)


def solve(ref, m, step, axis, start_pt, cur_pt, snap):
    """New position of the reference point.

    PLANE: free on the plane; X / Y: along that workplane axis; NORMAL: along the normal.
    With snap, in-plane coordinates land on grid lines (absolute, measured from the
    workplane origin); along the normal the distance moves in whole steps (relative)."""
    rot = m.to_3x3().normalized()
    rt = rot.transposed()
    origin = m.translation
    delta = cur_pt - start_pt
    if axis == 'NORMAL':
        n = rot.col[2]
        d = delta.dot(n)
        if snap and step > 0:
            d = round(d / step) * step
        return ref + n * d
    local_delta = rt @ delta
    local_delta.z = 0.0
    if axis == 'X':
        local_delta.y = 0.0
    elif axis == 'Y':
        local_delta.x = 0.0
    lt = rt @ (ref - origin) + local_delta
    if snap and step > 0:
        if axis in {'PLANE', 'X'}:
            lt.x = round(lt.x / step) * step
        if axis in {'PLANE', 'Y'}:
            lt.y = round(lt.y / step) * step
    return origin + rot @ lt


# ----------------------------------------------------------------------------
# Selection handling

class _ObjectItems:
    def __init__(self, context):
        sel = [o for o in context.selected_editable_objects]
        sel_set = set(sel)
        # Only move top-most selected objects (children follow their parent).
        self.objs = [o for o in sel if not any(p in sel_set for p in _parents(o))]
        self.orig = [o.matrix_world.copy() for o in self.objs]
        act = context.active_object
        if act in self.objs:
            self.ref = act.matrix_world.translation.copy()
        elif self.objs:
            self.ref = sum((o.matrix_world.translation for o in self.objs), Vector()) / len(self.objs)
        else:
            self.ref = None

    def apply(self, delta):
        for o, mw in zip(self.objs, self.orig):
            new = mw.copy()
            new.translation = mw.translation + delta
            o.matrix_world = new

    def restore(self):
        self.apply(Vector())


def _parents(o):
    p = o.parent
    while p:
        yield p
        p = p.parent


class _MeshItems:
    def __init__(self, context):
        self.data = []  # (obj, bm, verts, orig_cos, world->local 3x3)
        pts = []
        active = None
        for obj in (getattr(context, "objects_in_mode", None) or [context.edit_object]):
            if obj is None or obj.type != 'MESH':
                continue
            bm = bmesh.from_edit_mesh(obj.data)
            verts = [v for v in bm.verts if v.select and not v.hide]
            if not verts:
                continue
            mw = obj.matrix_world
            self.data.append((obj, bm, verts, [v.co.copy() for v in verts], mw.inverted_safe().to_3x3()))
            pts += [mw @ v.co for v in verts]
            hist = bm.select_history.active
            if obj == context.edit_object and isinstance(hist, bmesh.types.BMVert) and hist.select:
                active = mw @ hist.co
        self.ref = active if active is not None else (sum(pts, Vector()) / len(pts) if pts else None)

    def apply(self, delta):
        for obj, bm, verts, orig, inv3 in self.data:
            ld = inv3 @ delta
            for v, co in zip(verts, orig):
                v.co = co + ld
            bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)

    def restore(self):
        self.apply(Vector())


# ----------------------------------------------------------------------------
# Operator

class HB_WORKPLANE_OT_grid_move(bpy.types.Operator):
    """Move the selection on the workplane, snapping to its grid.
Ctrl: toggle snapping.  X / Y: lock to a workplane axis.  Z: along the normal"""
    bl_idname = "hb_workplane.grid_move"
    bl_label = "Move on Workplane Grid"
    bl_options = {'REGISTER', 'UNDO', 'BLOCKING', 'GRAB_CURSOR'}

    axis: EnumProperty(name="Constraint", items=(
        ('PLANE', "Plane", "Move freely on the workplane"),
        ('X', "X", "Along the workplane X axis"),
        ('Y', "Y", "Along the workplane Y axis"),
        ('NORMAL', "Normal", "Along the workplane normal"),
    ), default='PLANE')

    @classmethod
    def poll(cls, context):
        return context.mode in SUPPORTED_MODES and context.region_data is not None

    def _ray(self, event):
        co = Vector((event.mouse_x - self.region.x, event.mouse_y - self.region.y))
        o = view3d_utils.region_2d_to_origin_3d(self.region, self.rv3d, co)
        d = view3d_utils.region_2d_to_vector_3d(self.region, self.rv3d, co)
        return o, d

    def invoke(self, context, event):
        s = context.scene.hb_workplane
        self.region = context.region
        self.rv3d = context.region_data
        self.items = _MeshItems(context) if context.mode == 'EDIT_MESH' else _ObjectItems(context)
        if self.items.ref is None:
            self.report({'WARNING'}, "Nothing selected")
            return {'CANCELLED'}
        self.m = ops.current_matrix(context, self.rv3d)
        self.step = draw.grid_step(context, self.rv3d) / max(1, s.subdivisions)
        self.base_axis = self.axis
        self.ref = self.items.ref.copy()
        if not self._start(event):
            return {'CANCELLED'}
        self.result = self.ref.copy()
        context.window_manager.modal_handler_add(self)
        self._update(context, event)
        return {'RUNNING_MODAL'}

    def _start(self, event=None):
        if event is not None:
            self.init_ray = self._ray(event)
        o, d = self.init_ray
        self.start_pt = ray_point(self.ref, self.m, self.axis, o, d)
        if self.start_pt is None:  # looking edge-on at the plane: fall back to the normal-free axis
            self.report({'WARNING'}, "View is edge-on to the workplane")
            return False
        return True

    def _update(self, context, event):
        s = context.scene.hb_workplane
        o, d = self._ray(event)
        cur = ray_point(self.ref, self.m, self.axis, o, d)
        if cur is None:
            return
        snap = s.grid_snap != event.ctrl
        self.result = solve(self.ref, self.m, self.step, self.axis, self.start_pt, cur, snap)
        delta = self.result - self.ref
        self.items.apply(delta)
        draw.state["snap_point"] = self.result if snap else None
        draw.state["snap_step"] = self.step
        draw.state["snap_matrix"] = self.m
        lock = {'PLANE': "plane", 'X': "X axis", 'Y': "Y axis", 'NORMAL': "normal"}[self.axis]
        dl = self.m.to_3x3().normalized().transposed() @ delta
        context.area.header_text_set(
            "Workplane move (%s)  dX %.4g  dY %.4g  dZ %.4g  |  grid %.4g%s  |  "
            "Ctrl: snap on/off  X/Y/Z: lock  LMB: confirm  RMB/Esc: cancel"
            % (lock, dl.x, dl.y, dl.z, self.step, "" if snap else " (snap off)"))
        ops.tag_redraw_all(context)

    def _end(self, context):
        draw.state["snap_point"] = None
        context.area.header_text_set(None)
        ops.tag_redraw_all(context)

    def modal(self, context, event):
        if event.type in {'MIDDLEMOUSE', 'WHEELUPMOUSE', 'WHEELDOWNMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM'}:
            return {'PASS_THROUGH'}
        if event.type in {'MOUSEMOVE', 'INBETWEEN_MOUSEMOVE', 'LEFT_CTRL', 'RIGHT_CTRL'}:
            self._update(context, event)
            return {'RUNNING_MODAL'}
        if event.type in {'X', 'Y', 'Z'} and event.value == 'PRESS':
            want = {'X': 'X', 'Y': 'Y', 'Z': 'NORMAL'}[event.type]
            old = self.axis
            self.axis = self.base_axis if self.axis == want else want
            if not self._start():  # measured from the original mouse position, like Blender
                self.axis = old
                self._start()
            self._update(context, event)
            return {'RUNNING_MODAL'}
        if event.type in {'LEFTMOUSE', 'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'PRESS':
            self._end(context)
            return {'FINISHED'}
        if event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            self.items.restore()
            self._end(context)
            return {'CANCELLED'}
        return {'RUNNING_MODAL'}


def register():
    bpy.utils.register_class(HB_WORKPLANE_OT_grid_move)


def unregister():
    bpy.utils.unregister_class(HB_WORKPLANE_OT_grid_move)
