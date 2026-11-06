# SPDX-License-Identifier: GPL-2.0-or-later
"""Retopo Pen: draw an outline on the surface (corners, curves, freehand), close it, get a quad patch."""

import blf
import bmesh
import bpy
import gpu
import numpy as np
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty
from bpy_extras import view3d_utils
from gpu_extras.batch import batch_for_shader
from mathutils import Vector

from . import patch, surface

RETOPO_NAME = "Retopo"
LAYOUT_ITEMS = (('AUTO', "Auto", "4 corners: grid; otherwise one quad region per corner around a centre pole"),
                ('GRID', "Grid", "Grid (4 corners only, else centre pole)"),
                ('POLE', "Centre Pole", "One quad region per corner around a centre pole"))
DENSITY_ITEMS = (('SEGMENTS', "Segments", "Density as a number of segments along the longer sides"),
                 ('LENGTH', "Edge Length", "Density as a target edge length"))
CONFORM_ITEMS = (('SURFACE', "Surface", "Snap the patch onto the surfaces underneath"),
                 ('NONE', "Flat", "Keep the patch where the outline puts it (no projection inside)"))


def _patch_props():
    return dict(
        resolution=IntProperty(name="Resolution", default=8, min=1, max=200,
                               description="Segments along the longer sides (mouse wheel)"),
        density_mode=EnumProperty(name="Density", items=DENSITY_ITEMS, default='SEGMENTS'),
        edge_length=FloatProperty(name="Edge Length", default=0.1, min=1e-5, subtype='DISTANCE', unit='LENGTH',
                                  description="Target edge length (Density: Edge Length)"),
        relax=IntProperty(name="Relax", default=3, min=0, max=100, description="Relax passes on the surface"),
        side_smooth=IntProperty(name="Side Smoothing", default=0, min=0, max=100,
                                description="Smooth the outline's sides (not border-following ones)"),
        layout=EnumProperty(name="Layout", items=LAYOUT_ITEMS, default='AUTO'),
        conform=EnumProperty(name="Conform", items=CONFORM_ITEMS, default='SURFACE'),
        strength=FloatProperty(name="Strength", default=1.0, min=0.0, max=1.0, subtype='FACTOR',
                               description="How strongly the patch snaps onto the surface"),
        offset=FloatProperty(name="Offset", default=0.0, subtype='DISTANCE', unit='LENGTH',
                             description="Float the patch above (or below) the surface"),
        projection=EnumProperty(name="Projection", items=(
            ('NORMAL', "Along Normal", "Move points straight onto the surface along the patch's facing "
                                       "direction (no sideways sliding off bumps)"),
            ('NEAREST', "Nearest", "Closest point on the surface (better for patches wrapping far around)")),
            default='NORMAL'),
        subdiv=IntProperty(name="Subdivision", default=0, min=0, max=4,
                           description="Split every edge 2, 4, 8 or 16 times (new vertices go onto the surface) "
                                       "so the edges follow the surface; the layout stays the same"),
        subdiv_auto=BoolProperty(name="Auto", default=False,
                                 description="Pick the lowest subdivision level whose edges stay within "
                                             "Tolerance of the surface"),
        subdiv_tol=FloatProperty(name="Tolerance", default=0.02, min=0.001, max=0.5, subtype='FACTOR',
                                 description="Auto subdivision: largest allowed gap between an edge and the "
                                             "surface, as a fraction of the base edge length"),
    )


def _settings_props():
    d = _patch_props()
    d["snap_px"] = IntProperty(name="Snap Radius", default=14, min=2, max=100, subtype='PIXEL',
                               description="Snap to existing retopo vertices and close the loop within this distance")
    return d


PATCH_KEYS = tuple(_patch_props())


class HBRetopoSettings(bpy.types.PropertyGroup):
    __annotations__ = _settings_props()


class HBRetopoPoint(bpy.types.PropertyGroup):
    co: FloatVectorProperty(size=3, subtype='XYZ')
    side: IntProperty()
    fixed: BoolProperty()


def draw_patch_settings(layout, data):
    col = layout.column()
    row = col.row()
    row.prop(data, "density_mode", expand=True)
    col.prop(data, "resolution" if data.density_mode == 'SEGMENTS' else "edge_length")
    col.prop(data, "layout")
    col.separator()
    col.prop(data, "relax")
    col.prop(data, "side_smooth")
    col.separator()
    col.prop(data, "conform")
    sub = col.column()
    sub.active = data.conform == 'SURFACE'
    sub.prop(data, "projection")
    sub.prop(data, "strength", slider=True)
    sub.prop(data, "offset")
    col.separator()
    row = col.row(align=True)
    sub = row.row(align=True)
    sub.active = not data.subdiv_auto
    sub.prop(data, "subdiv")
    row.prop(data, "subdiv_auto", toggle=True)
    sub = col.row()
    sub.active = data.subdiv_auto
    sub.prop(data, "subdiv_tol", slider=True)


# ----------------------------------------------------------------------------
# Retopo object

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


# ----------------------------------------------------------------------------
# Outline -> sides

def border_path(obj, bm, a_co, b_co, tol):
    """Points (world) of the open-border path between the border vertices at a_co and b_co, or None."""
    mw = obj.matrix_world
    border = [v for v in bm.verts if v.is_boundary]
    if not border:
        return None

    def find(co):
        best = min(border, key=lambda v: ((mw @ v.co) - co).length)
        return best if ((mw @ best.co) - co).length <= tol else None

    a, b = find(a_co), find(b_co)
    if a is None or b is None or a is b:
        return None
    prev, queue = {a: None}, [a]
    for v in queue:
        if v is b:
            break
        for e in v.link_edges:
            if e.is_boundary:
                w = e.other_vert(v)
                if w not in prev:
                    prev[w] = v
                    queue.append(w)
    if b not in prev:
        return None
    path, v = [], b
    while v is not None:
        path.append(mw @ v.co)
        v = prev[v]
    path.reverse()
    chord = (path[-1] - path[0]).length
    if patch.poly_length(path) > 2.0 * chord + 1e-9:   # goes around the mesh, not along this side
        return None
    return path


def outline_sides(nodes, strokes, conform):
    """Turn the outline into side polylines (corner -> next corner).
    nodes: [(co, is_corner)], strokes: {segment i: [points]} (segment i = node i -> node i+1)."""
    n = len(nodes)
    corners = [i for i, (_c, corner) in enumerate(nodes) if corner]
    if len(corners) < 3:
        return None, []
    sides = []
    for ci, a in enumerate(corners):
        b = corners[(ci + 1) % len(corners)]
        idx = [a]
        k = a
        while k != b:
            k = (k + 1) % n
            idx.append(k)
        poly = [nodes[a][0].copy()]
        run = [a]          # nodes of the current non-freehand run (curve through them)

        def flush(run):
            pts = [nodes[i][0] for i in run]
            if len(pts) >= 3:
                curve = patch.catmull_rom(pts)
                return [conform(p) for p in curve[1:-1]] + [pts[-1].copy()]
            return [p.copy() for p in pts[1:]]

        for s_from, s_to in zip(idx, idx[1:]):
            if s_from in strokes:
                poly += flush(run)
                stroke = strokes[s_from]
                poly += [p.copy() for p in stroke[1:-1]] + [nodes[s_to][0].copy()]
                run = [s_to]
            else:
                run.append(s_to)
        poly += flush(run)
        # drop accidental duplicates
        clean = [poly[0]]
        for q in poly[1:]:
            if (q - clean[-1]).length > 1e-9:
                clean.append(q)
        if len(clean) < 2:
            clean.append(clean[0].copy())
        sides.append(clean)
    return sides, corners


def sides_from_outline(context, obj, nodes, strokes, p):
    """Side polylines + which ones follow an existing open border (exact vertices)."""
    surf = surface.Surfaces(context, exclude=(obj,))
    if p.projection == 'NORMAL':
        p.direction = patch.patch_direction(surf, [c for c, _k in nodes])
    conform = patch.make_conform(surf, p)
    sides, corners = outline_sides(nodes, strokes, conform)
    if sides is None:
        return None, None
    bm = bmesh.from_edit_mesh(obj.data)
    tol = 1e-3 * max(1.0, max(c.length for c, _k in nodes))
    fixed = []
    for k, side in enumerate(sides):
        a, b = corners[k], corners[(k + 1) % len(corners)]
        straight = len(side) == 2   # only plain sides follow a border (curved/freehand ones are drawn)
        path = border_path(obj, bm, side[0], side[-1], tol) if straight else None
        if path is not None:
            sides[k] = path
            fixed.append(True)
        else:
            fixed.append(False)
    return sides, fixed


def build_patch(context, obj, sides, fixed, p):
    bm = bmesh.from_edit_mesh(obj.data)
    surf = surface.Surfaces(context, exclude=(obj,))
    existing = [v for v in bm.verts if v.is_boundary]
    size = max(1.0, max(q.length for s in sides for q in s))
    faces = patch.build(bm, sides, fixed, p, surf, mw_inv=obj.matrix_world.inverted_safe(),
                        weld_to=existing, weld_dist=1e-4 * size)
    bm.normal_update()
    bmesh.update_edit_mesh(obj.data)
    return faces


# ----------------------------------------------------------------------------
# The registered operator (redo panel: all settings stay adjustable)

class HB_RETOPO_OT_patch(bpy.types.Operator):
    """Create a quad patch on the surface from a closed outline"""
    bl_idname = "hb_retopo.patch"
    bl_label = "Retopo Patch"
    bl_options = {'REGISTER', 'UNDO'}

    points: CollectionProperty(type=HBRetopoPoint, options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return context.mode in {'EDIT_MESH', 'OBJECT'}

    def draw(self, context):
        self.layout.use_property_split = True
        draw_patch_settings(self.layout, self)

    def execute(self, context):
        sides, fixed = {}, {}
        for pt in self.points:
            sides.setdefault(pt.side, []).append(Vector(pt.co))
            fixed[pt.side] = pt.fixed
        if len(sides) < 3:
            self.report({'WARNING'}, "Needs an outline with at least 3 corners")
            return {'CANCELLED'}
        order = sorted(sides)
        obj = retopo_object(context)
        ensure_edit(context, obj)
        faces = build_patch(context, obj, [sides[k] for k in order], [fixed[k] for k in order],
                            patch.Params.from_props(self))
        bm = bmesh.from_edit_mesh(obj.data)
        for f in bm.faces:
            f.select_set(False)
        for f in faces:
            if f.is_valid:
                f.select_set(True)
        bmesh.update_edit_mesh(obj.data)
        st = context.scene.hb_retopo
        for k in PATCH_KEYS:
            setattr(st, k, getattr(self, k))
        return {'FINISHED'}


HB_RETOPO_OT_patch.__annotations__.update(_patch_props())


# ----------------------------------------------------------------------------
# Drawing

COL_LINE = (1.0, 0.45, 0.75, 1.0)
COL_CORNER = (1.0, 1.0, 1.0, 1.0)
COL_CONTROL = (0.35, 0.85, 1.0, 1.0)


def _draw_view(op):
    try:
        region = bpy.context.region
        gpu.state.blend_set('ALPHA')
        gpu.state.depth_test_set('NONE')
        shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
        lines = op.preview_lines()
        if lines:
            batch = batch_for_shader(shader, 'LINES', {"pos": lines})
            shader.bind()
            shader.uniform_float("viewportSize", (region.width, region.height))
            shader.uniform_float("lineWidth", 2.0)
            shader.uniform_float("color", COL_LINE)
            batch.draw(shader)
        ps = gpu.shader.from_builtin('UNIFORM_COLOR')
        for pts, size, col in ((op.node_points(True), 9, COL_CORNER), (op.node_points(False), 7, COL_CONTROL)):
            if pts:
                gpu.state.point_size_set(size)
                b = batch_for_shader(ps, 'POINTS', {"pos": pts})
                ps.bind()
                ps.uniform_float("color", col)
                b.draw(ps)
        if op.closing and op.nodes:
            gpu.state.point_size_set(16)
            b = batch_for_shader(ps, 'POINTS', {"pos": [tuple(op.nodes[0][0])]})
            ps.bind()
            ps.uniform_float("color", (0.4, 1.0, 0.5, 0.9))
            b.draw(ps)
        gpu.state.point_size_set(1)
        gpu.state.blend_set('NONE')
    except ReferenceError:
        pass


def _draw_hud(op):
    try:
        lines = op.hud_lines()
        ui = bpy.context.preferences.system.ui_scale or 1.0
        x, y = 20 * ui, bpy.context.region.height - 60 * ui
        for text, bright in lines:
            blf.size(0, 13 * ui)
            blf.color(0, *((1, 1, 1, 0.95) if bright else (0.75, 0.75, 0.8, 0.85)))
            blf.position(0, x, y, 0)
            blf.draw(0, text)
            y -= 18 * ui
    except ReferenceError:
        pass


# ----------------------------------------------------------------------------
# The pen (modal)

DRAG_PX = 6


class HB_RETOPO_OT_pen(bpy.types.Operator):
    """Draw a patch outline on the surface. Click: corner, Shift+click: curve point, drag: freehand.
Click the first point (or Enter) to close it into a quad patch, then tune it and click to keep it"""
    bl_idname = "hb_retopo.pen"
    bl_label = "Retopo Pen"
    bl_options = {'BLOCKING'}

    @classmethod
    def poll(cls, context):
        return context.area is not None and context.area.type == 'VIEW_3D' and context.mode in {'EDIT_MESH', 'OBJECT'}

    # --- picking ---------------------------------------------------------------
    def _mouse(self, event):
        return Vector((event.mouse_region_x, event.mouse_region_y))

    def _existing_snap(self, mouse):
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
        d = np.where(ok, np.hypot(x - mouse.x, y - mouse.y), np.inf)
        i = int(np.argmin(d))
        return Vector(pts[i]) if d[i] <= self.st.snap_px else None

    def _refresh_verts(self):
        self.bm = bm = bmesh.from_edit_mesh(self.obj.data)
        mw = np.array(self.obj.matrix_world)
        co = np.array([v.co for v in bm.verts]) if bm.verts else np.zeros((0, 3))
        self.vert_cos = co @ mw[:3, :3].T + mw[:3, 3] if len(co) else co

    def _surface_hit(self, mouse):
        loc, _n = self.surf.mouse_hit(self.region, self.rv3d, mouse)
        return loc

    def _pick(self, mouse):
        snap = self._existing_snap(mouse)
        return snap if snap is not None else self._surface_hit(mouse)

    def _near_first(self, mouse):
        if len([1 for _c, k in self.nodes if k]) < 2 or len(self.nodes) < 3:
            return False
        p = view3d_utils.location_3d_to_region_2d(self.region, self.rv3d, self.nodes[0][0])
        return p is not None and (p - mouse).length <= self.st.snap_px

    # --- drawing data ---------------------------------------------------------------
    def node_points(self, corners):
        return [tuple(c) for c, k in self.nodes if k == corners]

    def preview_lines(self):
        if not self.nodes:
            return []
        pts = []
        n = len(self.nodes)
        conform = lambda q: q
        if self.preview is None:
            nodes = list(self.nodes)
            strokes = dict(self.strokes)
            if self.stroke:                       # freehand in progress
                strokes[len(nodes) - 1] = self.stroke
                nodes.append((self.stroke[-1], True))
            elif self.hover is not None:
                nodes.append((self.hover, not self.shift))
            seq = nodes
            for i in range(len(seq) - 1):
                if i in strokes:
                    path = strokes[i]
                else:
                    path = [seq[i][0], seq[i + 1][0]]
                for a, b in zip(path, path[1:]):
                    pts += [tuple(a), tuple(b)]
            return pts
        for side in self.sides or []:
            for a, b in zip(side, side[1:]):
                pts += [tuple(a), tuple(b)]
        return pts

    def hud_lines(self):
        s = self.st
        if self.preview is None:
            return [("Retopo Pen  —  outline (%d points)" % len(self.nodes), True),
                    ("Click: corner   Shift+Click: curve point   Drag: freehand", False),
                    ("Click first point / Enter: close   Backspace: undo point   Esc: done", False),
                    ("Wheel: density (%s)" % self._density_text(), False)]
        dens = self._density_text()
        return [("Retopo Pen  —  patch (%d faces)" % len(self.preview), True),
                ("Density  %s      [Wheel / + -,  D: segments / edge length]" % dens, True),
                ("Layout   %s      [L]" % dict((i[0], i[1]) for i in LAYOUT_ITEMS)[s.layout], True),
                ("Relax    %d      [R / Shift+R]" % s.relax, True),
                ("Side Smoothing  %d      [S / Shift+S]" % s.side_smooth, True),
                ("Conform  %s, %s      [C,  P: projection]" % (dict((i[0], i[1]) for i in CONFORM_ITEMS)[s.conform],
                                                          "along normal" if s.projection == 'NORMAL' else "nearest"), True),
                ("Strength  %.2f      [G / Shift+G]" % s.strength, True),
                ("Offset   %.4g      [F / Shift+F]" % s.offset, True),
                (("Subdivision  auto -> level %d (gap %.3g)      [A,  T / Shift+T: tolerance %.3f]"
                  % (self.last_level, self.last_gap, s.subdiv_tol)) if s.subdiv_auto else
                 ("Subdivision  %d      [V / Shift+V,  A: auto]" % s.subdiv), True),
                ("Click / Enter: keep    Backspace: edit outline    Esc: discard", False)]

    def _density_text(self):
        s = self.st
        return ("%d segments" % s.resolution) if s.density_mode == 'SEGMENTS' else ("edge %.4g" % s.edge_length)

    # --- preview -----------------------------------------------------------------------
    def _clear_preview(self):
        if self.preview:
            bm = bmesh.from_edit_mesh(self.obj.data)
            verts = {v for f in self.preview if f.is_valid for v in f.verts}
            new = [v for v in verts if v not in self.old_verts]
            bmesh.ops.delete(bm, geom=[f for f in self.preview if f.is_valid], context='FACES_ONLY')
            bmesh.ops.delete(bm, geom=[v for v in new if v.is_valid and not v.link_faces], context='VERTS')
            loose = [e for e in bm.edges if not e.link_faces and not (e.verts[0] in self.old_verts and e.verts[1] in self.old_verts)]
            if loose:
                bmesh.ops.delete(bm, geom=loose, context='EDGES')
            bmesh.update_edit_mesh(self.obj.data)
        self.preview = None

    def _close(self, context):
        p = patch.Params.from_props(self.st)
        self.sides, self.fixed = sides_from_outline(context, self.obj, self.nodes, self.strokes, p)
        if self.sides is None:
            self.report({'WARNING'}, "The outline needs at least 3 corners")
            return
        self._rebuild(context)

    def _rebuild(self, context):
        self._clear_preview()
        # keep the BMesh wrapper alive: element references die with it
        self.bm = bm = bmesh.from_edit_mesh(self.obj.data)
        self.old_verts = set(bm.verts)
        params = patch.Params.from_props(self.st)
        self.preview = build_patch(context, self.obj, self.sides, self.fixed, params)
        self.last_level, self.last_gap = params.level, params.gap

    def _commit(self, context):
        sides, fixed = self.sides, self.fixed
        self._clear_preview()
        pts = [{"name": "", "co": tuple(q), "side": k, "fixed": fixed[k]} for k, s in enumerate(sides) for q in s]
        kw = {k: getattr(self.st, k) for k in PATCH_KEYS}
        bpy.ops.hb_retopo.patch(points=pts, **kw)
        self.nodes, self.strokes, self.sides, self.fixed = [], {}, None, None
        self._refresh_verts()

    def _outline_length(self):
        pts = [c for c, _k in self.nodes]
        return max(1e-6, sum((pts[i + 1] - pts[i]).length for i in range(len(pts) - 1)) +
                   ((pts[0] - pts[-1]).length if len(pts) > 1 else 0))

    # --- operator -----------------------------------------------------------------------
    def invoke(self, context, event):
        self.st = context.scene.hb_retopo
        self.obj = retopo_object(context)
        ensure_edit(context, self.obj)
        self.region, self.rv3d = context.region, context.region_data
        self.surf = surface.Surfaces(context, exclude=(self.obj,))
        if not self.surf:
            self.report({'WARNING'}, "No surface to draw on: show the object(s) to retopologize")
            return {'CANCELLED'}
        self.nodes, self.strokes = [], {}
        self.stroke, self.press = None, None
        self.preview, self.sides, self.fixed = None, None, None
        self.hover, self.closing, self.shift = None, False, False
        self.old_verts = set()
        self.last_level, self.last_gap = 0, 0.0
        self._refresh_verts()
        self._press(event)                  # the click that started the tool
        self._handle = bpy.types.SpaceView3D.draw_handler_add(_draw_view, (self,), 'WINDOW', 'POST_VIEW')
        self._hud = bpy.types.SpaceView3D.draw_handler_add(_draw_hud, (self,), 'WINDOW', 'POST_PIXEL')
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def _finish(self, context):
        bpy.types.SpaceView3D.draw_handler_remove(self._handle, 'WINDOW')
        bpy.types.SpaceView3D.draw_handler_remove(self._hud, 'WINDOW')
        context.area.header_text_set(None)
        context.area.tag_redraw()

    # LMB press / drag / release while drawing the outline
    def _press(self, event):
        self.press = (self._mouse(event), bool(event.shift))
        self.stroke = None

    def _drag(self, event):
        mouse = self._mouse(event)
        start, _shift = self.press
        if self.stroke is None:
            if (mouse - start).length < DRAG_PX:
                return
            first = self._pick(start)
            if first is None:
                return
            if not self.nodes:
                self.nodes.append((first, True))
            elif (self.nodes[-1][0] - first).length > 1e-9:
                self.nodes.append((first, True))
            self.stroke = [self.nodes[-1][0].copy()]
            self._last_px = start
        if (mouse - self._last_px).length >= 4:
            hit = self._surface_hit(mouse)
            if hit is not None:
                self.stroke.append(hit)
                self._last_px = mouse

    def _release(self, context, event):
        mouse = self._mouse(event)
        start, shift = self.press
        self.press = None
        if self.stroke is not None and len(self.stroke) >= 2:      # freehand stroke
            stroke = self.stroke
            self.stroke = None
            if self._near_first(mouse):
                self.strokes[len(self.nodes) - 1] = stroke + [self.nodes[0][0].copy()]
                self._close(context)
                return
            end = self._pick(mouse) or stroke[-1]
            self.strokes[len(self.nodes) - 1] = stroke[:-1] + [end]
            self.nodes.append((end, True))
            return
        self.stroke = None
        if self._near_first(mouse):
            self._close(context)
            return
        p = self._pick(mouse)
        if p is not None:
            self.nodes.append((p, not shift))

    def _adjust(self, context, event):
        """Patch settings keys while the patch is shown. Returns True if handled."""
        s = self.st
        t, shift = event.type, event.shift
        if t in {'WHEELUPMOUSE', 'NUMPAD_PLUS', 'EQUAL', 'WHEELDOWNMOUSE', 'NUMPAD_MINUS', 'MINUS'}:
            up = t in {'WHEELUPMOUSE', 'NUMPAD_PLUS', 'EQUAL'}
            if s.density_mode == 'SEGMENTS':
                s.resolution = max(1, s.resolution + (1 if up else -1))
            else:
                s.edge_length = s.edge_length * (0.9 if up else 1.0 / 0.9)
        elif t == 'D':
            if s.density_mode == 'SEGMENTS':
                s.density_mode = 'LENGTH'
                if self.preview:
                    lens = [f.calc_perimeter() / 4 for f in self.preview if f.is_valid]
                    if lens:
                        s.edge_length = sum(lens) / len(lens) * self.obj.matrix_world.median_scale
            else:
                s.density_mode = 'SEGMENTS'
        elif t == 'L':
            keys = [i[0] for i in LAYOUT_ITEMS]
            s.layout = keys[(keys.index(s.layout) + 1) % len(keys)]
        elif t == 'R':
            s.relax = max(0, s.relax + (-1 if shift else 1))
        elif t == 'S':
            s.side_smooth = max(0, s.side_smooth + (-1 if shift else 1))
        elif t == 'C':
            s.conform = 'NONE' if s.conform == 'SURFACE' else 'SURFACE'
        elif t == 'G':
            s.strength = min(1.0, max(0.0, s.strength + (-0.1 if shift else 0.1)))
        elif t == 'F':
            step = self._outline_length() * 0.0025
            s.offset = s.offset + (-step if shift else step)
        elif t == 'P':
            s.projection = 'NEAREST' if s.projection == 'NORMAL' else 'NORMAL'
        elif t == 'V':
            s.subdiv_auto = False
            s.subdiv = max(0, min(4, s.subdiv + (-1 if shift else 1)))
        elif t == 'A':
            s.subdiv_auto = not s.subdiv_auto
        elif t == 'T':
            s.subdiv_tol = min(0.5, max(0.001, s.subdiv_tol * (1 / 1.5 if shift else 1.5)))
        else:
            return False
        self._rebuild(context)
        return True

    def modal(self, context, event):
        context.area.tag_redraw()
        self.shift = event.shift
        mouse = self._mouse(event)
        if event.type in {'MIDDLEMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM'} or \
                (event.type in {'WHEELUPMOUSE', 'WHEELDOWNMOUSE'} and event.ctrl):
            return {'PASS_THROUGH'}

        # --- patch shown: tune it ---
        if self.preview is not None:
            if event.value == 'PRESS' and self._adjust(context, event):
                return {'RUNNING_MODAL'}
            if event.type in {'LEFTMOUSE', 'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'PRESS':
                self._commit(context)
            elif event.type in {'BACK_SPACE'} and event.value == 'PRESS':
                self._clear_preview()           # back to the outline
            elif event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
                self._clear_preview()
                self.nodes, self.strokes = [], {}
            return {'RUNNING_MODAL'}

        # --- drawing the outline ---
        if event.type == 'MOUSEMOVE':
            if self.press is not None:
                self._drag(event)
            else:
                self.hover = self._pick(mouse)
                self.closing = self._near_first(mouse)
            return {'RUNNING_MODAL'}
        if event.type == 'LEFTMOUSE':
            if event.value == 'PRESS':
                self._press(event)
            elif event.value == 'RELEASE' and self.press is not None:
                self._release(context, event)
            return {'RUNNING_MODAL'}
        if event.type in {'WHEELUPMOUSE', 'WHEELDOWNMOUSE', 'NUMPAD_PLUS', 'NUMPAD_MINUS'} and event.value == 'PRESS':
            s = self.st
            up = event.type in {'WHEELUPMOUSE', 'NUMPAD_PLUS'}
            if s.density_mode == 'SEGMENTS':
                s.resolution = max(1, s.resolution + (1 if up else -1))
            else:
                s.edge_length *= 0.9 if up else 1 / 0.9
            return {'RUNNING_MODAL'}
        if event.type in {'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'PRESS':
            self._close(context)
            return {'RUNNING_MODAL'}
        if (event.type == 'BACK_SPACE' or (event.type == 'Z' and event.ctrl)) and event.value == 'PRESS':
            if self.nodes:
                last = len(self.nodes) - 1
                self.strokes.pop(last - 1, None)
                self.strokes.pop(last, None)
                self.nodes.pop()
            return {'RUNNING_MODAL'}
        if event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            if self.nodes:
                self.nodes, self.strokes = [], {}
                return {'RUNNING_MODAL'}
            self._finish(context)
            return {'FINISHED'}
        return {'RUNNING_MODAL'}


# ----------------------------------------------------------------------------
# Toolbar tool with Tool Settings

class VIEW3D_PT_hb_retopo_options(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Retopo Pen Options"

    def draw(self, context):
        self.layout.use_property_split = True
        draw_patch_settings(self.layout, context.scene.hb_retopo)
        self.layout.prop(context.scene.hb_retopo, "snap_px")


def _tool_settings(context, layout, tool):
    st = context.scene.hb_retopo
    layout.prop(st, "resolution" if st.density_mode == 'SEGMENTS' else "edge_length")
    layout.prop(st, "relax")
    layout.prop(st, "layout", text="")
    layout.prop(st, "conform", text="")
    layout.popover("VIEW3D_PT_hb_retopo_options", text="", icon='PREFERENCES')


def _make_tool(mode):
    return type("HB_TL_retopo_pen_%s" % mode.lower(), (bpy.types.WorkSpaceTool,), {
        "bl_space_type": 'VIEW_3D',
        "bl_context_mode": mode,
        "bl_idname": "hb_retopo.pen_tool",
        "bl_label": "Retopo Pen",
        "bl_description": ("Click: corner, Shift+click: curve point, drag: freehand. Close the outline "
                           "for a quad patch, then tune density / relax / smoothing / conform"),
        "bl_icon": "ops.mesh.polybuild_hover",
        "bl_keymap": (("hb_retopo.pen", {"type": 'LEFTMOUSE', "value": 'PRESS'}, None),
                      ("hb_retopo.pen", {"type": 'LEFTMOUSE', "value": 'PRESS', "shift": True}, None)),
        "draw_settings": _tool_settings,
    })


_tools = []
classes = (HBRetopoSettings, HBRetopoPoint, HB_RETOPO_OT_patch, HB_RETOPO_OT_pen, VIEW3D_PT_hb_retopo_options)


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
