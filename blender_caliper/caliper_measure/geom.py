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
    """All snappable points (vertices, edge midpoints, face centers, origins,
    other measurement handles), gathered lazily once per drag."""

    KIND_BIAS = {'VERT': 0.0, 'HANDLE': 0.0, 'ORIGIN': 2.0, 'CURSOR': 2.0, 'MID': 4.0, 'FACE': 6.0}

    def __init__(self, context, exclude_handles=()):
        self.points = []   # list of (N,3) arrays
        self.kinds = []    # kind per array
        self._collect(context, set(exclude_handles))
        if self.points:
            self.all_pts = np.concatenate(self.points)
            self.all_bias = np.concatenate([np.full(len(p), self.KIND_BIAS[k])
                                            for p, k in zip(self.points, self.kinds)])
            self.all_kind = np.concatenate([np.full(len(p), i)
                                            for i, p in enumerate(self.points)])
        else:
            self.all_pts = np.zeros((0, 3))

    def _add(self, arr, kind):
        if len(arr):
            self.points.append(np.asarray(arr, dtype=np.float64).reshape(-1, 3))
            self.kinds.append(kind)

    def _collect(self, context, exclude):
        depsgraph = context.evaluated_depsgraph_get()
        origins = []
        for obj in context.visible_objects:
            mw = np.array(obj.matrix_world, dtype=np.float64)
            origins.append(mw[:3, 3])
            if obj.type == 'MESH' and obj.mode == 'EDIT':
                bm = bmesh.from_edit_mesh(obj.data)
                verts = [v.co for v in bm.verts if not v.hide]
                mids = [(e.verts[0].co + e.verts[1].co) * 0.5 for e in bm.edges if not e.hide]
                faces = [f.calc_center_median() for f in bm.faces if not f.hide]
                for data, kind in ((verts, 'VERT'), (mids, 'MID'), (faces, 'FACE')):
                    if data:
                        self._add(_xform(np.array(data), mw), kind)
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
                    co = co.reshape(-1, 3)
                    self._add(_xform(co, mw), 'VERT')
                    if len(me.edges) and len(me.edges) < 2_000_000:
                        ev = np.empty(len(me.edges) * 2, dtype=np.int32)
                        me.edges.foreach_get("vertices", ev)
                        ev = ev.reshape(-1, 2)
                        self._add(_xform((co[ev[:, 0]] + co[ev[:, 1]]) * 0.5, mw), 'MID')
                    if len(me.polygons) and len(me.polygons) < 2_000_000:
                        fc = np.empty(len(me.polygons) * 3)
                        me.polygons.foreach_get("center", fc)
                        self._add(_xform(fc.reshape(-1, 3), mw), 'FACE')
                finally:
                    ob_eval.to_mesh_clear()
        self._add(origins, 'ORIGIN')
        self._add([tuple(context.scene.cursor.location)], 'CURSOR')

        handles = []
        for i, m in enumerate(context.scene.caliper.measures):
            if not m.visible:
                continue
            if (i, 'START') not in exclude:
                handles.append(tuple(m.start))
            if (i, 'END') not in exclude:
                handles.append(tuple(m.end))
        self._add(handles, 'HANDLE')

    def nearest(self, region, rv3d, mouse, radius):
        """Return (Vector, kind) of the best target within radius pixels, or None."""
        if not len(self.all_pts):
            return None
        xy, ok = project_points(region, rv3d, self.all_pts)
        d = np.hypot(xy[:, 0] - mouse[0], xy[:, 1] - mouse[1])
        d = np.where(ok & (d <= radius), d + self.all_bias, np.inf)
        i = int(np.argmin(d))
        if not math.isfinite(d[i]):
            return None
        return Vector(self.all_pts[i]), self.kinds[int(self.all_kind[i])]


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
    settings = scene.caliper
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
