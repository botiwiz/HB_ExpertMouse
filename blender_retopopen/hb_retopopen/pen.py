# SPDX-License-Identifier: GPL-2.0-or-later
"""Retopo Pen: click corner points on the surface, close the loop, get a quad patch."""

import bmesh
import bpy
import gpu
import numpy as np
from bpy.props import CollectionProperty, EnumProperty, FloatVectorProperty, IntProperty
from bpy_extras import view3d_utils
from gpu_extras.batch import batch_for_shader
from mathutils import Vector

from . import patch, surface

RETOPO_NAME = "Retopo"
LAYOUT_ITEMS = (('AUTO', "Auto", "4 corners: grid; otherwise one quad region per corner around a centre pole"),
                ('POLE', "Centre Pole", "Always one quad region per corner around a centre pole"))


def _settings_props():
    return dict(
        resolution=IntProperty(name="Resolution", default=8, min=1, max=200,
                               description="Segments along the patch's longer sides (mouse wheel while drawing)"),
        relax=IntProperty(name="Relax", default=3, min=0, max=50, description="Smoothing passes on the surface"),
        layout=EnumProperty(name="Layout", items=LAYOUT_ITEMS, default='AUTO'),
        snap_px=IntProperty(name="Snap Radius", default=14, min=2, max=100, subtype='PIXEL',
                            description="Snap to existing retopo vertices and close the loop within this distance"),
    )


class HBRetopoSettings(bpy.types.PropertyGroup):
    __annotations__ = _settings_props()


class HBRetopoPoint(bpy.types.PropertyGroup):
    co: FloatVectorProperty(size=3, subtype='XYZ')


def retopo_object(context, create=True):
    """The mesh being built: the edit object, else an object named Retopo, else a new one."""
    obj = context.edit_object
    if obj is not None and obj.type == 'MESH':
        return obj
    obj = context.scene.objects.get(RETOPO_NAME)
    if obj is None and create:
        me = bpy.data.meshes.new(RETOPO_NAME)
        obj = bpy.data.objects.new(RETOPO_NAME, me)
        context.collection.objects.link(obj)
        obj.show_in_front = True
        obj.show_wire = True
        obj.color = (0.45, 0.65, 1.0, 1.0)
    return obj


def ensure_edit(context, obj):
    if context.mode == 'EDIT_MESH' and context.edit_object is obj:
        return
    if context.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    for o in context.view_layer.objects:
        o.select_set(False)
    obj.hide_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode='EDIT')


def border_sides(obj, bm, corners, tol):
    """Sides of the outline that run along an open border of the retopo mesh: both corners are
    border vertices joined by a border path. Returns {side k: [world points corner k -> k+1]}."""
    mw = obj.matrix_world
    border = [v for v in bm.verts if v.is_boundary]
    if not border:
        return {}

    def find(co):
        best = min(border, key=lambda v: ((mw @ v.co) - co).length)
        return best if ((mw @ best.co) - co).length <= tol else None

    at = [find(Vector(c)) for c in corners]
    n = len(corners)
    out = {}
    for k in range(n):
        a, b = at[k], at[(k + 1) % n]
        if a is None or b is None or a is b:
            continue
        # shortest path from a to b along border edges
        prev, queue = {a: None}, [a]
        for v in queue:
            if v is b:
                break
            for e in v.link_edges:
                if not e.is_boundary:
                    continue
                w = e.other_vert(v)
                if w not in prev:
                    prev[w] = v
                    queue.append(w)
        if b not in prev:
            continue
        path, v = [], b
        while v is not None:
            path.append(v)
            v = prev[v]
        path.reverse()
        pts = [mw @ v.co for v in path]
        chord = (pts[-1] - pts[0]).length
        length = sum((pts[i + 1] - pts[i]).length for i in range(len(pts) - 1))
        if len(pts) >= 2 and length <= 2.0 * chord + 1e-9:   # follows the border, not around the mesh
            out[k] = pts
    return out


def make_patch(context, obj, corners, s, existing=None):
    """Build a patch into the edit mesh of obj. Returns the new faces."""
    bm = bmesh.from_edit_mesh(obj.data)
    surf = surface.Surfaces(context, exclude=(obj,))
    existing = existing if existing is not None else [v for v in bm.verts if v.is_boundary]
    mw_inv = obj.matrix_world.inverted_safe()
    weld = 1e-4 * max(1.0, max(c.length for c in corners))
    fixed = border_sides(obj, bm, corners, tol=weld * 10)
    faces = patch.build(bm, surf, [Vector(c) for c in corners], s.resolution, s.relax, s.layout,
                        weld_to=existing, weld_dist=weld, mw_inv=mw_inv, fixed=fixed)
    bm.normal_update()
    bmesh.update_edit_mesh(obj.data)
    return faces


# ----------------------------------------------------------------------------
# The registered operator (redo panel: change the density afterwards)

class HB_RETOPO_OT_patch(bpy.types.Operator):
    """Create a quad patch on the surface from a closed outline of corner points"""
    bl_idname = "hb_retopo.patch"
    bl_label = "Retopo Patch"
    bl_options = {'REGISTER', 'UNDO'}

    points: CollectionProperty(type=HBRetopoPoint, options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return context.mode in {'EDIT_MESH', 'OBJECT'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "resolution")
        layout.prop(self, "relax")
        layout.prop(self, "layout")

    def execute(self, context):
        if len(self.points) < 3:
            self.report({'WARNING'}, "Needs at least 3 corner points")
            return {'CANCELLED'}
        obj = retopo_object(context)
        ensure_edit(context, obj)
        faces = make_patch(context, obj, [p.co for p in self.points], self)
        bm = bmesh.from_edit_mesh(obj.data)
        for f in bm.faces:
            f.select_set(False)
        for f in faces:
            if f.is_valid:
                f.select_set(True)
        bmesh.update_edit_mesh(obj.data)
        st = context.scene.hb_retopo
        for n in ("resolution", "relax", "layout"):
            setattr(st, n, getattr(self, n))
        return {'FINISHED'}


HB_RETOPO_OT_patch.__annotations__.update(_settings_props())


# ----------------------------------------------------------------------------
# The pen (modal)

def _draw_view(op):
    try:
        pts = [tuple(p) for p in op.points]
        if op.hover is not None and op.preview is None:
            pts_line = pts + [tuple(op.hover)]
        else:
            pts_line = pts
        shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
        region = bpy.context.region
        gpu.state.blend_set('ALPHA')
        gpu.state.depth_test_set('NONE')
        if len(pts_line) >= 2:
            seg = []
            for a, b in zip(pts_line, pts_line[1:]):
                seg += [a, b]
            if op.preview is not None and len(pts) >= 3:
                seg += [pts[-1], pts[0]]
            batch = batch_for_shader(shader, 'LINES', {"pos": seg})
            shader.bind()
            shader.uniform_float("viewportSize", (region.width, region.height))
            shader.uniform_float("lineWidth", 2.0)
            shader.uniform_float("color", (1.0, 0.45, 0.75, 1.0))
            batch.draw(shader)
        if pts:
            ps = gpu.shader.from_builtin('UNIFORM_COLOR')
            gpu.state.point_size_set(9)
            b = batch_for_shader(ps, 'POINTS', {"pos": pts})
            ps.bind()
            ps.uniform_float("color", (1.0, 1.0, 1.0, 1.0))
            b.draw(ps)
            if op.closing:
                gpu.state.point_size_set(16)
                b = batch_for_shader(ps, 'POINTS', {"pos": [pts[0]]})
                ps.uniform_float("color", (0.4, 1.0, 0.5, 0.9))
                b.draw(ps)
            gpu.state.point_size_set(1)
        gpu.state.blend_set('NONE')
    except ReferenceError:
        pass


class HB_RETOPO_OT_pen(bpy.types.Operator):
    """Draw a patch outline on the surface: click corners (snaps to the surface and to existing
retopo vertices), click the first point again (or Enter) to close it into a quad patch,
mouse wheel sets the density, click / Enter to keep it"""
    bl_idname = "hb_retopo.pen"
    bl_label = "Retopo Pen"
    bl_options = {'BLOCKING'}

    @classmethod
    def poll(cls, context):
        return context.area is not None and context.area.type == 'VIEW_3D' and context.mode in {'EDIT_MESH', 'OBJECT'}

    # --- helpers ---------------------------------------------------------------
    def _mouse(self, event):
        return Vector((event.mouse_region_x, event.mouse_region_y))

    def _existing_snap(self, mouse):
        """World position of the retopo vertex nearest the mouse within the snap radius."""
        if self.vert_cos is None or not len(self.vert_cos):
            return None
        m = np.array(self.rv3d.perspective_matrix)
        pts = self.vert_cos
        clip = pts @ m[:3, :3].T + m[:3, 3]
        w = pts @ m[3, :3] + m[3, 3]
        ok = w > 1e-6
        w = np.where(ok, w, 1.0)
        x = (clip[:, 0] / w + 1) * 0.5 * self.region.width
        y = (clip[:, 1] / w + 1) * 0.5 * self.region.height
        d = np.hypot(x - mouse.x, y - mouse.y)
        d = np.where(ok, d, np.inf)
        i = int(np.argmin(d))
        return Vector(pts[i]) if d[i] <= self.st.snap_px else None

    def _refresh_verts(self):
        bm = bmesh.from_edit_mesh(self.obj.data)
        mw = np.array(self.obj.matrix_world)
        co = np.array([v.co for v in bm.verts]) if bm.verts else np.zeros((0, 3))
        self.vert_cos = co @ mw[:3, :3].T + mw[:3, 3] if len(co) else co
        self.boundary = [v for v in bm.verts if v.is_boundary]

    def _pick(self, mouse):
        snap = self._existing_snap(mouse)
        if snap is not None:
            return snap
        loc, _n = self.surf.mouse_hit(self.region, self.rv3d, mouse)
        return loc

    def _near_first(self, mouse):
        if len(self.points) < 3:
            return False
        p = view3d_utils.location_3d_to_region_2d(self.region, self.rv3d, self.points[0])
        return p is not None and (p - mouse).length <= self.st.snap_px

    def _header(self, context):
        if self.preview is None:
            txt = ("Retopo Pen  %d points  |  click: add corner, click first point / Enter: close, "
                   "Backspace: undo point, wheel: resolution (%d), Esc: done" % (len(self.points), self.st.resolution))
        else:
            txt = ("Retopo Pen  Resolution %d  |  wheel / +-: density, click / Enter: keep, "
                   "Backspace: back to editing the outline, Esc: discard" % self.st.resolution)
        context.area.header_text_set(txt)

    def _clear_preview(self):
        if self.preview:
            bm = bmesh.from_edit_mesh(self.obj.data)
            verts = {v for f in self.preview if f.is_valid for v in f.verts}
            # only remove vertices that were made for the preview (keep welded existing ones)
            new = [v for v in verts if v not in self.old_verts]
            bmesh.ops.delete(bm, geom=[f for f in self.preview if f.is_valid], context='FACES_ONLY')
            bmesh.ops.delete(bm, geom=[v for v in new if v.is_valid and not v.link_faces], context='VERTS')
            loose = [e for e in bm.edges if not e.link_faces and e.verts[0] not in self.old_verts]
            if loose:
                bmesh.ops.delete(bm, geom=loose, context='EDGES')
            bmesh.update_edit_mesh(self.obj.data)
        self.preview = None

    def _make_preview(self, context):
        self._clear_preview()
        bm = bmesh.from_edit_mesh(self.obj.data)
        self.old_verts = set(bm.verts)
        self.preview = make_patch(context, self.obj, self.points, self.st, existing=self.boundary)

    def _commit(self, context):
        pts = [tuple(p) for p in self.points]
        self._clear_preview()
        bpy.ops.hb_retopo.patch(points=[{"name": "", "co": p} for p in pts], resolution=self.st.resolution,
                                relax=self.st.relax, layout=self.st.layout)
        self.points = []
        self._refresh_verts()

    # --- operator ------------------------------------------------------------------
    def invoke(self, context, event):
        self.st = context.scene.hb_retopo
        self.obj = retopo_object(context)
        ensure_edit(context, self.obj)
        self.region, self.rv3d = context.region, context.region_data
        self.surf = surface.Surfaces(context, exclude=(self.obj,))
        if not self.surf:
            self.report({'WARNING'}, "No surface to draw on: show the object(s) to retopologize")
            return {'CANCELLED'}
        self.points, self.preview, self.hover, self.closing = [], None, None, False
        self.old_verts = set()
        self._refresh_verts()
        p = self._pick(self._mouse(event))
        if p is not None:
            self.points.append(p)
        self._handle = bpy.types.SpaceView3D.draw_handler_add(_draw_view, (self,), 'WINDOW', 'POST_VIEW')
        context.window_manager.modal_handler_add(self)
        self._header(context)
        return {'RUNNING_MODAL'}

    def _finish(self, context):
        bpy.types.SpaceView3D.draw_handler_remove(self._handle, 'WINDOW')
        context.area.header_text_set(None)
        context.area.tag_redraw()

    def modal(self, context, event):
        context.area.tag_redraw()
        mouse = self._mouse(event)
        if event.type in {'MIDDLEMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM'} or \
                (event.type in {'WHEELUPMOUSE', 'WHEELDOWNMOUSE'} and (event.ctrl or event.shift)):
            return {'PASS_THROUGH'}

        if event.type == 'MOUSEMOVE':
            if self.preview is None:
                self.hover = self._pick(mouse)
                self.closing = self._near_first(mouse)
            return {'RUNNING_MODAL'}

        if event.type in {'WHEELUPMOUSE', 'NUMPAD_PLUS', 'EQUAL'} and event.value == 'PRESS':
            self.st.resolution += 1
            if self.preview is not None:
                self._make_preview(context)
            self._header(context)
            return {'RUNNING_MODAL'}
        if event.type in {'WHEELDOWNMOUSE', 'NUMPAD_MINUS', 'MINUS'} and event.value == 'PRESS':
            self.st.resolution = max(1, self.st.resolution - 1)
            if self.preview is not None:
                self._make_preview(context)
            self._header(context)
            return {'RUNNING_MODAL'}

        if event.type == 'LEFTMOUSE' and event.value == 'PRESS':
            if self.preview is not None:
                self._commit(context)
            elif self._near_first(mouse):
                self._make_preview(context)
            else:
                p = self._pick(mouse)
                if p is not None:
                    self.points.append(p)
            self._header(context)
            return {'RUNNING_MODAL'}

        if event.type in {'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'PRESS':
            if self.preview is not None:
                self._commit(context)
            elif len(self.points) >= 3:
                self._make_preview(context)
            self._header(context)
            return {'RUNNING_MODAL'}

        if event.type in {'BACK_SPACE', 'DEL'} and event.value == 'PRESS' or \
                (event.type == 'Z' and event.ctrl and event.value == 'PRESS'):
            if self.preview is not None:
                self._clear_preview()
            elif self.points:
                self.points.pop()
            self._header(context)
            return {'RUNNING_MODAL'}

        if event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            if self.preview is not None:
                self._clear_preview()
                self.points = []
                self._header(context)
                return {'RUNNING_MODAL'}
            if self.points:
                self.points = []
                self._header(context)
                return {'RUNNING_MODAL'}
            self._finish(context)
            return {'FINISHED'}
        return {'RUNNING_MODAL'}


# ----------------------------------------------------------------------------
# Toolbar tool with Tool Settings

def _tool_settings(context, layout, tool):
    st = context.scene.hb_retopo
    layout.prop(st, "resolution")
    layout.prop(st, "relax")
    layout.prop(st, "layout", text="")
    layout.prop(st, "snap_px")


def _make_tool(mode):
    return type("HB_TL_retopo_pen_%s" % mode.lower(), (bpy.types.WorkSpaceTool,), {
        "bl_space_type": 'VIEW_3D',
        "bl_context_mode": mode,
        "bl_idname": "hb_retopo.pen_tool",
        "bl_label": "Retopo Pen",
        "bl_description": ("Click corner points on the surface, close the loop for a quad patch; "
                           "mouse wheel: density"),
        "bl_icon": "ops.mesh.polybuild_hover",
        "bl_keymap": (("hb_retopo.pen", {"type": 'LEFTMOUSE', "value": 'PRESS'}, None),),
        "draw_settings": _tool_settings,
    })


_tools = []
classes = (HBRetopoSettings, HBRetopoPoint, HB_RETOPO_OT_patch, HB_RETOPO_OT_pen)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.hb_retopo = bpy.props.PointerProperty(type=HBRetopoSettings)
    for mode in ('OBJECT', 'EDIT_MESH'):
        tool = _make_tool(mode)
        try:
            bpy.utils.register_tool(tool, separator=True)
            _tools.append(tool)
        except Exception as ex:
            print("HB_RetopoPen: tool not added for %s (%s)" % (mode, ex))


def unregister():
    for tool in _tools:
        try:
            bpy.utils.unregister_tool(tool)
        except Exception:
            pass
    _tools.clear()
    del bpy.types.Scene.hb_retopo
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
