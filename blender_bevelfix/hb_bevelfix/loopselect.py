# SPDX-License-Identifier: GPL-2.0-or-later
"""Angle-based edge loop selection (in the spirit of MESHmachine's LSelect / Select wrapper).

From each selected edge the loop is grown in both directions: at every vertex it continues
along the edge that keeps the current direction best, as long as the turn stays within
Max Angle. It does not rely on quad topology (Blender's loop select does).
If every selected edge is marked sharp, all connected sharp edges are selected instead."""

import math

import bmesh
import bpy
from bpy.props import BoolProperty, FloatProperty


def _continue(v_from, v, edge, max_angle, taken):
    """Next edge after `edge` at vertex v (coming from v_from), or None."""
    d = v.co - v_from.co
    if d.length < 1e-12:
        return None
    d.normalize()
    best, best_ang = None, max_angle
    for e in v.link_edges:
        if e is edge or e.hide or e in taken:
            continue
        w = e.other_vert(v).co - v.co
        if w.length < 1e-12:
            continue
        ang = d.angle(w)
        if ang <= best_ang:
            best, best_ang = e, ang
    return best


def angle_loop(start_edges, max_angle):
    """Edges of the loops grown from start_edges."""
    result = set(start_edges)
    for e0 in start_edges:
        for v_start in e0.verts:
            v_from, v, edge = e0.other_vert(v_start), v_start, e0
            for _ in range(100000):
                nxt = _continue(v_from, v, edge, max_angle, result)
                if nxt is None:
                    break
                result.add(nxt)
                v_from, v, edge = v, nxt.other_vert(v), nxt  # stops by itself once the loop closes
    return result


def connected_sharp(start_edges):
    result = set(start_edges)
    stack = list(start_edges)
    while stack:
        e = stack.pop()
        for v in e.verts:
            for x in v.link_edges:
                if not x.smooth and not x.hide and x not in result:
                    result.add(x)
                    stack.append(x)
    return result


class MESH_OT_hb_select_loop(bpy.types.Operator):
    """Select edge loops following the direction of the selected edge(s), up to a maximum turn
angle; works on any topology. If all selected edges are sharp, selects connected sharp edges"""
    bl_idname = "mesh.hb_select_loop"
    bl_label = "Select Loop (Angle)"
    bl_options = {'REGISTER', 'UNDO'}

    max_angle: FloatProperty(name="Max Angle", subtype='ANGLE', min=0.0, max=math.radians(180),
                             default=math.radians(60),
                             description="Largest turn the loop may take at a vertex (higher grows the selection)")
    sharp: BoolProperty(name="Connected Sharps", default=True,
                        description="If every selected edge is marked sharp, select all connected sharp edges")

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        total = 0
        found_any = False
        for obj in context.objects_in_mode:
            if obj.type != 'MESH':
                continue
            bm = bmesh.from_edit_mesh(obj.data)
            start = [e for e in bm.edges if e.select and not e.hide]
            if not start:
                continue
            found_any = True
            if self.sharp and all(not e.smooth for e in start):
                edges = connected_sharp(start)
            else:
                edges = angle_loop(start, self.max_angle)
            for e in edges:
                e.select_set(True)
            total += len(edges)
            bmesh.update_edit_mesh(obj.data, loop_triangles=False, destructive=False)
        if not found_any:
            self.report({'WARNING'}, "Select one or more edges to grow loops from")
            return {'CANCELLED'}
        ts = context.tool_settings
        if not ts.mesh_select_mode[1]:
            # In vertex mode, edges between selected vertices (e.g. diagonals) would show as selected too.
            ts.mesh_select_mode = (False, True, False)
        self.report({'INFO'}, "%d edges selected" % total)
        return {'FINISHED'}


def register():
    bpy.utils.register_class(MESH_OT_hb_select_loop)


def unregister():
    bpy.utils.unregister_class(MESH_OT_hb_select_loop)
