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
# N-gons around the bevel

def _corners(f, count=4):
    """The `count` vertices of a face where its outline turns the most."""
    vs = list(f.verts)
    n = len(vs)
    turn = []
    for i, v in enumerate(vs):
        a, c = vs[i - 1].co, vs[(i + 1) % n].co
        d1, d2 = v.co - a, c - v.co
        ang = d1.angle(d2) if d1.length > 1e-12 and d2.length > 1e-12 else 0.0
        turn.append((ang, i))
    idx = sorted(i for _a, i in sorted(turn, reverse=True)[:count])
    return idx


def _sides(f):
    """The face outline split at its 4 corners: 4 vertex runs (corner ... corner)."""
    vs = list(f.verts)
    n = len(vs)
    cs = _corners(f)
    out = []
    for k in range(4):
        i, j = cs[k], cs[(k + 1) % 4]
        run = [vs[i]]
        x = i
        while x != j:
            x = (x + 1) % n
            run.append(vs[x])
        out.append(run)
    return out


def _fractions(run):
    lens = [(run[i + 1].co - run[i].co).length for i in range(len(run) - 1)]
    tot = sum(lens) or 1.0
    acc, out = 0.0, []
    for l in lens[:-1]:
        acc += l
        out.append(acc / tot)
    return out


def quad_split(bm, f, budget, reach=2, leftovers=None):
    """Split a 'quad with extra vertices on one side' into quads by continuing each extra vertex
    straight across; new vertices on the opposite edge carry the cut into the next quad
    (like an edge-ring cut) until it reaches a face that isn't a quad. Returns True if split."""
    if len(f.verts) <= 4 or budget[0] <= 0:
        return False
    sides = _sides(f)
    long_ = [k for k in range(4) if len(sides[k]) > 2]
    if len(long_) != 1:
        return False
    k = long_[0]
    opp = sides[(k + 2) % 4]
    if len(opp) != 2:
        return False
    run = sides[k]
    # only straight sides (extra vertices close to the line between the corners)
    from mathutils.geometry import intersect_point_line
    a_co, b_co = run[0].co, run[-1].co
    side_len = (b_co - a_co).length
    if side_len < 1e-9:
        return False
    for v in run[1:-1]:
        p, _t = intersect_point_line(v.co, a_co, b_co)
        if (p - v.co).length > 0.08 * side_len:
            return False
    fr = _fractions(run)                       # positions of the extra vertices along their side
    e = bm.edges.get((opp[0], opp[1]))
    if e is None:
        return False
    neighbour = next((g for g in e.link_faces if g is not f), None)
    # opp runs opposite to `run` around the face, so position t on run is 1-t from opp[0]
    new_verts = []
    start, cur_edge = opp[0], e
    remaining = 1.0
    for t in sorted((1.0 - x for x in fr)):
        # split cur_edge (from `start` towards opp[1]) at absolute fraction t of the original edge
        local = (t - (1.0 - remaining)) / remaining if remaining > 1e-12 else 0.5
        ne, nv = bmesh.utils.edge_split(cur_edge, start, max(1e-4, min(1 - 1e-4, local)))
        new_verts.append(nv)
        remaining = 1.0 - t
        start = nv
        cur_edge = next(x for x in nv.link_edges if x.other_vert(nv) is opp[1])
    # connect each extra vertex of `run` to its partner on the opposite side
    extras = run[1:-1]
    partners = list(reversed(new_verts))
    for a_v, b_v in zip(extras, partners):
        face = next((g for g in a_v.link_faces if b_v in g.verts), None)
        if face is not None and bm.edges.get((a_v, b_v)) is None:
            bmesh.utils.face_split(face, a_v, b_v)
    budget[0] -= 1
    # carry on into the neighbour if it was a quad (now a quad with extra vertices), up to `reach`
    if neighbour is not None and neighbour.is_valid and len(neighbour.verts) == 4 + len(new_verts):
        if reach > 1:
            if not quad_split(bm, neighbour, budget, reach - 1, leftovers) and leftovers is not None:
                leftovers.append(neighbour)
        elif leftovers is not None:
            leftovers.append(neighbour)
    return True


def fill_ngons(bm, faces, mode, reach=2):
    """Make the n-gons (> 4 sides) among `faces` quads ('QUADS': split quads-with-extra-vertices
    straight across, triangulate the rest and join the triangles back into quads where the shapes
    allow) or triangles ('TRIS')."""
    import math
    ngons = [f for f in faces if f.is_valid and len(f.verts) > 4]
    if mode == 'NONE' or not ngons:
        return 0
    count = len(ngons)
    if mode == 'QUADS':
        budget, leftovers = [500], []
        for f in ngons:
            if f.is_valid:
                quad_split(bm, f, budget, reach, leftovers)
        ngons = [f for f in set(faces) | set(leftovers) if f.is_valid and len(f.verts) > 4]
    if not ngons:
        return count
    res = bmesh.ops.triangulate(bm, faces=ngons, quad_method='BEAUTY', ngon_method='BEAUTY')
    tris = [f for f in res["faces"] if f.is_valid]
    if mode == 'QUADS' and tris:
        bmesh.ops.join_triangles(bm, faces=tris, angle_face_threshold=math.radians(40),
                                 angle_shape_threshold=math.radians(90))
    return count


# ----------------------------------------------------------------------------
# Settings (shared by the operator, its redo panel and the toolbar tool's Tool Settings)

FILL_ITEMS = (('QUADS', "Quads", "Turn the n-gons around the bevel into quads (and a few triangles)"),
              ('TRIS', "Triangles", "Triangulate the n-gons around the bevel"),
              ('NONE', "Off", "Leave the n-gons"))


def _settings_props():
    return dict(
        width=FloatProperty(name="Width", default=0.0, min=0.0, subtype='DISTANCE', unit='LENGTH', step=0.1,
                            description="Bevel width (0 = start from a quarter of the loop's average edge)"),
        segments=IntProperty(name="Segments", default=3, min=1, max=64),
        profile=FloatProperty(name="Shape", default=0.5, min=0.0, max=1.0, subtype='FACTOR'),
        cleanup=BoolProperty(name="Boolean Cleanup", default=True),
        keep_side=EnumProperty(name="Keep Side", items=(
            ('AUTO', "Auto", "Keep the side most loop vertices have an edge on (usually the cutter)"),
            ('A', "A", ""), ('B', "B", "")), default='AUTO'),
        threshold=FloatProperty(name="Merge Distance", default=0.5, min=0.0, max=5.0, subtype='FACTOR',
                                description="Cleanup merges loop vertices closer than this (times the width) "
                                            "and stops there"),
        offset=FloatProperty(name="Offset", default=1.6, min=1.05, max=10.0,
                             description="Support loop distance from the intersection, times the width"),
        loop_slide=BoolProperty(name="Loop Slide", default=False,
                                description="Offset along existing edges instead of straight across the surface"),
        offset_clean=FloatProperty(name="Offset Cleanup", default=0.3, min=0.0, max=1.0, subtype='FACTOR',
                                   description="Clean up existing vertices closer than this (times the width) "
                                               "to the support loops"),
        fill=EnumProperty(name="Fill N-gons", items=FILL_ITEMS, default='QUADS'),
        reach=IntProperty(name="Cut Reach", default=2, min=1, max=50,
                          description="Quads: how many faces a straight cut may continue into before the rest "
                                      "is triangulated and joined"),
        fix_weld=BoolProperty(name="Fix: Weld Conflicts", default=True),
    )


PROP_NAMES = tuple(_settings_props())


class HBBoolBevelSettings(bpy.types.PropertyGroup):
    __annotations__ = _settings_props()


def draw_settings_column(layout, data):
    col = layout.column()
    col.prop(data, "width")
    col.prop(data, "segments")
    col.prop(data, "profile", slider=True)
    col.prop(data, "fill")
    sub = col.row()
    sub.active = data.fill == 'QUADS'
    sub.prop(data, "reach")
    col.separator()
    col.prop(data, "cleanup")
    sub = col.column()
    sub.active = data.cleanup
    sub.row().prop(data, "keep_side", expand=True)
    sub.prop(data, "threshold", slider=True)
    col.separator()
    col.prop(data, "offset")
    col.prop(data, "loop_slide")
    col.prop(data, "offset_clean", slider=True)
    col.prop(data, "fix_weld")


# ----------------------------------------------------------------------------
# The pipeline

RING_LAYER = "hb_ring"


def auto_width(bm):
    lens = [e.calc_length() for e in bm.edges if e.select]
    return 0.25 * (sum(lens) / len(lens)) if lens else 0.05


def run_pipeline(context, p):
    """Run the whole Boolean Bevel on the selected loop(s) of the edit object. `p` holds the
    settings (attributes as in _settings_props). Returns a report string; raises LoopError."""
    obj = context.edit_object
    me = obj.data
    bm = bmesh.from_edit_mesh(me)
    # (adding a layer invalidates vertex references, so do it before reading anything)
    ring_layer = bm.verts.layers.int.get(RING_LAYER) or bm.verts.layers.int.new(RING_LAYER)
    bm.normal_update()
    width = p.width if p.width > 0 else auto_width(bm)

    loops = selected_loops(bm)
    merged = 0
    if p.cleanup:
        cleaned = []
        for seq, cyclic, edges in loops:
            keep = choose_side(seq, edges) if p.keep_side == 'AUTO' else (0 if p.keep_side == 'A' else 1)
            order, m = cleanup(bm, seq, cyclic, edges, keep, p.threshold * width)
            merged += m
            cleaned.append(loop_from_verts(bm, order, cyclic))
        loops = cleaned
        bm.normal_update()

    dist = p.offset * width
    loop_edges_all = []
    cleaned_n = 0
    for v in bm.verts:
        v[ring_layer] = 0
    for seq, cyclic, edges in loops:
        a, b = split_sides(seq, edges)
        protect = set(seq)
        offs_a, strip_a, _ = offset_side(bm, seq, cyclic, edges, a, dist, loop_slide=p.loop_slide)
        offs_b, strip_b, _ = offset_side(bm, seq, cyclic, edges, b, dist, loop_slide=p.loop_slide)
        protect |= set(offs_a) | set(offs_b)
        cleaned_n += clean_offset(bm, offs_a + offs_b, protect, p.offset_clean * width,
                                  seq=seq, cyclic=cyclic, dist=dist, rings=(offs_a, offs_b))
        dissolve_ring_slivers(bm, offs_a, strip_a)
        dissolve_ring_slivers(bm, offs_b, strip_b)
        bm.normal_update()
        merge_folded(bm, offs_a, strip_a)
        merge_folded(bm, offs_b, strip_b)
        for v in offs_a + offs_b:
            if v.is_valid:
                v[ring_layer] = 1
        loop_edges_all += [bm.edges.get((seq[i], seq[(i + 1) % len(seq)]))
                           for i in range(len(seq) if cyclic else len(seq) - 1)]
    near = {e for e in bm.edges if e.is_valid and e.calc_length() < width * 1e-3}
    if near:
        bmesh.ops.dissolve_degenerate(bm, dist=width * 1e-3, edges=list(near))

    # Bevel the loop.
    for g in list(bm.verts) + list(bm.edges) + list(bm.faces):
        g.select = False
    for e in loop_edges_all:
        if e is not None and e.is_valid:
            e.select_set(True)
    bm.normal_update()
    bmesh.update_edit_mesh(me)
    context.tool_settings.mesh_select_mode = (False, True, False)
    bpy.ops.mesh.bevel(offset=width, segments=p.segments, profile=p.profile,
                       loop_slide=True, clamp_overlap=False, affect='EDGES')

    # Fix the bevel, then fill the n-gons around it (faces touching the support loops).
    bm = bmesh.from_edit_mesh(me)
    bm.verts.index_update()
    st = fix.fix(bm, weld=p.fix_weld)
    ring_layer = bm.verts.layers.int.get(RING_LAYER)
    filled = 0
    if ring_layer is not None:
        around = {f for v in bm.verts if v[ring_layer] for f in v.link_faces if not f.select}
        filled = fill_ngons(bm, around, p.fill, p.reach)
        bm.verts.layers.int.remove(ring_layer)
    bm.normal_update()
    bmesh.update_edit_mesh(me)
    return ("Width %.4g: cleanup merged %d, offset cleaned %d, fix %d sides / %d welded, %d n-gons filled"
            % (width, merged, cleaned_n, st["affected"], st["welded"], filled))


# ----------------------------------------------------------------------------
# Operator (interactive like Ctrl+B: drag for the width, wheel for segments)

class MESH_OT_hb_boolean_bevel(bpy.types.Operator):
    """Bevel a boolean intersection loop cleanly: Boolean Cleanup, support loops on both sides,
offset cleanup, bevel, fix, fill the n-gons around it.
Drag to set the width, mouse wheel for segments, click to confirm"""
    bl_idname = "mesh.hb_boolean_bevel"
    bl_label = "Boolean Bevel"
    bl_options = {'REGISTER', 'UNDO', 'GRAB_CURSOR', 'BLOCKING'}

    __annotations__ = _settings_props()

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def draw(self, context):
        self.layout.use_property_split = True
        draw_settings_column(self.layout, self)

    def execute(self, context):
        try:
            msg = run_pipeline(context, self)
        except LoopError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        self.report({'INFO'}, msg)
        return {'FINISHED'}

    # --- interactive ------------------------------------------------------
    def _restore(self, context):
        bpy.ops.object.mode_set(mode='OBJECT')
        self.init_bm.to_mesh(self.obj.data)
        bpy.ops.object.mode_set(mode='EDIT')

    def _preview(self, context):
        self._restore(context)
        try:
            msg = run_pipeline(context, self)
        except LoopError as ex:
            return str(ex)
        return msg

    def _header(self, context, msg=""):
        context.area.header_text_set(
            "Boolean Bevel  Width %.4g  Segments %d   |  move: width, Shift: precise, wheel: segments, "
            "LMB/Enter: confirm, RMB/Esc: cancel   %s" % (self.width, self.segments, msg))

    def invoke(self, context, event):
        from bpy_extras import view3d_utils
        st = context.scene.hb_boolbevel
        for n in PROP_NAMES:  # start from the Tool Settings
            setattr(self, n, getattr(st, n))
        self.obj = context.edit_object
        bm = bmesh.from_edit_mesh(self.obj.data)
        if not any(e.select for e in bm.edges):
            self.report({'WARNING'}, "Select the boolean intersection loop (edges)")
            return {'CANCELLED'}
        if self.width <= 0:
            self.width = auto_width(bm)
        sel = [v.co for v in bm.verts if v.select]
        centre_local = sum(sel, Vector()) / len(sel)
        self.obj.update_from_editmode()
        self.init_bm = bmesh.new()
        self.init_bm.from_mesh(self.obj.data)

        rv3d, region = context.region_data, context.region
        c2d = view3d_utils.location_3d_to_region_2d(region, rv3d, self.obj.matrix_world @ centre_local) \
            if rv3d else None
        self.center = c2d if c2d is not None else Vector((region.width / 2, region.height / 2))
        mouse = Vector((event.mouse_region_x, event.mouse_region_y))
        self.d0 = max((mouse - self.center).length, 30.0)
        self.w0 = self.width
        self.last = (None, None)
        self.precise_anchor = None
        msg = self._preview(context)
        self.last = (self.width, self.segments)
        self._header(context, msg)
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        if event.type in {'MIDDLEMOUSE', 'TRACKPADPAN', 'TRACKPADZOOM'} or \
                (event.type in {'WHEELUPMOUSE', 'WHEELDOWNMOUSE'} and event.ctrl):
            return {'PASS_THROUGH'}
        changed = False
        if event.type == 'MOUSEMOVE':
            mouse = Vector((event.mouse_region_x, event.mouse_region_y))
            ratio = (mouse - self.center).length / self.d0
            w = self.w0 * ratio
            if event.shift:
                if self.precise_anchor is None:
                    self.precise_anchor = (w, self.width)
                w = self.precise_anchor[1] + (w - self.precise_anchor[0]) * 0.1
            else:
                self.precise_anchor = None
            self.width = max(w, 1e-5)
            changed = True
        elif event.type in {'WHEELUPMOUSE', 'NUMPAD_PLUS', 'PAGE_UP'} and event.value == 'PRESS':
            self.segments = min(64, self.segments + 1)
            changed = True
        elif event.type in {'WHEELDOWNMOUSE', 'NUMPAD_MINUS', 'PAGE_DOWN'} and event.value == 'PRESS':
            self.segments = max(1, self.segments - 1)
            changed = True
        elif event.type in {'LEFTMOUSE', 'RET', 'NUMPAD_ENTER', 'SPACE'} and event.value == 'PRESS':
            if self.last != (self.width, self.segments):
                self._preview(context)
            context.area.header_text_set(None)
            st = context.scene.hb_boolbevel
            for n in PROP_NAMES:  # remember for next time / show in Tool Settings
                setattr(st, n, getattr(self, n))
            self.init_bm.free()
            return {'FINISHED'}
        elif event.type in {'RIGHTMOUSE', 'ESC'} and event.value == 'PRESS':
            self._restore(context)
            self.init_bm.free()
            context.area.header_text_set(None)
            return {'CANCELLED'}
        if changed:
            lw, ls = self.last
            if ls != self.segments or lw is None or abs(self.width - lw) > 0.01 * max(lw, 1e-9):
                msg = self._preview(context)
                self.last = (self.width, self.segments)
                self._header(context, msg)
        return {'RUNNING_MODAL'}


# ----------------------------------------------------------------------------
# Toolbar tool with the settings in the Tool Settings bar

class VIEW3D_PT_hb_boolbevel_more(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Boolean Bevel Options"

    def draw(self, context):
        self.layout.use_property_split = True
        draw_settings_column(self.layout, context.scene.hb_boolbevel)


class HB_TL_boolean_bevel(bpy.types.WorkSpaceTool):
    bl_space_type = 'VIEW_3D'
    bl_context_mode = 'EDIT_MESH'
    bl_idname = "hb_bevelfix.boolean_bevel"
    bl_label = "Boolean Bevel"
    bl_description = ("Select the boolean intersection loop, then drag in the viewport to bevel it cleanly "
                      "(wheel: segments). Click still selects")
    bl_icon = "ops.mesh.bevel"
    bl_keymap = (
        ("mesh.hb_boolean_bevel", {"type": 'LEFTMOUSE', "value": 'CLICK_DRAG'}, None),
    )

    def draw_settings(context, layout, tool):
        st = context.scene.hb_boolbevel
        layout.prop(st, "width")
        layout.prop(st, "segments")
        layout.prop(st, "profile", slider=True)
        layout.prop(st, "fill", text="")
        layout.popover("VIEW3D_PT_hb_boolbevel_more", text="", icon='PREFERENCES')


def register():
    bpy.utils.register_class(HBBoolBevelSettings)
    bpy.types.Scene.hb_boolbevel = bpy.props.PointerProperty(type=HBBoolBevelSettings)
    bpy.utils.register_class(MESH_OT_hb_boolean_bevel)
    bpy.utils.register_class(VIEW3D_PT_hb_boolbevel_more)
    try:
        bpy.utils.register_tool(HB_TL_boolean_bevel, after={"builtin.bevel"}, separator=False)
    except Exception as ex:
        print("HB_BevelFix: Boolean Bevel tool not added (%s)" % ex)


def unregister():
    try:
        bpy.utils.unregister_tool(HB_TL_boolean_bevel)
    except Exception:
        pass
    bpy.utils.unregister_class(VIEW3D_PT_hb_boolbevel_more)
    bpy.utils.unregister_class(MESH_OT_hb_boolean_bevel)
    del bpy.types.Scene.hb_boolbevel
    bpy.utils.unregister_class(HBBoolBevelSettings)
