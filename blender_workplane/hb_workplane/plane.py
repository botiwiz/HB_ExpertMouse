# SPDX-License-Identifier: GPL-2.0-or-later
"""Workplane math: build plane matrices from selections, hits, the view."""

import math

import bmesh
import numpy as np
from mathutils import Matrix, Vector

WORLD = {'X': Vector((1, 0, 0)), 'Y': Vector((0, 1, 0)), 'Z': Vector((0, 0, 1))}


def make_matrix(origin, normal, x_hint):
    """4x4 matrix whose Z is the plane normal and X follows x_hint (projected)."""
    z = Vector(normal).normalized()
    x = Vector(x_hint) - z * Vector(x_hint).dot(z) if x_hint is not None else Vector()
    if x.length < 1e-6:
        # Pick the world axis most perpendicular to the normal.
        ref = min((WORLD['X'], WORLD['Y'], WORLD['Z']), key=lambda a: abs(a.dot(z)))
        x = ref - z * ref.dot(z)
    x.normalize()
    y = z.cross(x).normalized()
    m = Matrix((x, y, z)).transposed().to_4x4()
    m.translation = origin
    return m


def world_x_hint(normal):
    """World axis (X preferred, then Y, then Z) projected onto the plane."""
    z = Vector(normal).normalized()
    for ax in ('X', 'Y', 'Z'):
        a = WORLD[ax]
        if abs(a.dot(z)) < 0.9:
            return a
    return WORLD['X']


def auto_matrix(view_rotation, bias, center=Vector()):
    """World plane (XY / XZ / YZ) that faces the view most; bias favours the XY (ground) plane."""
    view_dir = view_rotation @ Vector((0, 0, -1))
    scores = {
        'X': abs(view_dir.x),
        'Y': abs(view_dir.y),
        'Z': abs(view_dir.z) * (1.0 + 2.0 * bias),
    }
    axis = max(scores, key=scores.get)
    if axis == 'Z':
        m = Matrix.Identity(4)
    elif axis == 'Y':  # XZ plane, normal Y
        m = Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0))).transposed().to_4x4()
    else:              # YZ plane, normal X
        m = Matrix(((0, 1, 0), (0, 0, 1), (1, 0, 0))).transposed().to_4x4()
    m.translation = center
    return m, axis


def _normal_matrix(mw):
    return mw.to_3x3().inverted_safe().transposed()


def _longest_edge_dir(edges_world):
    best, best_len = None, -1.0
    for a, b in edges_world:
        d = b - a
        if d.length > best_len:
            best, best_len = d, d.length
    return best


def from_edit_selection(objects, view_dir, x_mode='EDGE'):
    """Plane from the selected elements of meshes in Edit Mode, or None.

    Faces: active face (or area-weighted average normal of all selected faces).
    3+ vertices without faces: best-fit plane. Edge / 2 vertices: plane containing the
    edge that faces the view. 1 vertex: its normal."""
    faces, verts, edges = [], [], []
    active_face = None
    for obj in objects:
        if obj.type != 'MESH':
            continue
        mw = obj.matrix_world
        nm = _normal_matrix(mw)
        bm = bmesh.from_edit_mesh(obj.data)
        af = bm.faces.active
        for f in bm.faces:
            if f.select:
                item = (nm @ f.normal, f.calc_area(), mw @ f.calc_center_median(),
                        [(mw @ e.verts[0].co, mw @ e.verts[1].co) for e in f.edges],
                        mw.to_3x3() @ f.normal)
                faces.append(item)
                if f is af:
                    active_face = item
        verts += [(mw @ v.co, (nm @ v.normal).normalized()) for v in bm.verts if v.select]
        edges += [(mw @ e.verts[0].co, mw @ e.verts[1].co) for e in bm.edges if e.select]

    if faces:
        if active_face is None and len(faces) == 1:
            active_face = faces[0]
        if active_face is not None and len(faces) == 1:
            normal = active_face[0]
            origin = active_face[2]
        else:
            normal = sum((f[0].normalized() * max(f[1], 1e-12) for f in faces), Vector())
            if normal.length < 1e-9:
                normal = (active_face or faces[0])[0]
            origin = sum((v[0] for v in verts), Vector()) / max(1, len(verts))
        src = active_face or faces[0]
        hint = _longest_edge_dir(src[3]) if x_mode == 'EDGE' else world_x_hint(normal)
        return make_matrix(origin, normal, hint)

    if len(verts) >= 3:
        pts = np.array([v[0] for v in verts])
        c = pts.mean(axis=0)
        _u, s, vt = np.linalg.svd(pts - c)
        if s[1] > 1e-9:  # not collinear
            normal = Vector(vt[2])
            if normal.dot(view_dir) > 0:
                normal = -normal
            hint = _longest_edge_dir(edges) if (x_mode == 'EDGE' and edges) else \
                (Vector(vt[0]) if x_mode == 'EDGE' else world_x_hint(normal))
            return make_matrix(Vector(c), normal, hint)

    if len(verts) >= 2:
        a, b = verts[0][0], verts[-1][0]
        if edges:
            a, b = edges[0]
        x = b - a
        if x.length > 1e-9:
            x.normalize()
            # Plane containing the edge, turned as much towards the viewer as possible.
            normal = -view_dir + x * view_dir.dot(x)
            if normal.length < 1e-6:
                normal = x.orthogonal()
            return make_matrix((a + b) / 2, normal, x if x_mode == 'EDGE' else world_x_hint(normal))

    if len(verts) == 1:
        co, n = verts[0]
        if n.length < 1e-6:
            n = -view_dir
        return make_matrix(co, n, world_x_hint(n))
    return None


def from_hit(context, location, normal, obj, face_index, x_mode='EDGE'):
    """Plane on the face under the mouse (from scene.ray_cast)."""
    hint = None
    if x_mode == 'EDGE' and obj is not None and face_index >= 0:
        ob_eval = obj.evaluated_get(context.evaluated_depsgraph_get())
        try:
            me = ob_eval.to_mesh()
        except RuntimeError:
            me = None
        if me is not None:
            try:
                if face_index < len(me.polygons):
                    poly = me.polygons[face_index]
                    mw = obj.matrix_world
                    vs = [mw @ me.vertices[i].co for i in poly.vertices]
                    hint = _longest_edge_dir([(vs[i], vs[(i + 1) % len(vs)]) for i in range(len(vs))])
            finally:
                ob_eval.to_mesh_clear()
    if hint is None:
        hint = world_x_hint(normal)
    return make_matrix(location, normal, hint)


def same_plane(a, b, eps=1e-4):
    """True if two plane matrices describe the same plane (normal and position)."""
    na, nb = a.col[2].to_3d().normalized(), b.col[2].to_3d().normalized()
    if abs(abs(na.dot(nb)) - 1.0) > eps:
        return False
    return abs((b.translation - a.translation).dot(na)) < eps * max(1.0, a.translation.length)


def nice_step(raw):
    """1/2/5 x 10^n >= raw."""
    if raw <= 0:
        return 1.0
    e = math.floor(math.log10(raw))
    for m in (1, 2, 5, 10):
        if m * 10 ** e >= raw:
            return m * 10 ** e
    return 10 ** (e + 1)
