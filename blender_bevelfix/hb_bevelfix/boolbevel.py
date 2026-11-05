# SPDX-License-Identifier: GPL-2.0-or-later
"""Boolean Bevel: clean bevels on boolean intersection loops, following MESHmachine's workflow
(Boolean Cleanup -> Offset on both sides -> Bevel), as one operator:

1. Cleanup: loop vertices that have no edge on the kept side are merged into their closest loop
   neighbour, closest first, until every remaining gap is longer than the threshold.
2. Offset: a parallel loop on each side, so the bevel slides along a clean strip of quads.
   Offset vertices slide along existing outward edges where there are any (re-using the far
   vertex when it's about the right distance away), otherwise they're placed in the face.
3. Clean up the offset: existing vertices left very close to an offset vertex are welded to it.
4. Bevel the loop, then run Fix Bevel Overlap on the result.
"""

import bmesh
import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty
from mathutils import Vector

from . import fix


class LoopError(Exception):
    pass


# ----------------------------------------------------------------------------
# Loop + sides

def selected_loops(bm):
    """Ordered vertex sequences of the selected edges: [(verts, cyclic, edges)]."""
    sel = [e for e in bm.edges if e.select]
    if not sel:
        raise LoopError("Select the boolean intersection loop (edges)")
    adj = {}
    for e in sel:
        for v in e.verts:
            adj.setdefault(v, []).append(e)
    if any(len(es) > 2 for es in adj.values()):
        raise LoopError("The selection branches: select simple edge loops")
    remaining = set(sel)
    loops = []
    while remaining:
        ends = [v for v, es in adj.items() if sum(1 for e in es if e in remaining) == 1]
        start = ends[0] if ends else next(iter(remaining)).verts[0]
        seq, edges, v = [start], [], start
        while True:
            nxt = [e for e in adj[v] if e in remaining]
            if not nxt:
                break
            e = nxt[0]
            remaining.discard(e)
            edges.append(e)
            v = e.other_vert(v)
            if v is start:
                break
            seq.append(v)
        loops.append((seq, v is start and len(edges) >= 3, edges))
    return loops


def loop_from_verts(bm, seq, cyclic):
    edges = []
    for i in range(len(seq) if cyclic else len(seq) - 1):
        e = bm.edges.get((seq[i], seq[(i + 1) % len(seq)]))
        if e is None:
            raise LoopError("The loop broke apart during cleanup")
        edges.append(e)
    return seq, cyclic, edges


def split_sides(loop_verts, loop_edges):
    """Faces around the loop split into the two sides: (set A, set B)."""
    loop_edges = set(loop_edges)
    ring = {f for v in loop_verts for f in v.link_faces}
    side = {}
    label = 0
    for f in ring:
        if f in side:
            continue
        side[f] = label
        stack = [f]
        while stack:
            g = stack.pop()
            for e in g.edges:
                if e in loop_edges:
                    continue
                for h in e.link_faces:
                    if h in ring and h not in side:
                        side[h] = label
                        stack.append(h)
        label += 1
    if label != 2:
        raise LoopError("The loop must separate two surfaces (got %d sides)" % label)
    a = {f for f, l in side.items() if l == 0}
    b = {f for f, l in side.items() if l == 1}
    return a, b


def rail_edges(v, side_faces, loop_edges):
    """Edges leaving v into the given side (not on the loop, all faces on that side)."""
    return [e for e in v.link_edges if e not in loop_edges and e.link_faces
            and all(f in side_faces for f in e.link_faces)]


# ----------------------------------------------------------------------------
# 1) Boolean cleanup

def cleanup(bm, seq, cyclic, edges, keep, threshold):
    """Merge loop vertices without a rail on the kept side into their closest loop neighbour,
    closest first, while the gap is <= threshold. Returns (remaining loop vertices, merged count)."""
    a, b = split_sides(seq, edges)
    keep_faces = a if keep == 0 else b
    loop_edges = set(edges)
    fixed = {v for v in seq if rail_edges(v, keep_faces, loop_edges)}
    if not cyclic:
        fixed |= {seq[0], seq[-1]}
    order = list(seq)
    merged = 0
    while True:
        best = None
        n = len(order)
        for i, v in enumerate(order):
            if v in fixed:
                continue
            nbrs = []
            if cyclic or i > 0:
                nbrs.append(order[i - 1])
            if cyclic or i < n - 1:
                nbrs.append(order[(i + 1) % n])
            for w in nbrs:
                d = (w.co - v.co).length
                if d <= threshold and (best is None or d < best[0]):
                    best = (d, i, w)
        if best is None or len(order) <= 3:
            break
        _d, i, w = best
        v = order.pop(i)
        bmesh.ops.pointmerge(bm, verts=[v, w], merge_co=w.co.copy())
        merged += 1
    return order, merged


def choose_side(seq, edges):
    """Keep the side with fewer rail vertices (the more regular surface, usually the cutter)."""
    a, b = split_sides(seq, edges)
    le = set(edges)
    na = sum(1 for v in seq if rail_edges(v, a, le))
    nb = sum(1 for v in seq if rail_edges(v, b, le))
    if na == 0:
        return 1
    if nb == 0:
        return 0
    return 0 if na <= nb else 1


# ----------------------------------------------------------------------------
# 2) Offset loops on both sides

def _side_normal(v, faces):
    n = sum((f.normal for f in v.link_faces if f in faces), Vector())
    return n.normalized() if n.length > 1e-12 else None


def offset_side(bm, seq, cyclic, edges, side_faces, dist, reuse_tol=0.35):
    """Build a parallel loop on one side; returns (offset verts, new strip faces)."""
    loop_edges = set(edges)
    n = len(seq)
    targets = []   # per loop vertex: (existing vertex to reuse) or (new position)
    for i, v in enumerate(seq):
        prev_v = seq[i - 1] if (cyclic or i > 0) else None
        next_v = seq[(i + 1) % n] if (cyclic or i < n - 1) else None
        tangent = ((next_v.co if next_v else v.co) - (prev_v.co if prev_v else v.co))
        rails = rail_edges(v, side_faces, loop_edges)
        if rails:
            # the rail most perpendicular to the loop
            rail = min(rails, key=lambda e: abs((e.other_vert(v).co - v.co).normalized().dot(tangent.normalized()))
                       if tangent.length > 1e-12 else 0)
            far = rail.other_vert(v)
            L = (far.co - v.co).length
            if abs(L - dist) <= reuse_tol * dist:
                targets.append(far)
            else:
                targets.append(v.co + (far.co - v.co).normalized() * min(dist, 0.8 * L))
            continue
        nrm = _side_normal(v, side_faces)
        if nrm is None or tangent.length < 1e-12:
            raise LoopError("Can't offset a loop vertex (no faces on one side)")
        d = nrm.cross(tangent).normalized()
        centre = sum((f.calc_center_median() for f in v.link_faces if f in side_faces), Vector())
        if d.dot(centre / max(1, len([f for f in v.link_faces if f in side_faces])) - v.co) < 0:
            d = -d
        targets.append(v.co + d * dist)

    # Neighbouring loop vertices must not re-use the same far vertex (that would make a triangle).
    for i in range(n):
        j = (i + 1) % n
        if (cyclic or j > i) and isinstance(targets[i], bmesh.types.BMVert) and targets[i] is targets[j]:
            targets[j] = targets[j].co.copy()
    offs = [t if isinstance(t, bmesh.types.BMVert) else bm.verts.new(t) for t in targets]
    sub = dict(zip(seq, offs))

    # Rebuild the side's faces with the offset vertices in place of the loop vertices.
    specs = []
    for f in side_faces:
        verts = []
        for v in f.verts:
            w = sub.get(v, v)
            if not verts or verts[-1] is not w:
                verts.append(w)
        if len(verts) > 1 and verts[0] is verts[-1]:
            verts.pop()
        specs.append((verts, f.material_index, f.smooth))
    bmesh.ops.delete(bm, geom=list(side_faces), context='FACES_ONLY')
    rebuilt = []
    for verts, mat, smooth in specs:
        if len(verts) >= 3 and len(set(verts)) == len(verts):
            try:
                f = bm.faces.new(verts)
            except ValueError:
                continue
            f.material_index, f.smooth = mat, smooth
            rebuilt.append(f)

    # The strip of quads between the loop and its offset.
    strip = []
    smooth = rebuilt[0].smooth if rebuilt else False
    for i in range(n if cyclic else n - 1):
        j = (i + 1) % n
        quad = [seq[i], seq[j], offs[j], offs[i]]
        if len(set(quad)) == 4:
            try:
                f = bm.faces.new(quad)
                f.smooth = smooth
                strip.append(f)
            except ValueError:
                pass
    # Old rail edges between loop and re-used vertices are now strip edges; drop loose leftovers.
    for e in [e for v in seq for e in v.link_edges if not e.link_faces]:
        if e.is_valid:
            bm.edges.remove(e)
    bmesh.ops.recalc_face_normals(bm, faces=strip + rebuilt)
    return offs, strip, rebuilt


def clean_offset(bm, offs, protect, tol):
    """Weld existing vertices that ended up within `tol` of an offset vertex into it."""
    targetmap = {}
    for o in offs:
        if not o.is_valid:
            continue
        for f in o.link_faces:
            for w in f.verts:
                if w is o or w in protect or w in targetmap or w in targetmap.values():
                    continue
                if (w.co - o.co).length < tol:
                    targetmap[w] = o
    if targetmap:
        bmesh.ops.weld_verts(bm, targetmap=targetmap)
    return len(targetmap)


# ----------------------------------------------------------------------------
# Operator

class MESH_OT_hb_boolean_bevel(bpy.types.Operator):
    """Bevel a boolean intersection loop cleanly: Boolean Cleanup, offset support loops on both
sides (a clean strip of quads), clean up the offset, bevel, then fix the bevel"""
    bl_idname = "mesh.hb_boolean_bevel"
    bl_label = "Boolean Bevel"
    bl_options = {'REGISTER', 'UNDO'}

    width: FloatProperty(name="Width", default=0.05, min=1e-5, subtype='DISTANCE', unit='LENGTH', step=0.1)
    segments: IntProperty(name="Segments", default=3, min=1, max=64)
    profile: FloatProperty(name="Shape", default=0.5, min=0.0, max=1.0, subtype='FACTOR')

    cleanup: BoolProperty(name="Boolean Cleanup", default=True)
    keep_side: EnumProperty(name="Keep Side", items=(
        ('AUTO', "Auto", "Keep the vertices of the more regular side (usually the cutter)"),
        ('A', "A", ""), ('B', "B", "")), default='AUTO')
    threshold: FloatProperty(name="Merge Distance", default=0.5, min=0.0, max=5.0, subtype='FACTOR',
                             description="Cleanup welds loop vertices closer than this (times the width) and "
                                         "stops there")
    offset: FloatProperty(name="Offset", default=1.6, min=1.05, max=10.0,
                          description="Support loop distance from the intersection, times the width")
    offset_clean: FloatProperty(name="Offset Cleanup", default=0.3, min=0.0, max=1.0, subtype='FACTOR',
                                description="Weld existing vertices closer than this (times the width) to the "
                                            "support loops")
    fix_weld: BoolProperty(name="Fix: Weld Conflicts", default=True)

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        col = layout.column()
        col.prop(self, "width")
        col.prop(self, "segments")
        col.prop(self, "profile", slider=True)
        col.separator()
        col.prop(self, "cleanup")
        sub = col.column()
        sub.active = self.cleanup
        sub.row().prop(self, "keep_side", expand=True)
        sub.prop(self, "threshold", slider=True)
        col.separator()
        col.prop(self, "offset")
        col.prop(self, "offset_clean", slider=True)
        col.prop(self, "fix_weld")

    def execute(self, context):
        obj = context.edit_object
        me = obj.data
        bm = bmesh.from_edit_mesh(me)
        bm.normal_update()
        try:
            loops = selected_loops(bm)
            merged = 0
            if self.cleanup:
                cleaned = []
                for seq, cyclic, edges in loops:
                    keep = choose_side(seq, edges) if self.keep_side == 'AUTO' else (0 if self.keep_side == 'A' else 1)
                    order, m = cleanup(bm, seq, cyclic, edges, keep, self.threshold * self.width)
                    merged += m
                    cleaned.append(loop_from_verts(bm, order, cyclic))
                loops = cleaned
                bm.normal_update()

            dist = self.offset * self.width
            loop_edges_all = []
            welded = 0
            for seq, cyclic, edges in loops:
                a, b = split_sides(seq, edges)
                protect = set(seq)
                offs_a, strip_a, _ = offset_side(bm, seq, cyclic, edges, a, dist)
                offs_b, strip_b, _ = offset_side(bm, seq, cyclic, edges, b, dist)
                protect |= set(offs_a) | set(offs_b)
                if self.offset_clean > 0:
                    welded += clean_offset(bm, offs_a + offs_b, protect, self.offset_clean * self.width)
                loop_edges_all += [bm.edges.get((seq[i], seq[(i + 1) % len(seq)]))
                                   for i in range(len(seq) if cyclic else len(seq) - 1)]
            near = {e for e in bm.edges if e.is_valid and e.calc_length() < self.width * 1e-3}
            if near:
                bmesh.ops.dissolve_degenerate(bm, dist=self.width * 1e-3, edges=list(near))
        except LoopError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}

        # 4) select the loop and bevel it
        for g in list(bm.verts) + list(bm.edges) + list(bm.faces):
            g.select = False
        for e in loop_edges_all:
            if e is not None and e.is_valid:
                e.select_set(True)
        bm.normal_update()
        bmesh.update_edit_mesh(me)
        context.tool_settings.mesh_select_mode = (False, True, False)
        bpy.ops.mesh.bevel(offset=self.width, segments=self.segments, profile=self.profile,
                           loop_slide=True, clamp_overlap=False, affect='EDGES')

        bm = bmesh.from_edit_mesh(me)
        bm.verts.index_update()
        st = fix.fix(bm, weld=self.fix_weld)
        bm.normal_update()
        bmesh.update_edit_mesh(me)
        self.report({'INFO'}, "Cleanup merged %d, offset welded %d, fix: %d sides, %d welded"
                    % (merged, welded, st["affected"], st["welded"]))
        return {'FINISHED'}


def register():
    bpy.utils.register_class(MESH_OT_hb_boolean_bevel)


def unregister():
    bpy.utils.unregister_class(MESH_OT_hb_boolean_bevel)
