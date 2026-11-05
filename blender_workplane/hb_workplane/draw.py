# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the workplane grid in every 3D view."""

import bpy
import gpu
import numpy as np
from gpu_extras.batch import batch_for_shader

from . import ops, plane

X_COLOR = (1.0, 0.27, 0.32)
Y_COLOR = (0.47, 0.82, 0.18)
Z_COLOR = (0.2, 0.55, 1.0)
SEGMENTS = 12

_handle = None
# Set by the grid move operator while it runs.
state = {"snap_point": None, "snap_step": 0.0, "snap_matrix": None}


def grid_geometry(base, center, step, n, opacity, color):
    """Grid on the plane of matrix `base` (local XY), lines at multiples of `step`
    around the local 2D point `center`. Returns (positions (K,3), colors (K,4)) for LINES."""
    mat = np.array(base, dtype=np.float64)
    cx = round(center[0] / step) * step
    cy = round(center[1] / step) * step
    extent = n * step
    t = np.linspace(0.0, 1.0, SEGMENTS + 1)
    seg = np.repeat(np.arange(SEGMENTS), 2) + np.tile([0, 1], SEGMENTS)
    pos, col = [], []

    def add_line(p0, p1, rgb, alpha, fade_r):
        pts = p0[None, :] + (p1 - p0)[None, :] * t[:, None]
        r = np.hypot(pts[:, 0] - cx, pts[:, 1] - cy) / fade_r
        a = np.clip(1.0 - r * r, 0.0, 1.0) * alpha
        pos.append(pts[seg])
        c = np.empty((len(seg), 4))
        c[:, :3] = rgb
        c[:, 3] = a[seg]
        col.append(c)

    for i in range(-n, n + 1):
        for horizontal in (True, False):
            u = (cy if horizontal else cx) + i * step
            k = int(round(u / step))
            if k == 0:
                continue  # axis line, drawn below
            a = opacity * (1.7 if k % 10 == 0 else 1.0)
            if horizontal:
                add_line(np.array([cx - extent, u, 0.0]), np.array([cx + extent, u, 0.0]), color, a, extent + step)
            else:
                add_line(np.array([u, cy - extent, 0.0]), np.array([u, cy + extent, 0.0]), color, a, extent + step)

    e = extent * 1.5
    axis_a = min(1.0, opacity * 2.5)
    add_line(np.array([cx - e, 0.0, 0.0]), np.array([cx + e, 0.0, 0.0]), X_COLOR, axis_a, e)
    add_line(np.array([0.0, cy - e, 0.0]), np.array([0.0, cy + e, 0.0]), Y_COLOR, axis_a, e)

    p = np.concatenate(pos)
    return p @ mat[:3, :3].T + mat[:3, 3], np.concatenate(col)


def grid_frame(context, rv3d):
    """(base matrix, local 2D center) for the grid: custom plane at its origin, or the
    automatic world plane through the world origin centered under the view focus."""
    s = context.scene.hb_workplane
    m = ops.current_matrix(context, rv3d)
    if s.mode == 'CUSTOM':
        return m, (0.0, 0.0)
    base = m.copy()
    base.translation = (0.0, 0.0, 0.0)
    local = base.to_3x3().transposed() @ m.translation
    return base, (local.x, local.y)


def grid_step(context, rv3d, density=None):
    s = context.scene.hb_workplane
    if s.spacing_mode == 'FIXED':
        return s.spacing
    if density is None:
        density = s.grid_density
    us = context.scene.unit_settings
    scale = us.scale_length if us.system != 'NONE' else 1.0
    raw = max(rv3d.view_distance, 1e-6) / (8.0 * density) * scale
    return plane.nice_step(raw) / scale


def grid_line_count(context, rv3d, step):
    """Lines per side so the grid covers the same area whatever the density."""
    s = context.scene.hb_workplane
    base = grid_step(context, rv3d, density=1.0)
    return max(1, min(400, int(round(s.grid_lines * base / step))))


def draw_view(context=None, region=None, rv3d=None):
    context = context or bpy.context
    scene = context.scene
    if scene is None or not hasattr(scene, "hb_workplane"):
        return
    s = scene.hb_workplane
    if not (s.enabled and s.show_grid):
        return
    if s.mode == 'AUTO' and not s.show_auto_grid:
        return
    region = region or context.region
    rv3d = rv3d or context.region_data
    if rv3d is None:
        return
    m, center = grid_frame(context, rv3d)
    step = grid_step(context, rv3d)
    pos, col = grid_geometry(m, center, step, grid_line_count(context, rv3d, step),
                             s.grid_opacity, tuple(s.grid_color))

    shader = gpu.shader.from_builtin('POLYLINE_SMOOTH_COLOR')
    batch = batch_for_shader(shader, 'LINES', {"pos": pos.astype(np.float32).tolist(),
                                               "color": col.astype(np.float32).tolist()})
    gpu.state.blend_set('ALPHA')
    if s.occlude:
        gpu.state.depth_test_set('LESS_EQUAL')
    gpu.state.depth_mask_set(False)
    shader.bind()
    shader.uniform_float("viewportSize", (region.width, region.height))
    shader.uniform_float("lineWidth", 1.0)
    batch.draw(shader)

    if s.mode == 'CUSTOM':  # normal indicator
        mat = np.array(m)
        o = mat[:3, 3]
        n = mat[:3, 2]
        nb = batch_for_shader(shader, 'LINES', {"pos": [tuple(o), tuple(o + n * step * 2)],
                                                 "color": [(*Z_COLOR, 0.9), (*Z_COLOR, 0.0)]})
        shader.uniform_float("lineWidth", 2.0)
        nb.draw(shader)

    sp = state["snap_point"]
    if sp is not None and state["snap_matrix"] is not None:  # snapped point marker
        rot = state["snap_matrix"].to_3x3().normalized()
        r = max(state["snap_step"], 1e-6) * 0.6
        gpu.state.depth_test_set('NONE')
        pts, cols = [], []
        for ax, c in ((rot.col[0], X_COLOR), (rot.col[1], Y_COLOR)):
            pts += [tuple(sp - ax * r), tuple(sp + ax * r)]
            cols += [(1, 1, 1, 1), (1, 1, 1, 1)]
        mb = batch_for_shader(shader, 'LINES', {"pos": pts, "color": cols})
        shader.uniform_float("lineWidth", 2.5)
        mb.draw(shader)

    gpu.state.depth_test_set('NONE')
    gpu.state.depth_mask_set(True)
    gpu.state.blend_set('NONE')


def register():
    global _handle
    _handle = bpy.types.SpaceView3D.draw_handler_add(draw_view, (), 'WINDOW', 'POST_VIEW')


def unregister():
    global _handle
    if _handle is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_handle, 'WINDOW')
        _handle = None
# Set by the grid move operator while it runs.
state = {"snap_point": None, "snap_step": 0.0, "snap_matrix": None}
