# SPDX-License-Identifier: GPL-2.0-or-later
"""View projection, snapping and unit formatting helpers (no drawing)."""

import math

import bmesh
import numpy as np
from mathutils import Vector
from mathutils.geometry import intersect_line_line, intersect_line_plane
from bpy_extras import view3d_utils

AXES = {'X': Vector((1, 0, 0)), 'Y': Vector((0, 1, 0)), 'Z': Vector((0, 0, 1))}

_UNIT_FACTORS = {  # meters per unit, suffix
    'KILOMETERS': (1000.0, "km"), 'METERS': (1.0, "m"), 'CENTIMETERS': (0.01, "cm"),
    'MILLIMETERS': (0.001, "mm"), 'MICROMETERS': (1e-6, "µm"),
    'MILES': (1609.344, "mi"), 'FEET': (0.3048, "ft"), 'INCHES': (0.0254, "in"),
    'THOU': (0.0000254, "thou"),
}


def ui_scale(context=None):
    import bpy
    try:
        v = (context or bpy.context).preferences.system.ui_scale
    except AttributeError:
        v = 1.0
    return v if v > 0 else 1.0


# ----------------------------------------------------------------------------
# Projection

def to_2d(region, rv3d, co):
    return view3d_utils.location_3d_to_region_2d(region, rv3d, co)


def mouse_ray(region, rv3d, mouse):
    origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, mouse)
    direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, mouse)
    return origin, direction


def on_view_plane(region, rv3d, mouse, depth_co):
    return view3d_utils.region_2d_to_location_3d(region, rv3d, mouse, depth_co)


def project_points(region, rv3d, pts):
    """pts: (N,3) world -> (N,2) region pixels and a mask of points in front of the view."""
    m = np.array(rv3d.perspective_matrix, dtype=np.float64)
    clip = pts @ m[:3, :3].T + m[:3, 3]
    w = pts @ m[3, :3] + m[3, 3]
    ok = w > 1e-6
    w = np.where(ok, w, 1.0)
    xy = np.empty((len(pts), 2))
    xy[:, 0] = (clip[:, 0] / w + 1.0) * 0.5 * region.width
    xy[:, 1] = (clip[:, 1] / w + 1.0) * 0.5 * region.height
    return xy, ok


# ----------------------------------------------------------------------------
# Constraints

def constrain(anchor, target, axis=None, plane=None):
    """Project an exact 3D target onto the axis line / plane through anchor."""
    if axis is not None:
        a = AXES[axis]
        return anchor + a * (target - anchor).dot(a)
    if plane is not None:
        n = AXES[plane]
        return target - n * (target - anchor).dot(n)
    return target.copy()


def ray_to_constraint(origin, direction, anchor, axis=None, plane=None):
    """Point on the axis line / plane through anchor under the mouse ray (or None)."""
    if axis is not None:
        a = AXES[axis]
        res = intersect_line_line(anchor, anchor + a, origin, origin + direction)
        return res[0] if res else None
    if plane is not None:
        n = AXES[plane]
        if abs(direction.dot(n)) < 1e-6:
            return None
        return intersect_line_plane(origin, origin + direction, anchor, n)
    return None


# ----------------------------------------------------------------------------
# Snap targets

class SnapTargets:
    """Snappable geometry, gathered once per drag: points (vertices, edge
    midpoints, origins, 3D cursor, other measurement handles) and edges."""

    KIND_BIAS = {'VERT': 0.0, 'HANDLE': 0.0, 'ORIGIN': 2.0, 'CURSOR': 2.0, 'MID': 3.0}
    MAX_VIS_CHECKS = 24

    def __init__(self, context, exclude_handles=()):
        self.context = context
        self.points = []   # list of (N,3) arrays
        self.kinds = []    # kind per array
        self.edge_list = []  # list of (M,2,3) arrays
        self._collect(context, set(exclude_handles))
        if self.points:
            self.all_pts = np.concatenate(self.points)
            self.all_bias = np.concatenate([np.full(len(p), self.KIND_BIAS[k])
                                            for p, k in zip(self.points, self.kinds)])
            self.all_kind = np.concatenate([np.full(len(p), i)
                                            for i, p in enumerate(self.points)])
        else:
            self.all_pts = np.zeros((0, 3))
        self.edges = np.concatenate(self.edge_list) if self.edge_list else np.zeros((0, 2, 3))

    def _add(self, arr, kind):
        if len(arr):
            self.points.append(np.asarray(arr, dtype=np.float64).reshape(-1, 3))
            self.kinds.append(kind)

    def _add_edges(self, a, b):
        if len(a):
            self.edge_list.append(np.stack([a, b], axis=1))
            self._add((a + b) * 0.5, 'MID')

    def _collect(self, context, exclude):
        depsgraph = context.evaluated_depsgraph_get()
        origins = []
        for obj in context.visible_objects:
            mw = np.array(obj.matrix_world, dtype=np.float64)
            origins.append(mw[:3, 3])
            if obj.type == 'MESH' and obj.mode == 'EDIT':
                bm = bmesh.from_edit_mesh(obj.data)
                verts = [v.co for v in bm.verts if not v.hide]
                if verts:
                    self._add(_xform(np.array(verts), mw), 'VERT')
                edges = [(e.verts[0].co, e.verts[1].co) for e in bm.edges if not e.hide]
                if edges:
                    e = np.array(edges, dtype=np.float64)
                    self._add_edges(_xform(e[:, 0], mw), _xform(e[:, 1], mw))
            elif obj.type in {'MESH', 'CURVE', 'SURFACE', 'FONT', 'META'}:
                ob_eval = obj.evaluated_get(depsgraph)
                try:
                    me = ob_eval.to_mesh()
                except RuntimeError:
                    continue
                if me is None:
                    continue
                try:
                    co = np.empty(len(me.vertices) * 3)
                    me.vertices.foreach_get("co", co)
                    co = _xform(co.reshape(-1, 3), mw)
                    self._add(co, 'VERT')
                    if len(me.edges):
                        ev = np.empty(len(me.edges) * 2, dtype=np.int32)
                        me.edges.foreach_get("vertices", ev)
                        ev = ev.reshape(-1, 2)
                        self._add_edges(co[ev[:, 0]], co[ev[:, 1]])
                finally:
                    ob_eval.to_mesh_clear()
        self._add(origins, 'ORIGIN')
        self._add([tuple(context.scene.cursor.location)], 'CURSOR')

        handles = []
        for i, m in enumerate(context.scene.hb_caliper.measures):
            if not m.visible:
                continue
            if (i, 'START') not in exclude:
                handles.append(tuple(m.start))
            if (i, 'END') not in exclude:
                handles.append(tuple(m.end))
        self._add(handles, 'HANDLE')

    def _visible(self, region, rv3d, co, xray):
        if xray:
            return True
        p2 = to_2d(region, rv3d, co)
        if p2 is None:
            return False
        origin, direction = mouse_ray(region, rv3d, p2)
        dist = (co - origin).length
        hit, loc, *_ = self.context.scene.ray_cast(self.context.evaluated_depsgraph_get(), origin, direction)
        return not hit or (loc - origin).length >= dist - (1e-4 * dist + 1e-5)

    def nearest(self, region, rv3d, mouse, radius, xray=False):
        """Closest visible snap target within radius pixels: a vertex/point, or the
        closest point along an edge. Points win when they're about as close as
        the edge (so corners and midpoints stay easy to hit). Returns (Vector, kind) or None."""
        point = self._nearest_point(region, rv3d, mouse, radius, xray)
        edge = self._nearest_edge(region, rv3d, mouse, radius * 0.75, xray)
        if point and (not edge or point[2] <= 8.0 or point[2] <= edge[2] + 4.0):
            return point[0], point[1]
        if edge:
            return edge[0], 'EDGE'
        return None

    def _nearest_point(self, region, rv3d, mouse, radius, xray):
        if not len(self.all_pts):
            return None
        mx, my = mouse[0], mouse[1]
        xy, ok = project_points(region, rv3d, self.all_pts)
        raw = np.hypot(xy[:, 0] - mx, xy[:, 1] - my)
        d = np.where(ok & (raw <= radius), raw + self.all_bias, np.inf)
        for i in np.argsort(d)[:self.MAX_VIS_CHECKS]:
            if not math.isfinite(d[i]):
                break
            co = Vector(self.all_pts[i])
            if self._visible(region, rv3d, co, xray):
                return co, self.kinds[int(self.all_kind[i])], float(d[i])
        return None

    def _nearest_edge(self, region, rv3d, mouse, radius, xray):
        if not len(self.edges):
            return None
        mx, my = mouse[0], mouse[1]
        xy, ok = project_points(region, rv3d, self.edges.reshape(-1, 3))
        a, b = xy[0::2], xy[1::2]
        ok = ok[0::2] & ok[1::2]
        ab = b - a
        l2 = np.einsum('ij,ij->i', ab, ab)
        t = np.clip(((mx - a[:, 0]) * ab[:, 0] + (my - a[:, 1]) * ab[:, 1]) / np.maximum(l2, 1e-12), 0, 1)
        p = a + ab * t[:, None]
        d = np.hypot(p[:, 0] - mx, p[:, 1] - my)
        d = np.where(ok & (d <= radius), d, np.inf)
        origin, direction = mouse_ray(region, rv3d, Vector((mx, my)))
        for i in np.argsort(d)[:self.MAX_VIS_CHECKS]:
            if not math.isfinite(d[i]):
                break
            e0, e1 = Vector(self.edges[i, 0]), Vector(self.edges[i, 1])
            seg = e1 - e0
            res = intersect_line_line(e0, e1, origin, origin + direction) if seg.length_squared > 1e-18 else None
            if res:
                tt = (res[0] - e0).dot(seg) / seg.length_squared
                co = e0 + seg * min(1.0, max(0.0, tt))
            else:
                co = e0.lerp(e1, float(t[i]))
            if self._visible(region, rv3d, co, xray):
                return co, 'EDGE', float(d[i])
        return None


def _xform(co, mw):
    return co @ mw[:3, :3].T + mw[:3, 3]


def surface_hit(context, region, rv3d, mouse):
    origin, direction = mouse_ray(region, rv3d, mouse)
    hit, loc, *_ = context.scene.ray_cast(context.evaluated_depsgraph_get(), origin, direction)
    return loc if hit else None


# ----------------------------------------------------------------------------
# Units

def format_length(context, value_bu):
    """Format a length given in Blender units according to the add-on/scene settings."""
    scene = context.scene
    settings = scene.hb_caliper
    us = scene.unit_settings
    prec = settings.precision
    meters = value_bu * us.scale_length

    unit = settings.unit
    if unit == 'SCENE':
        if us.system == 'NONE':
            return "%.*f" % (prec, value_bu)
        unit = us.length_unit
        if unit == 'ADAPTIVE':
            import bpy
            return bpy.utils.units.to_string(us.system, 'LENGTH', meters, precision=prec)
    factor, suffix = _UNIT_FACTORS.get(unit, (1.0, "m"))
    return "%.*f %s" % (prec, meters / factor, suffix)


def nice_tick_step(length_bu, length_px, scale_length, min_px=7.0):
    """World step (Blender units) for ruler ticks: 1/2/5 x 10^n meters, at least min_px apart."""
    if length_px < 1 or length_bu <= 0:
        return None
    meters_per_px = length_bu * scale_length / length_px
    raw = meters_per_px * min_px
    exp = math.floor(math.log10(raw))
    for m in (1, 2, 5, 10):
        step = m * 10 ** exp
        if step >= raw:
            return step / scale_length
    return 10 ** (exp + 1) / scale_length
