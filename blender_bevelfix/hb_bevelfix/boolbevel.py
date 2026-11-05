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
        bmesh.ops.weld_verts(bm, targetmap={v: w})
        merged += 1
    return order, merged


def choose_side(seq, edges):
    """Keep the side most loop vertices have an edge on (their structure, usually the cutter's);
    the minority - vertices the boolean added mid-face on that side - get merged away."""
    a, b = split_sides(seq, edges)
    le = set(edges)
    na = sum(1 for v in seq if rail_edges(v, a, le))
    nb = sum(1 for v in seq if rail_edges(v, b, le))
    if na == 0:
        return 1
    if nb == 0:
        return 0
    return 0 if na >= nb else 1


# ----------------------------------------------------------------------------
# 2) Offset loops on both sides

def _side_normal(v, faces):
    n = sum((f.normal for f in v.link_faces if f in faces), Vector())
    return n.normalized() if n.length > 1e-12 else None


def offset_side(bm, seq, cyclic, edges, side_faces, dist, reuse_tol=0.35, loop_slide=False):
    """Build a parallel loop on one side; returns (offset verts, new strip faces)."""
    loop_edges = set(edges)
    n = len(seq)
    targets = []   # per loop vertex: (existing vertex to reuse) or (new position)
    snap = {}      # re-used vertex -> where it moves to
    for i, v in enumerate(seq):
        prev_v = seq[i - 1] if (cyclic or i > 0) else None
        next_v = seq[(i + 1) % n] if (cyclic or i < n - 1) else None
        tangent = ((next_v.co if next_v else v.co) - (prev_v.co if prev_v else v.co))
        rails = rail_edges(v, side_faces, loop_edges)
        if rails and loop_slide:
            # the rail most perpendicular to the loop
            rail = min(rails, key=lambda e: abs((e.other_vert(v).co - v.co).normalized().dot(tangent.normalized()))
                       if tangent.length > 1e-12 else 0)
            far = rail.other_vert(v)
            L = (far.co - v.co).length
            targets.append(v.co + (far.co - v.co).normalized() * min(dist, 0.8 * L))
            continue
        nrm = _side_normal(v, side_faces)
        if nrm is None or tangent.length < 1e-12:
            raise LoopError("Can't offset a loop vertex (no faces on one side)")
        d = nrm.cross(tangent).normalized()
        # Which way is "into" this side: from the winding of the side's face on each loop edge at v
        # (a face's interior lies to the left of its edges, seen along the face normal).
        inward = Vector()
        for e in v.link_edges:
            if e not in loop_edges:
                continue
            for f in e.link_faces:
                if f not in side_faces:
                    continue
                for l in f.loops:
                    if l.edge is e:
                        p, q = l.vert.co, l.link_loop_next.vert.co
                        inward += f.normal.cross(q - p).normalized()
        if inward.length > 1e-9 and d.dot(inward) < 0:
            d = -d
        target = v.co + d * dist
        # Re-use an existing outward neighbour that already sits about where the offset goes.
        near = [e.other_vert(v) for e in rails if (e.other_vert(v).co - target).length <= reuse_tol * dist]
        if near:
            w = min(near, key=lambda w: (w.co - target).length)
            snap[w] = target  # moved onto the offset loop (like Merge Perimeter)
            targets.append(w)
        else:
            targets.append(target)

    # Neighbouring loop vertices must not re-use the same far vertex (that would make a triangle).
    for i in range(n):
        j = (i + 1) % n
        if (cyclic or j > i) and isinstance(targets[i], bmesh.types.BMVert) and targets[i] is targets[j]:
            targets[j] = targets[j].co.copy()

    def specs_for(sub):
        out = []
        for f in side_faces:
            verts = []
            for v in f.verts:
                w = sub.get(v, v)
                if not verts or verts[-1] is not w:
                    verts.append(w)
            if len(verts) > 1 and verts[0] is verts[-1]:
                verts.pop()
            out.append((f, verts))
        return out

    def bad(f, verts):
        if len(verts) < 3 or len(set(verts)) != len(verts):
            return True
        existing = bm.faces.get(verts) if all(isinstance(v, bmesh.types.BMVert) for v in verts) else None
        return existing is not None and existing not in side_faces

    # Re-using an existing vertex must not break a face; where it would, place a new vertex instead.
    for _ in range(n + 1):
        sub = {v: t for v, t in zip(seq, targets) if isinstance(t, bmesh.types.BMVert)}
        broken = [f for f, verts in specs_for(sub) if bad(f, verts)]
        if not broken:
            break
        for f in broken:
            for i, v in enumerate(seq):
                if v in f.verts and isinstance(targets[i], bmesh.types.BMVert):
                    targets[i] = targets[i].co.copy()
    offs = [t if isinstance(t, bmesh.types.BMVert) else bm.verts.new(t) for t in targets]
    for w in offs:
        if w in snap:
            w.co = snap[w]
    sub = dict(zip(seq, offs))

    # Rebuild the side's faces with the offset vertices in place of the loop vertices.
    specs = [(verts, f.material_index, f.smooth) for f, verts in specs_for(sub)]
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
    if len(rebuilt) != len(specs):
        raise LoopError("Offset failed to rebuild %d face(s); try a smaller Offset" % (len(specs) - len(rebuilt)))

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


def _closest_on_loop(co, seq, cyclic):
    from mathutils.geometry import intersect_point_line
    best, best_q = float("inf"), None
    n = len(seq)
    for i in range(n if cyclic else n - 1):
        a, b = seq[i].co, seq[(i + 1) % n].co
        p, t = intersect_point_line(co, a, b)
        q = a if t <= 0 else b if t >= 1 else p
        d = (co - q).length
        if d < best:
            best, best_q = d, q
    return best, best_q


def _dist_to_loop(co, seq, cyclic):
    return _closest_on_loop(co, seq, cyclic)[0]


def clean_offset(bm, offs, protect, tol, seq=None, cyclic=True, dist=0.0, rings=()):
    """Clean up after the offset (like MESHmachine's Merge Perimeter): existing vertices sitting on
    top of an offset vertex are welded into it; ones left inside the new strip or right on the
    offset loop are pushed out to just beyond it, so the faces around the loop can't fold."""
    offs = [o for o in offs if o.is_valid]
    targetmap, pushed = {}, 0
    for o in offs:
        for f in o.link_faces:
            for w in f.verts:
                if w in protect or w in targetmap or w in offs:
                    continue
                if seq is None or dist <= 0:
                    if (w.co - o.co).length < tol:
                        targetmap[w] = o
                    continue
                d, q = _closest_on_loop(w.co, seq, cyclic)
                on_ring = (w.co - o.co).length < tol or any(_dist_to_loop(w.co, r, cyclic) < tol for r in rings)
                if (d < dist * 0.98 or on_ring) and q is not None and (w.co - q).length > 1e-9:
                    w.co = q + (w.co - q).normalized() * (dist + max(tol, 0.25 * dist))
                    pushed += 1
    if targetmap:
        bmesh.ops.weld_verts(bm, targetmap=targetmap)
    return len(targetmap) + pushed


def _folded(f):
    """Face self-intersects in its own plane (or is degenerate)."""
    from mathutils.geometry import intersect_line_line_2d
    n = f.normal
    if n.length < 1e-9 or f.calc_area() < 1e-12:
        return True
    x = (f.verts[1].co - f.verts[0].co)
    x = (x - n * x.dot(n)).normalized()
    y = n.cross(x)
    pts = [Vector((v.co.dot(x), v.co.dot(y))) for v in f.verts]
    k = len(pts)
    for i in range(k):
        for j in range(i + 2, k):
            if i == 0 and j == k - 1:
                continue
            p = intersect_line_line_2d(pts[i], pts[(i + 1) % k], pts[j], pts[(j + 1) % k])
            if p is not None and all((p - q).length > 1e-7 for q in (pts[i], pts[(i + 1) % k], pts[j], pts[(j + 1) % k])):
                return True
    return False


def merge_folded(bm, ring, strips):
    """Faces next to an offset loop that fold over themselves (thin slivers where an old edge runs
    almost along the loop) are merged into their outer neighbour by dissolving their outer edges."""
    ring = {v for v in ring if v.is_valid}
    fixed = 0
    for _ in range(4):
        strips = {f for f in strips if f.is_valid}
        bad = [f for f in {f for v in ring if v.is_valid for f in v.link_faces}
               if f not in strips and _folded(f)]
        if not bad:
            break
        edges = set()
        for f in bad:
            for e in f.edges:
                if len(e.link_faces) == 2 and not any(g in strips for g in e.link_faces) \
                        and not all(v in ring for v in e.verts):
                    edges.add(e)
        if not edges:
            break
        bmesh.ops.dissolve_edges(bm, edges=list(edges), use_verts=False)
        bm.normal_update()
        fixed += len(edges)
    return fixed


def dissolve_ring_slivers(bm, ring, strips):
    """Faces left lying entirely on an offset loop (slivers after welding) are merged into their
    outer neighbour by dissolving the edge that isn't part of the loop strip."""
    ring = {v for v in ring if v.is_valid}
    strips = {f for f in strips if f.is_valid}
    edges = set()
    for f in {f for v in ring for f in v.link_faces}:
        if f in strips or not all(v in ring for v in f.verts):
            continue
        for e in f.edges:
            if not any(g in strips for g in e.link_faces) and len(e.link_faces) == 2:
                edges.add(e)
    if edges:
        bmesh.ops.dissolve_edges(bm, edges=list(edges), use_verts=False)
    return len(edges)


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
    loop_slide: BoolProperty(name="Loop Slide", default=False,
                             description="Offset along existing edges instead of straight across the surface")
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
        col.prop(self, "loop_slide")
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
                offs_a, strip_a, _ = offset_side(bm, seq, cyclic, edges, a, dist, loop_slide=self.loop_slide)
                offs_b, strip_b, _ = offset_side(bm, seq, cyclic, edges, b, dist, loop_slide=self.loop_slide)
                protect |= set(offs_a) | set(offs_b)
                welded += clean_offset(bm, offs_a + offs_b, protect, self.offset_clean * self.width,
                                       seq=seq, cyclic=cyclic, dist=dist, rings=(offs_a, offs_b))
                dissolve_ring_slivers(bm, offs_a, strip_a)
                dissolve_ring_slivers(bm, offs_b, strip_b)
                bm.normal_update()
                merge_folded(bm, offs_a, strip_a)
                merge_folded(bm, offs_b, strip_b)
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
        self.report({'INFO'}, "Cleanup merged %d, offset cleaned %d, fix: %d sides, %d welded"
                    % (merged, welded, st["affected"], st["welded"]))
        return {'FINISHED'}


def register():
    bpy.utils.register_class(MESH_OT_hb_boolean_bevel)


def unregister():
    bpy.utils.unregister_class(MESH_OT_hb_boolean_bevel)
