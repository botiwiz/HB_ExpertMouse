# SPDX-License-Identifier: GPL-2.0-or-later
"""Snapping onto the surfaces being retopologized (any number of visible mesh objects)."""

import bpy
from bpy_extras import view3d_utils
from mathutils import Vector
from mathutils.bvhtree import BVHTree


class Surfaces:
    """BVH trees of the target objects, queried in world space."""

    def __init__(self, context, exclude=()):
        depsgraph = context.evaluated_depsgraph_get()
        self.items = []
        for obj in context.visible_objects:
            if obj.type != 'MESH' or obj in exclude or obj.name in {o.name for o in exclude}:
                continue
            try:
                tree = BVHTree.FromObject(obj, depsgraph)
            except Exception:
                continue
            mw = obj.matrix_world.copy()
            self.items.append((tree, mw, mw.inverted_safe(), mw.to_3x3().inverted_safe().transposed()))

    def __bool__(self):
        return bool(self.items)

    def ray_cast(self, origin, direction):
        """Closest hit along a world-space ray: (location, normal) or (None, None)."""
        best = (None, None, float("inf"))
        for tree, mw, inv, nmat in self.items:
            o = inv @ origin
            d = (inv.to_3x3() @ direction).normalized()
            loc, nrm, _i, _dist = tree.ray_cast(o, d)
            if loc is None:
                continue
            wloc = mw @ loc
            dist = (wloc - origin).length
            if dist < best[2]:
                best = (wloc, (nmat @ nrm).normalized(), dist)
        return best[0], best[1]

    def nearest(self, co):
        """Closest surface point to a world-space location: (location, normal) or (co, None)."""
        best = (co, None, float("inf"))
        for tree, mw, inv, nmat in self.items:
            loc, nrm, _i, _dist = tree.find_nearest(inv @ co)
            if loc is None:
                continue
            wloc = mw @ loc
            dist = (wloc - co).length
            if dist < best[2]:
                best = (wloc, (nmat @ nrm).normalized(), dist)
        return best[0], best[1]

    def mouse_hit(self, region, rv3d, mouse):
        origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, mouse)
        direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, mouse)
        return self.ray_cast(origin, direction)
