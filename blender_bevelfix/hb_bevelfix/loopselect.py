# SPDX-License-Identifier: GPL-2.0-or-later
"""Select tools working like MESHmachine's Select / LSelect / SSelect / VSelect.

Select (Alt + click, Blender's loop-select key) looks at what is already selected:
  * Vertex mode, the mesh has vertex groups and vertices are selected -> VSelect
  * Edge mode, isolated edges selected (edges not touching other selected edges):
      any of them smooth -> LSelect (angle based loop select), all sharp -> SSelect
      (connected sharp edges). The redo panel toggles Loop / Sharp and sets Min Angle.
  * Face mode, exactly two neighbouring faces selected -> LSelect face loop
  * otherwise it passes the click through to Blender's own loop select.
"""

import math

import bmesh
import bpy
import gpu
from bpy.props import BoolProperty, IntProperty
from gpu_extras.batch import batch_for_shader


def _mode(context):
    return tuple(context.scene.tool_settings.mesh_select_mode)


# ----------------------------------------------------------------------------
# Edge loops (LSelect)

def isolated_edges(edges):
    """Selected edges that don't share a vertex with another selected edge."""
    sel = set(edges)
    return [e for e in edges
            if not any(x is not e and x in sel for v in e.verts for x in v.link_edges)]


def angle_between_edges(e1, e2):
    """Angle (degrees) between two edges at their shared vertex: 180 = straight continuation."""
    shared = set(e1.verts) & set(e2.verts)
    if not shared:
        return 0.0
    v = shared.pop()
    a = e1.other_vert(v).co - v.co
    b = e2.other_vert(v).co - v.co
    if a.length < 1e-12 or b.length < 1e-12:
        return 0.0
    return math.degrees(a.angle(b))


def get_loop_edges(min_angle, edge, vert, select=False):
    """Walk from `edge` away from `vert`: at each vertex take the edge forming the largest angle
    with the current one (the straightest continuation) while that angle is above min_angle."""
    walked = []
    while True:
        other = edge.other_vert(vert)
        nxt = [(angle_between_edges(edge, e), e) for e in other.link_edges if e is not edge and not e.hide]
        if nxt:
            ang, e = max(nxt, key=lambda x: x[0])
            if e not in walked and ang > min_angle:
                if select:
                    e.select_set(True)
                walked.append(e)
                vert, edge = other, e
                continue
        break
    return walked


def loop_select_edges(edges, min_angle_ui):
    """LSelect on edges. min_angle_ui is the UI value (higher grows the selection)."""
    out = []
    for edge in edges:
        for vert in edge.verts:  # both directions
            out += get_loop_edges(180 - min_angle_ui, edge, vert, select=True)
    return out


# ----------------------------------------------------------------------------
# Face loops (LSelect with two neighbouring faces)

def _neighbor_loop_select(faces, center_edge, reverse=False):
    loop1, loop2 = center_edge.link_loops[0], center_edge.link_loops[1]
    loop = None
    for a, b, pick in ((loop1.link_loop_next.link_loop_radial_next, loop2.link_loop_prev.link_loop_radial_next, 0),
                       (loop1.link_loop_prev.link_loop_radial_next, loop2.link_loop_next.link_loop_radial_next, 1)):
        fa, fb = a.face, b.face
        if fa is not fb and len(fa.verts) == len(fb.verts) == 4 and set(fa.edges) & set(fb.edges):
            if pick == 0:
                loop = a if reverse else b
            else:
                loop = b if reverse else a
            break
    if loop is None:
        return False
    for _ in range(100000):
        step = loop.link_loop_prev if reverse else loop.link_loop_next
        across = step.link_loop_radial_next
        if across is step:  # open mesh border: the strip ends here (don't turn back)
            break
        loop = across.link_loop_prev if reverse else across.link_loop_next
        if loop.link_loop_radial_next is loop:  # no face on the loop side
            break
        loop_face, sel_face = loop.face, loop.link_loop_radial_next.face
        if sel_face not in faces and loop_face and sel_face and len(loop_face.verts) == 4:
            sel_face.select_set(True)
            faces.append(sel_face)
        else:
            break
    return True


def loop_select_faces(faces):
    """Continue the face loop through two neighbouring selected faces. Returns an error or None."""
    common = set(faces[0].edges) & set(faces[1].edges)
    if not common:
        return "The 2 selected faces aren't next to each other"
    center = common.pop()
    if len(center.link_loops) != 2:
        return "The 2 selected faces aren't next to each other"
    if not _neighbor_loop_select(faces, center):
        return "Couldn't find 2 quads on either side of the selection"
    _neighbor_loop_select(faces, center, reverse=True)
    return None


def _face_islands(faces):
    left, islands = set(faces), []
    while left:
        stack = [left.pop()]
        island = set(stack)
        while stack:
            f = stack.pop()
            for e in f.edges:
                for g in e.link_faces:
                    if g in left:
                        left.discard(g)
                        island.add(g)
                        stack.append(g)
        islands.append(island)
    return islands


# ----------------------------------------------------------------------------
# Sharp edges (SSelect)

def sharp_select(bm):
    sharps = [e for e in bm.edges if e.select and not e.smooth]
    select = set(sharps)
    stack = list(sharps)
    while stack:
        e = stack.pop()
        for v in e.verts:
            for x in v.link_edges:
                if not x.smooth and x not in select:
                    select.add(x)
                    stack.append(x)
    for e in select:
        e.select = True
    return len(select)


# ----------------------------------------------------------------------------
# Operators

class MESH_OT_hb_lselect(bpy.types.Operator):
    """Turn isolated edge selections into loop selections with control over the loop angle
(or continue the face loop through 2 neighbouring selected faces)"""
    bl_idname = "mesh.hb_lselect"
    bl_label = "LSelect (Loop Select)"
    bl_options = {'REGISTER', 'UNDO'}

    min_angle: IntProperty(name="Min Angle", default=60, min=0, max=180,
                           description="How much the loop may turn at a vertex (higher grows the selection)")

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        obj = context.edit_object
        bm = bmesh.from_edit_mesh(obj.data)
        mode = _mode(context)
        if mode == (False, False, True):
            faces = [f for f in bm.faces if f.select]
            if len(faces) != 2:
                self.report({'WARNING'}, "Select 2 neighbouring faces")
                return {'CANCELLED'}
            err = loop_select_faces(faces)
            if err:
                self.report({'WARNING'}, err)
                return {'CANCELLED'}
        else:
            edges = isolated_edges([e for e in bm.edges if e.select])
            if not edges:
                self.report({'WARNING'}, "Select one or more isolated edges")
                return {'CANCELLED'}
            loop_select_edges(edges, self.min_angle)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)
        return {'FINISHED'}


class MESH_OT_hb_sselect(bpy.types.Operator):
    """Select all sharp edges connected to the existing selection"""
    bl_idname = "mesh.hb_sselect"
    bl_label = "SSelect (Sharp Select)"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        obj = context.edit_object
        bm = bmesh.from_edit_mesh(obj.data)
        if not any(e.select and not e.smooth for e in bm.edges):
            self.report({'WARNING'}, "Select a sharp edge")
            return {'CANCELLED'}
        sharp_select(bm)
        bm.select_flush_mode()
        bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)
        return {'FINISHED'}


class MESH_OT_hb_vselect(bpy.types.Operator):
    """Visually select mesh elements by vertex group: wheel cycles the groups of the selection
(all groups if nothing is selected), S toggles the current group, A toggles all, click confirms"""
    bl_idname = "mesh.hb_vselect"
    bl_label = "VSelect (Vertex Group Select)"
    bl_options = {'REGISTER', 'UNDO'}

    sel_idx: IntProperty(name="Selection Index")

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH' and context.edit_object and len(context.edit_object.vertex_groups) > 0

    # --- data ---------------------------------------------------------------
    def _gather(self, context):
        self.obj = context.edit_object
        self.bm = bmesh.from_edit_mesh(self.obj.data)
        deform = self.bm.verts.layers.deform.verify()
        sel = [v for v in self.bm.verts if v.select]
        if sel:
            gids = sorted({g for v in sel for g in v[deform].keys()})
        else:
            gids = list(range(len(self.obj.vertex_groups)))
        if not gids:
            return False
        self.gids = gids
        self.members = {g: [v for v in self.bm.verts if g in v[deform]] for g in gids}
        self.chosen = {g: False for g in gids}
        self.sel_idx = self.sel_idx % len(gids)
        return True

    def _apply(self):
        for g, on in self.chosen.items():
            if on:
                for v in self.members[g]:
                    v.select = True
        self.bm.select_flush(True)
        bmesh.update_edit_mesh(self.obj.data)

    # --- drawing ------------------------------------------------------------
    def _draw(self):
        mw = self.obj.matrix_world
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')
        gpu.state.blend_set('ALPHA')
        gpu.state.depth_test_set('NONE')

        def pts(verts, color, size):
            co = [tuple(mw @ v.co) for v in verts if v.is_valid]
            if not co:
                return
            gpu.state.point_size_set(size)
            batch = batch_for_shader(shader, 'POINTS', {"pos": co})
            shader.bind()
            shader.uniform_float("color", color)
            batch.draw(shader)

        pts({v for g in self.gids for v in self.members[g]}, (0.35, 0.35, 0.35, 1.0), 5)
        pts({v for g, on in self.chosen.items() if on for v in self.members[g]}, (0.3, 0.9, 0.4, 1.0), 8)
        pts(self.members[self.gids[self.sel_idx]], (1.0, 1.0, 1.0, 1.0), 4)
        gpu.state.point_size_set(1)
        gpu.state.blend_set('NONE')

    def _header(self, context):
        g = self.gids[self.sel_idx]
        context.area.header_text_set(
            "VSelect  Group %d/%d: %s  [%s]   |  wheel: next group, S: toggle, A: toggle all, "
            "LMB/Space: select, RMB/Esc: cancel" % (self.sel_idx + 1, len(self.gids), self.obj.vertex_groups[g].name,
                                                    "selected" if self.chosen[g] else "-"))

    # --- operator -------------------------------------------------------------
    def invoke(self, context, event):
        if not self._gather(context):
            self.report({'WARNING'}, "The selection isn't in any vertex group")
            return {'CANCELLED'}
        if len(self.gids) == 1:
            self.chosen[self.gids[0]] = True
            self._apply()
            return {'FINISHED'}
        self._handle = bpy.types.SpaceView3D.draw_handler_add(self._draw, (), 'WINDOW', 'POST_VIEW')
        self._header(context)
        context.window_manager.modal_handler_add(self)
        context.area.tag_redraw()
        return {'RUNNING_MODAL'}

    def _finish(self, context):
        bpy.types.SpaceView3D.draw_handler_remove(self._handle, 'WINDOW')
        context.area.header_text_set(None)
        context.area.tag_redraw()

    def modal(self, context, event):
        if event.type in {'WHEELUPMOUSE', 'ONE'} and event.value == 'PRESS':
            self.sel_idx = (self.sel_idx + 1) % len(self.gids)
        elif event.type in {'WHEELDOWNMOUSE', 'TWO'} and event.value == 'PRESS':
            self.sel_idx = (self.sel_idx - 1) % len(self.gids)
        elif event.type == 'S' and event.value == 'PRESS':
            g = self.gids[self.sel_idx]
            self.chosen[g] = not self.chosen[g]
        elif event.type == 'A' and event.value == 'PRESS':
            on = not any(self.chosen.values())
            for g in self.chosen:
                self.chosen[g] = on
        elif event.type in {'MIDDLEMOUSE'} or event.type.startswith('NDOF') or \
                (event.alt and event.type in {'LEFTMOUSE', 'RIGHTMOUSE'}):
            return {'PASS_THROUGH'}
        elif event.type in {'LEFTMOUSE', 'SPACE', 'RET'} and event.value == 'PRESS':
            if not any(self.chosen.values()):  # nothing toggled: take the current group
                self.chosen[self.gids[self.sel_idx]] = True
            self._apply()
            self._finish(context)
            return {'FINISHED'}
        elif event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            self._finish(context)
            return {'CANCELLED'}
        else:
            return {'RUNNING_MODAL'}
        self._header(context)
        context.area.tag_redraw()
        return {'RUNNING_MODAL'}

    def execute(self, context):  # redo / scripting: select the current group
        if not self._gather(context):
            return {'CANCELLED'}
        self.chosen[self.gids[self.sel_idx]] = True
        self._apply()
        return {'FINISHED'}


def _always_loop():
    addon = bpy.context.preferences.addons.get(__package__)
    return bool(addon and getattr(addon.preferences, "always_loop", False))


class MESH_OT_hb_select(bpy.types.Operator):
    """Run VSelect, SSelect, LSelect on the current selection, or pass through to Blender's loop select"""
    bl_idname = "mesh.hb_select"
    bl_label = "Select (Loop / Sharp / Vertex Group)"
    bl_options = {'REGISTER', 'UNDO'}

    loop: BoolProperty(name="Loop Select", default=False)
    min_angle: IntProperty(name="Min Angle", default=60, min=0, max=180,
                           description="How much the loop may turn at a vertex (higher grows the selection)")
    draw_props: BoolProperty(default=False, options={'HIDDEN', 'SKIP_SAVE'})
    select_type: bpy.props.StringProperty(default="", options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        if self.draw_props:
            row = self.layout.column().row(align=True)
            row.prop(self, "loop", text="Loop Select" if self.loop else "Sharp Select", toggle=True)
            r = row.row(align=True)
            r.active = self.loop
            r.prop(self, "min_angle")

    @staticmethod
    def edge_type(edges, always_loop):
        iso = isolated_edges(edges)
        if not iso:
            return ""
        if always_loop or any(e.smooth for e in iso):
            return 'LOOP'
        return 'SHARP'

    def _run(self, context, interactive):
        """First run on the current selection. Returns False if nothing applies."""
        self.loop = False
        self.draw_props = False
        self.select_type = ""
        obj = context.edit_object
        bm = bmesh.from_edit_mesh(obj.data)
        mode = _mode(context)

        if mode == (True, False, False):
            if obj.vertex_groups and any(v.select for v in bm.verts):
                if interactive:
                    bpy.ops.mesh.hb_vselect('INVOKE_DEFAULT')
                else:
                    bpy.ops.mesh.hb_vselect()
                return True

        elif mode == (False, True, False):
            self.select_type = self.edge_type([e for e in bm.edges if e.select], _always_loop())
            if self.select_type:
                if self.select_type == 'LOOP':
                    self.loop = True
                    bpy.ops.mesh.hb_lselect(min_angle=self.min_angle)
                else:
                    bpy.ops.mesh.hb_sselect()
                self.draw_props = True
                return True

        elif mode == (False, False, True):
            faces = [f for f in bm.faces if f.select]
            if len(faces) == 2 and len(_face_islands(faces)) == 1:
                bpy.ops.mesh.hb_lselect()
                return True
        return False

    def invoke(self, context, event):
        if self._run(context, interactive=True):
            return {'FINISHED'}
        return {'PASS_THROUGH'}  # Blender's own loop select gets the click

    def execute(self, context):
        if not self.select_type:  # called directly (script / no window): act like the click
            return {'FINISHED'} if self._run(context, interactive=False) else {'CANCELLED'}
        # redo panel
        if not self.loop:
            bm = bmesh.from_edit_mesh(context.edit_object.data)
            if all(e.smooth for e in bm.edges if e.select):
                self.loop = True
        if self.loop:
            bpy.ops.mesh.hb_lselect(min_angle=self.min_angle)
        else:
            bpy.ops.mesh.hb_sselect()
        return {'FINISHED'}


classes = (MESH_OT_hb_lselect, MESH_OT_hb_sselect, MESH_OT_hb_vselect, MESH_OT_hb_select)


class VIEW3D_MT_hb_select(bpy.types.Menu):
    bl_label = "HB Select"

    def draw(self, context):
        layout = self.layout
        layout.operator(MESH_OT_hb_select.bl_idname, text="Select")
        layout.separator()
        layout.operator(MESH_OT_hb_lselect.bl_idname, text="LSelect")
        layout.operator(MESH_OT_hb_sselect.bl_idname, text="SSelect")
        layout.operator(MESH_OT_hb_vselect.bl_idname, text="VSelect")


def loop_select_keymap_event():
    """The key Blender's own Alt loop select uses in the active keymap (so pass-through reaches it)."""
    try:
        km = bpy.context.window_manager.keyconfigs.active.keymaps.get("Mesh")
        for kmi in km.keymap_items:
            if kmi.idname == "mesh.loop_select" and kmi.alt and not kmi.shift and not kmi.ctrl:
                return kmi.type, kmi.value
    except AttributeError:
        pass
    return 'LEFTMOUSE', 'CLICK'


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.utils.register_class(VIEW3D_MT_hb_select)


def unregister():
    bpy.utils.unregister_class(VIEW3D_MT_hb_select)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
