# SPDX-License-Identifier: GPL-2.0-or-later
"""Viewport overlay: drawn in every 3D view regardless of the object mode."""

import math

import blf
import bpy
import gpu
from gpu_extras.batch import batch_for_shader
from mathutils import Vector

from . import geom

# Runtime state shared with the drag operator (not saved in the file).
state = {
    "drag": None,      # dict: index, which, anchor, axis, plane, snap, snap_kind
    "hover": None,     # (index, which) highlighted handle
}

AXIS_COLORS = {'X': (1.0, 0.25, 0.3, 1.0), 'Y': (0.45, 0.85, 0.2, 1.0), 'Z': (0.25, 0.55, 1.0, 1.0)}
LCD_BG = (0.0, 0.0, 0.0, 0.9)
LCD_FG = (1.0, 1.0, 1.0, 1.0)
OUTLINE = (0.0, 0.0, 0.0, 0.6)

HANDLE_RADIUS = 6.0
JAW_LEN = 16.0

_handles = []


def _ui_scale():
    return geom.ui_scale()


# ----------------------------------------------------------------------------
# Primitive helpers

def _lines(coords, color, width, viewport):
    if not coords:
        return
    shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    batch = batch_for_shader(shader, 'LINES', {"pos": [(c[0], c[1], c[2] if len(c) > 2 else 0.0)
                                                       for c in coords]})
    shader.bind()
    shader.uniform_float("viewportSize", viewport)
    shader.uniform_float("lineWidth", width)
    shader.uniform_float("color", color)
    batch.draw(shader)


def _tris(coords, color):
    if not coords:
        return
    shader = gpu.shader.from_builtin('UNIFORM_COLOR')
    batch = batch_for_shader(shader, 'TRIS', {"pos": [(c[0], c[1], 0.0) for c in coords]})
    shader.bind()
    shader.uniform_float("color", color)
    batch.draw(shader)


def _rect(x0, y0, x1, y1, color):
    _tris([(x0, y0), (x1, y0), (x1, y1), (x0, y0), (x1, y1), (x0, y1)], color)


def _rounded_rect(x0, y0, x1, y1, r, color, seg=6):
    r = max(0.0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
    pts = []
    for cx, cy, a0 in ((x1 - r, y1 - r, 0.0), (x0 + r, y1 - r, 90.0),
                       (x0 + r, y0 + r, 180.0), (x1 - r, y0 + r, 270.0)):
        for i in range(seg + 1):
            a = math.radians(a0 + 90.0 * i / seg)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    c = ((x0 + x1) / 2, (y0 + y1) / 2)
    tris = []
    for i in range(len(pts)):
        tris += [c, pts[i], pts[(i + 1) % len(pts)]]
    _tris(tris, color)


def _circle_pts(c, r, seg=20):
    return [(c[0] + r * math.cos(2 * math.pi * i / seg), c[1] + r * math.sin(2 * math.pi * i / seg))
            for i in range(seg)]


def _disc(c, r, color):
    pts = _circle_pts(c, r)
    tris = []
    for i in range(len(pts)):
        tris += [c, pts[i], pts[(i + 1) % len(pts)]]
    _tris(tris, color)


def _ring(c, r, color, width, viewport):
    pts = _circle_pts(c, r)
    segs = []
    for i in range(len(pts)):
        segs += [pts[i], pts[(i + 1) % len(pts)]]
    _lines(segs, color, width, viewport)


def _text(x, y, s, size, color, center=True):
    blf.size(0, size)
    w, h = blf.dimensions(0, s)
    if center:
        x -= w / 2
    blf.position(0, x, y, 0)
    blf.color(0, *color)
    blf.draw(0, s)
    return w, h


# ----------------------------------------------------------------------------
# Main drawing

def draw_measure(context, region, rv3d, idx, m, settings, viewport):
    scale = _ui_scale()
    a2 = geom.to_2d(region, rv3d, Vector(m.start))
    b2 = geom.to_2d(region, rv3d, Vector(m.end))
    if a2 is None or b2 is None:
        return
    start, end = Vector(m.start), Vector(m.end)
    length = (end - start).length
    d2 = b2 - a2
    len2 = d2.length
    if len2 > 1e-3:
        dirv = d2 / len2
    else:
        dirv = Vector((1.0, 0.0))
    n2 = Vector((-dirv.y, dirv.x))
    if n2.y < 0 or (abs(n2.y) < 1e-6 and n2.x < 0):
        n2 = -n2  # ticks point "up" on screen

    col = tuple(settings.color)
    is_active = idx == settings.active_index
    lw = 2.0 * scale if is_active else 1.5 * scale

    segs = [a2, b2]
    jaw = JAW_LEN * scale
    segs += [a2 - n2 * jaw * 0.4, a2 + n2 * jaw, b2 - n2 * jaw * 0.4, b2 + n2 * jaw]

    tick_segs = []
    if settings.show_ticks and len2 > 20:
        step = geom.nice_tick_step(length, len2, context.scene.unit_settings.scale_length, 7.0 * scale)
        if step:
            count = int(length / step + 1e-6)
            for k in range(1, count + 1):
                t = k * step / length
                if t >= 0.999:
                    break
                p = a2 + d2 * t
                h = (9.0 if k % 10 == 0 else 6.0 if k % 5 == 0 else 3.5) * scale
                tick_segs += [p, p + n2 * h]

    _lines(segs, OUTLINE, lw + 2.0 * scale, viewport)
    _lines(segs, col, lw, viewport)
    _lines(tick_segs, col, 1.0 * scale, viewport)

    # Handles.
    for which, p in (('START', a2), ('END', b2)):
        hot = state["hover"] == (idx, which) or (
            state["drag"] and state["drag"]["index"] == idx and state["drag"]["which"] in (which, 'BOTH'))
        r = HANDLE_RADIUS * scale * (1.3 if hot else 1.0)
        _disc(p, r, (1.0, 1.0, 1.0, 0.95) if hot else (0.1, 0.1, 0.1, 0.8))
        _ring(p, r, col, 1.5 * scale, viewport)

    # LCD label (on the side opposite to the ticks).
    text = geom.format_length(context, length)
    size = 15 * scale
    blf.size(0, size)
    tw, th = blf.dimensions(0, text)
    mid = (a2 + b2) / 2 - n2 * (18 * scale + th)
    pad = 5 * scale
    lines_extra = []
    if settings.show_deltas:
        dv = end - start
        for ax, v in zip("XYZ", dv):
            lines_extra.append((ax, geom.format_length(context, abs(v))))
    small = 11 * scale
    extra_h = 0.0
    extra_w = 0.0
    line_adv = small + 4 * scale
    if lines_extra:
        blf.size(0, small)
        for ax, s in lines_extra:
            extra_w = max(extra_w, blf.dimensions(0, "%s %s" % (ax, s))[0])
        extra_h = line_adv * len(lines_extra)
    box_w = max(tw, extra_w) + 2 * pad
    box_h = th + extra_h + 2 * pad
    x0, y1 = mid.x - box_w / 2, mid.y + th + pad
    y0 = y1 - box_h
    _rounded_rect(x0, y0, x0 + box_w, y1, 6 * scale, LCD_BG)
    _text(mid.x, y1 - pad - th, text, size, LCD_FG)
    y = y1 - pad - th
    for ax, s in lines_extra:
        y -= line_adv
        c = AXIS_COLORS[ax]
        _text(mid.x, y, "%s %s" % (ax, s), small, c)


def draw_px(context=None, region=None, rv3d=None):
    context = context or bpy.context
    region = region or context.region
    rv3d = rv3d or context.region_data
    scene = context.scene
    if scene is None or rv3d is None or not hasattr(scene, "caliper"):
        return
    settings = scene.caliper
    if not settings.show:
        return
    viewport = (region.width, region.height)
    gpu.state.blend_set('ALPHA')
    for i, m in enumerate(settings.measures):
        if m.visible:
            draw_measure(context, region, rv3d, i, m, settings, viewport)

    drag = state["drag"]
    if drag and drag.get("snap") is not None:
        p = geom.to_2d(region, rv3d, drag["snap"])
        if p is not None:
            s = _ui_scale()
            r = 9 * s
            _ring(p, r, (1.0, 1.0, 1.0, 1.0), 2.0 * s, viewport)
            _lines([(p.x - r * 1.6, p.y), (p.x - r * 0.5, p.y), (p.x + r * 0.5, p.y), (p.x + r * 1.6, p.y),
                    (p.x, p.y - r * 1.6), (p.x, p.y - r * 0.5), (p.x, p.y + r * 0.5), (p.x, p.y + r * 1.6)],
                   (1.0, 1.0, 1.0, 1.0), 1.5 * s, viewport)
            _text(p.x + r * 1.8, p.y + r, drag.get("snap_kind", ""), 11 * s, (1, 1, 1, 0.9), center=False)
    gpu.state.blend_set('NONE')


def draw_view(context=None, region=None):
    """3D-space pass: the active axis constraint line while dragging."""
    drag = state["drag"]
    if not drag:
        return
    region = region or bpy.context.region
    axes = [drag["axis"]] if drag.get("axis") else []
    if drag.get("plane"):
        axes = [a for a in "XYZ" if a != drag["plane"]]
    if not axes:
        return
    anchor = drag["anchor"]
    gpu.state.blend_set('ALPHA')
    for ax in axes:
        a = geom.AXES[ax] * 1e5
        _lines([tuple(anchor - a), tuple(anchor + a)], AXIS_COLORS[ax], 1.5 * _ui_scale(),
               (region.width, region.height))
    gpu.state.blend_set('NONE')


def register():
    _handles.append((bpy.types.SpaceView3D.draw_handler_add(draw_px, (), 'WINDOW', 'POST_PIXEL'), 'WINDOW'))
    _handles.append((bpy.types.SpaceView3D.draw_handler_add(draw_view, (), 'WINDOW', 'POST_VIEW'), 'WINDOW'))


def unregister():
    for h, region_type in _handles:
        bpy.types.SpaceView3D.draw_handler_remove(h, region_type)
    _handles.clear()
