# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn Corner: redirect the chamfer flow on a quad corner (where three chamfers meet).

Same idea as MESHmachine's Turn Corner. The corner quad has two inner vertices (3 edges,
c1 and c2, joined by the inner edge) and two outer ones (4 edges, c3 next to c1, c4 next to c2).
The outer vertex c3 is split into two new vertices along its two outward edges, the three faces
around it are rebuilt, a new corner quad [c1, a, b, c4] is made, and c1 + c2 are merged where
their outward edges meet. The chamfer that used to end at the corner now flows through it.
Repeating it turns the corner further.
"""

import bmesh
import bpy
from bpy.props import FloatProperty, IntProperty
from mathutils import Vector
from mathutils.geometry import intersect_line_line


class CornerError(Exception):
    pass


def quad_corner_parts(face):
    """(c1, c2, c3, c4) of a quad corner face, with c1 -> c2 in face winding order."""
    if len(face.verts) != 4:
        raise CornerError("Select a single quad corner (a quad where three chamfers meet)")
    inner = [v for v in face.verts if len(v.link_edges) == 3]
    outer = [v for v in face.verts if len(v.link_edges) == 4]
    if len(inner) != 2 or len(outer) != 2:
        raise CornerError("Not a quad corner: needs two vertices with 3 edges and two with 4")
    loops = {l.vert: l for l in face.loops}
    c1, c2 = inner
    if loops[c1].link_loop_next.vert is not c2:
        c1, c2 = c2, c1
        if loops[c1].link_loop_next.vert is not c2:
            raise CornerError("The two 3-edge vertices of the quad aren't connected")
    c4 = loops[c2].link_loop_next.vert
    c3 = loops[c4].link_loop_next.vert
    return c1, c2, c3, c4


def _edge(a, b):
    for e in a.link_edges:
        if e.other_vert(a) is b:
            return e
    return None


def _other_face(edge, face):
    fs = [f for f in edge.link_faces if f is not face]
    if len(fs) != 1:
        raise CornerError("The corner must be surrounded by faces (manifold)")
    return fs[0]


def auto_width(c1, c2, c3, c4):
    return ((c3.co - c1.co).length + (c4.co - c2.co).length) / 2


def split_vertex(bm, face, v, width):
    """Split vertex v of `face` into two new vertices placed `width` along v's two edges that
    lead away from the face; the face gains a side, the three neighbouring faces are rebuilt.
    Returns the rebuilt face (the widened corner)."""
    loop = next((l for l in face.loops if l.vert is v), None)
    if loop is None:
        raise CornerError("Vertex not in face")
    e_p, e_n = loop.link_loop_prev.edge, loop.edge          # face edges at v (to prev / next vertex)
    face_p, face_n = _other_face(e_p, face), _other_face(e_n, face)
    out = [e for e in v.link_edges if e is not e_p and e is not e_n]
    if len(out) != 2 or len(v.link_faces) != 4:
        raise CornerError("The corner vertex must have exactly two outward edges (4 faces around it)")
    o_p = next((e for e in out if face_p in e.link_faces), None)
    o_n = next((e for e in out if face_n in e.link_faces), None)
    if o_p is None or o_n is None or o_p is o_n:
        raise CornerError("Unexpected topology around the corner")
    face_far = next(f for f in v.link_faces if f not in (face, face_p, face_n))

    def along(e):
        d = e.other_vert(v).co - v.co
        return v.co + d.normalized() * min(width, d.length * 0.95)

    new_p, new_n = bm.verts.new(along(o_p)), bm.verts.new(along(o_n))
    side = {e_p: new_p, o_p: new_p, e_n: new_n, o_n: new_n}

    def rebuilt(f):
        verts = []
        for l in f.loops:
            if l.vert is v:
                a, b = side[l.link_loop_prev.edge], side[l.edge]
                verts.extend([a] if a is b else [a, b])
            else:
                verts.append(l.vert)
        return verts

    old = [face, face_p, face_n, face_far]
    specs = [(rebuilt(f), f.material_index, f.smooth) for f in old]
    bmesh.ops.delete(bm, geom=old, context='FACES_ONLY')
    new_faces = []
    for verts, mat, smooth in specs:
        f = bm.faces.new(verts)
        f.material_index = mat
        f.smooth = smooth
        new_faces.append(f)
    bm.verts.remove(v)  # also removes its now face-less edges
    bmesh.ops.recalc_face_normals(bm, faces=new_faces)
    return new_faces[0]


def turn_corner(bm, face, width=0.0):
    """Turn one quad corner. Returns the new corner face."""
    c1, c2, c3, c4 = quad_corner_parts(face)
    if width <= 0:
        width = auto_width(c1, c2, c3, c4)
    e12, e13, e24 = _edge(c1, c2), _edge(c1, c3), _edge(c2, c4)
    # Merge point of the two inner vertices: where their outward edges meet.
    r1 = next(e for e in c1.link_edges if e is not e12 and e is not e13).other_vert(c1)
    r2 = next(e for e in c2.link_edges if e is not e12 and e is not e24).other_vert(c2)
    hit = intersect_line_line(c1.co, r1.co, c2.co, r2.co)
    merge_co = (hit[0] + hit[1]) / 2 if hit else (c1.co + c2.co) / 2

    new_corner = split_vertex(bm, face, c3, width)
    bmesh.ops.pointmerge(bm, verts=[c1, c2], merge_co=merge_co)
    new_corner = new_corner if new_corner.is_valid else None
    if new_corner is not None:
        bmesh.ops.recalc_face_normals(bm, faces=list({f for v in new_corner.verts for f in v.link_faces}))
    return new_corner


def quad_from_tri_corner(bm, face, corner=0, width=0.0):
    """Turn a triangle corner (Blender's 1-segment bevel corner) into a quad corner by
    splitting one of its vertices. `corner` (0-2) picks the vertex, i.e. the flow direction."""
    if len(face.verts) != 3:
        raise CornerError("Not a triangle corner")
    if width <= 0:
        width = sum(e.calc_length() for e in face.edges) / 3
    v = list(face.verts)[corner % 3]
    return split_vertex(bm, face, v, width)


class MESH_OT_hb_turn_corner(bpy.types.Operator):
    """Fix / redirect the chamfer flow where three chamfers meet. Select the corner face:
a triangle (Blender's 1-segment bevel corner) becomes a quad corner, a quad corner is turned.
Run again to keep turning"""
    bl_idname = "mesh.hb_turn_corner"
    bl_label = "Turn Corner"
    bl_options = {'REGISTER', 'UNDO'}

    width: FloatProperty(name="Width", default=0.0, min=0.0, subtype='DISTANCE', unit='LENGTH', step=0.1,
                         description="Width of the turned chamfer; 0 = same as the existing chamfer")
    count: IntProperty(name="Turns", default=1, min=1, max=2,
                       description="How many times to turn the corner")
    corner: IntProperty(name="Corner", default=0, min=0, max=2,
                        description="Triangle corners: which of the 3 vertices is split (sets the flow direction)")

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        obj = context.edit_object
        bm = bmesh.from_edit_mesh(obj.data)
        faces = [f for f in bm.faces if f.select]
        if len(faces) != 1:
            self.report({'WARNING'}, "Select exactly one face: the corner where three chamfers meet")
            return {'CANCELLED'}
        face = faces[0]
        try:
            if len(face.verts) == 3:
                face = quad_from_tri_corner(bm, face, self.corner, self.width)
            else:
                for _ in range(self.count):
                    face = turn_corner(bm, face, self.width)
                    if face is None:
                        break
        except CornerError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        for g in list(bm.verts) + list(bm.edges) + list(bm.faces):
            g.select = False
        if face is not None:
            face.select_set(True)
        bm.normal_update()
        bmesh.update_edit_mesh(obj.data)
        return {'FINISHED'}


def register():
    bpy.utils.register_class(MESH_OT_hb_turn_corner)


def unregister():
    bpy.utils.unregister_class(MESH_OT_hb_turn_corner)
