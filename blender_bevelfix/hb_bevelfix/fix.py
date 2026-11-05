# SPDX-License-Identifier: GPL-2.0-or-later
"""Fix bevel overlaps by edge-sliding the affected bevel vertices to smoothed offsets.

The selected faces are taken as the bevel. It is read as a strip of cross-sections
(c0 ... ck across the bevel). For each section the two rails (the unselected edges the
outer bevel vertices slid along) are intersected to recover the original corner `o`.
Each outer vertex's slide is then  s = distance from o along its rail.

Overshoot (the vertex slid past the far end of its rail) or a slide that jumps compared
with its neighbours marks a side as affected; affected slides are replaced by the average
of their neighbours (iterated, the healthy ones stay fixed), clamped inside the rail.
The inner profile vertices are re-mapped so the profile keeps its shape.
"""

import bmesh
from mathutils import Vector
from mathutils.geometry import intersect_line_line, intersect_line_plane

MAX_SEGMENTS = 128


class Section:
    __slots__ = ("verts", "o", "rails", "s", "L", "ok", "free", "ends", "nbrs", "key")

    def __init__(self, verts):
        self.verts = verts               # [c0, ..., ck]
        self.o = None                    # original corner
        self.rails = [None, None]        # unit direction o -> rail end, per side
        self.s = [0.0, 0.0]              # slide distance from o, per side
        self.L = [0.0, 0.0]              # rail length from o to its far vertex, per side
        self.ok = [False, False]
        self.free = [False, False]       # no edge leads back to the corner: slide on the surface
        self.ends = [None, None]         # far vertex of each side's rail
        self.nbrs = []                   # (other section, same_orientation)
        self.key = tuple(sorted((verts[0].index, verts[-1].index)))


def _sel_face_count(e):
    return sum(1 for f in e.link_faces if f.select)


def _walk_section(v0, boundary):
    """Walk across the bevel from boundary vertex v0 to the other side."""
    rungs = [e for e in v0.link_edges if _sel_face_count(e) == 2]
    if len(rungs) != 1:
        return None
    verts = [v0]
    e = rungs[0]
    v = e.other_vert(v0)
    for _ in range(MAX_SEGMENTS):
        verts.append(v)
        if v in boundary:
            return verts
        faces_e = set(e.link_faces)
        nxt = [x for x in v.link_edges
               if x is not e and _sel_face_count(x) == 2 and not (set(x.link_faces) & faces_e)]
        if len(nxt) != 1:
            return None
        e = nxt[0]
        v = e.other_vert(v)
    return None


def _rail_candidates(v):
    """Far vertices of the unselected edges at boundary vertex v (the edges it may slide on)."""
    return [e.other_vert(v) for e in v.link_edges
            if _sel_face_count(e) == 0 and (e.other_vert(v).co - v.co).length > 1e-12]


EXACT_GAP = 0.02   # rails meeting this closely (relative to bevel width) are trusted outright
MAX_GAP = 0.25


def _pair_candidates(c0, ck, cands0, cands1):
    """Corners where a rail of each side meets: [(gap, o, a0, a1)]."""
    width = max((ck.co - c0.co).length, 1e-9)
    out = []
    for a0 in cands0:
        d0 = (a0.co - c0.co).normalized()
        for a1 in cands1:
            d1 = (a1.co - ck.co).normalized()
            if d0.cross(d1).length < 1e-4:
                continue  # parallel rails: no corner
            res = intersect_line_line(c0.co, a0.co, ck.co, a1.co)
            if not res:
                continue
            gap = (res[0] - res[1]).length / width
            if gap <= MAX_GAP:
                out.append((gap, (res[0] + res[1]) / 2, a0, a1))
    return out


def _surface_normal(v):
    ns = [f.normal for f in v.link_faces if not f.select]
    n = sum(ns, Vector())
    return n.normalized() if n.length > 1e-9 else None


def _one_sided_candidates(c0, ck, cands0, cands1):
    """Corners from one side's rail meeting the surface the other side lies on:
    [(None, o, a0 or None, a1 or None)]. Used where a side has no rail that reaches the corner
    (e.g. the bevel placed that vertex inside a face)."""
    out = []
    width = max((ck.co - c0.co).length, 1e-9)
    for c_rail, c_other, cands, side in ((c0, ck, cands0, 0), (ck, c0, cands1, 1)):
        n = _surface_normal(c_other)
        if n is None:
            continue
        for a in cands:
            d = (a.co - c_rail.co).normalized()
            if abs(d.dot(n)) < 0.05:
                continue
            o = intersect_line_plane(c_rail.co, a.co, c_other.co, n)
            if o is None or (o - c_rail.co).length > 3 * width or (o - c_other.co).length > 3 * width:
                continue
            out.append((None, o, a, None) if side == 0 else (None, o, None, a))
    return out


def _set_corner(sec, cand):
    _gap, o, a0, a1 = cand
    c0, ck = sec.verts[0], sec.verts[-1]
    sec.o = o
    width = max((ck.co - c0.co).length, 1e-9)
    # A side without a rail in the chosen corner may still have one passing through it.
    for side, c in ((0, c0), (1, ck)):
        if (a0, a1)[side] is None:
            for a in _rail_candidates(c):
                d = (a.co - c.co)
                t = (o - c.co).dot(d) / d.length_squared
                if (c.co + d * t - o).length < EXACT_GAP * width:
                    if side == 0:
                        a0 = a
                    else:
                        a1 = a
                    break
    for side, (c, a) in enumerate(((c0, a0), (ck, a1))):
        sec.ok[side] = False
        sec.free[side] = False
        if a is None:
            # Slide on the surface this vertex lies on, straight away from the corner.
            n = _surface_normal(c)
            d = c.co - o
            if n is not None:
                d = d - n * d.dot(n)
            if d.length < 1e-9:
                continue
            d.normalize()
            sec.rails[side] = d
            sec.L[side] = float("inf")
            sec.s[side] = (c.co - o).dot(d)
            sec.ok[side] = True
            sec.free[side] = True
            continue
        ra = a.co - o
        if ra.length < 1e-12:
            continue
        d = ra.normalized()
        sec.rails[side] = d
        sec.L[side] = ra.length
        sec.s[side] = (c.co - o).dot(d)
        sec.ok[side] = True
        sec.ends[side] = a


def _nbr_slide(sec, side):
    vals = sorted(n.s[side if same else 1 - side] for n, same in sec.nbrs
                  if n.o is not None and n.ok[side if same else 1 - side]
                  and not n.free[side if same else 1 - side])
    return vals[len(vals) // 2] if vals else None


def _score(sec, cand, ref, width):
    """Lower is better: close to the neighbours' corners, and the rail side(s) slide like
    their neighbours (a healthy side pins down the corner)."""
    _gap, o, a0, a1 = cand
    score = (o - ref).length / width
    for side, (c, a) in enumerate(((sec.verts[0], a0), (sec.verts[-1], a1))):
        if a is None:
            continue
        ra = a.co - o
        if ra.length < 1e-12:
            return float("inf")
        sl = (c.co - o).dot(ra.normalized())
        if not (0.0 < sl < ra.length):
            score += 5.0  # this corner would mean the vertex slid off its own edge: unlikely
        med = _nbr_slide(sec, side)
        if med:
            score += 2.0 * abs(sl - med) / med
    return score


def build_sections(bm):
    sel_faces = [f for f in bm.faces if f.select]
    if not sel_faces:
        return []
    boundary = set()
    for f in sel_faces:
        for e in f.edges:
            if _sel_face_count(e) == 1:
                boundary.update(e.verts)

    sections, by_key, by_end = [], {}, {}
    for v in boundary:
        verts = _walk_section(v, boundary)
        if not verts or len(verts) < 2:
            continue
        sec = Section(verts)
        if sec.key in by_key:
            continue
        by_key[sec.key] = sec
        sections.append(sec)
        by_end[verts[0]] = (sec, 0)
        by_end[verts[-1]] = (sec, 1)

    # Neighbours along the strip (sections whose ends share a boundary edge).
    for sec in sections:
        for side, end in ((0, sec.verts[0]), (1, sec.verts[-1])):
            for e in end.link_edges:
                if _sel_face_count(e) != 1:
                    continue
                other = e.other_vert(end)
                if other not in by_end:
                    continue
                osec, oside = by_end[other]
                if osec is sec:
                    continue
                same = (oside == side)
                if not any(n is osec for n, _ in sec.nbrs):
                    sec.nbrs.append((osec, same))
    # Open strip ends: the last slice's rungs border only one selected face, so the walk
    # above can't find it. Step one row of faces outward from its neighbour instead.
    for sec in list(sections):
        if len(sec.nbrs) != 1:
            continue
        nb = sec.nbrs[0][0]
        nb_verts = set(nb.verts)
        row = []
        for j in range(len(sec.verts) - 1):
            p, q = sec.verts[j], sec.verts[j + 1]
            e = p.link_edges and next((x for x in p.link_edges if x.other_vert(p) is q), None)
            faces = [f for f in (e.link_faces if e else []) if f.select and not (set(f.verts) & nb_verts)]
            if len(faces) != 1 or len(faces[0].verts) != 4:
                row = None
                break
            fv = list(faces[0].verts)
            i = fv.index(p)
            # quad p, q, q', p' (in either winding)
            if fv[(i + 1) % 4] is q:
                p2, q2 = fv[(i + 3) % 4], fv[(i + 2) % 4]
            else:
                p2, q2 = fv[(i + 1) % 4], fv[(i + 2) % 4]
            if row and row[-1] is not p2:
                row = None
                break
            if not row:
                row.append(p2)
            row.append(q2)
        if not row or len(row) != len(sec.verts) or row[0] not in boundary or row[-1] not in boundary:
            continue
        end = Section(row)
        if end.key in by_key or row[0] in by_end or row[-1] in by_end:
            continue
        by_key[end.key] = end
        sections.append(end)
        by_end[row[0]] = (end, 0)
        by_end[row[-1]] = (end, 1)
        end.nbrs.append((sec, True))
        sec.nbrs.append((end, True))

    # Recover the original corner of each section and the slide of each side.
    cands = {}
    for sec in sections:
        c0, ck = sec.verts[0], sec.verts[-1]
        r0, r1 = _rail_candidates(c0), _rail_candidates(ck)
        pairs = _pair_candidates(c0, ck, r0, r1)
        cands[id(sec)] = pairs + _one_sided_candidates(c0, ck, r0, r1)
        exact = [p for p in pairs if p[0] < EXACT_GAP]
        if exact:
            _set_corner(sec, min(exact, key=lambda p: p[0]))

    # The rest: the candidate that continues the neighbours' corners (the original edge is smooth).
    for _ in range(len(sections)):
        progress = False
        for sec in sections:
            if sec.o is not None or not cands[id(sec)]:
                continue
            ref = [n.o for n, _same in sec.nbrs if n.o is not None]
            if not ref:
                continue
            ref = sum(ref, Vector()) / len(ref)
            width = max((sec.verts[-1].co - sec.verts[0].co).length, 1e-9)
            best = min(cands[id(sec)], key=lambda cd: _score(sec, cd, ref, width))
            _set_corner(sec, best)
            progress = True
        if not progress:
            break
    return sections


def _side_of(nbr_same, side):
    return side if nbr_same else 1 - side


def _weldable(sec, side, bevel_verts):
    """The rail's far vertex may be merged into the bevel vertex (weld mode)."""
    a = sec.ends[side]
    return (a is not None and not sec.free[side] and a.is_valid and not a.select
            and a not in bevel_verts)


def _chain_positions(sections):
    """0..1 position of each section along its open strip (for tapering); cyclic strips get 1."""
    pos = {}
    seen = set()
    for start in sections:
        if id(start) in seen:
            continue
        # collect the component
        comp, stack = [], [start]
        seen.add(id(start))
        while stack:
            sec = stack.pop()
            comp.append(sec)
            for n, _same in sec.nbrs:
                if id(n) not in seen:
                    seen.add(id(n))
                    stack.append(n)
        ends = [sec for sec in comp if len(sec.nbrs) <= 1]
        if not ends:
            for sec in comp:
                pos[id(sec)] = 1.0
            continue
        dist = {id(ends[0]): 0}
        queue = [ends[0]]
        for sec in queue:
            for n, _same in sec.nbrs:
                if id(n) not in dist:
                    dist[id(n)] = dist[id(sec)] + 1
                    queue.append(n)
        top = max(dist.values()) or 1
        for sec in comp:
            pos[id(sec)] = dist.get(id(sec), 0) / top
    return pos


def _apply_section(sec, new_ends):
    """Move a section's outer vertices and re-map its profile. Returns moved vertex count."""
    from mathutils import Matrix
    o = sec.o
    old = [v.co.copy() for v in sec.verts]
    u_old, w_old = old[0] - o, old[-1] - o
    u_new, w_new = new_ends[0] - o, new_ends[1] - o
    n_old, n_new = u_old.cross(w_old), u_new.cross(w_new)
    if n_old.length < 1e-12 or n_new.length < 1e-12:
        return 0
    # keep the out-of-plane part of the profile at the same scale relative to the width
    basis_old = Matrix((u_old, w_old, n_old.normalized() * (old[0] - old[-1]).length)).transposed()
    basis_new = Matrix((u_new, w_new, n_new.normalized() * (new_ends[0] - new_ends[1]).length)).transposed()
    try:
        inv = basis_old.inverted()
    except ValueError:
        return 0
    moved = 0
    last = len(sec.verts) - 1
    for i, v in enumerate(sec.verts):
        co = new_ends[0] if i == 0 else new_ends[1] if i == last else o + basis_new @ (inv @ (old[i] - o))
        if (co - v.co).length > 1e-9:
            v.co = co
            moved += 1
    return moved


def _collapse_thin(bm, sections, threshold):
    """Merge neighbouring sections whose vertices all lie within `threshold` of each other
    (removes the sliver faces between them). In the spirit of MESHmachine's Boolean Cleanup,
    which merges loop vertices into their closest neighbour within a threshold."""
    parent = {id(s): id(s) for s in sections}
    by_id = {id(s): s for s in sections}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def aligned(sec, other, same):
        vs = other.verts if same else list(reversed(other.verts))
        return vs if len(vs) == len(sec.verts) else None

    for sec in sections:
        if not all(v.is_valid for v in sec.verts):
            continue
        for n, same in sec.nbrs:
            vs = aligned(sec, n, same)
            if vs is None or not all(v.is_valid for v in vs):
                continue
            if all((a.co - b.co).length <= threshold for a, b in zip(sec.verts, vs)):
                parent[find(id(sec))] = find(id(n))

    groups = {}
    for sec in sections:
        groups.setdefault(find(id(sec)), []).append(sec)
    targetmap = {}
    collapsed = 0
    for members in groups.values():
        if len(members) < 2:
            continue
        anchor = members[0]
        rows = []
        for sec in members:
            same = None
            if sec is anchor:
                rows.append(sec.verts)
                continue
            # orient each member like the anchor (by nearest end)
            d_same = (sec.verts[0].co - anchor.verts[0].co).length
            d_flip = (sec.verts[-1].co - anchor.verts[0].co).length
            rows.append(sec.verts if d_same <= d_flip else list(reversed(sec.verts)))
        for j in range(len(anchor.verts)):
            col = [row[j] for row in rows]
            avg = sum((v.co for v in col), Vector()) / len(col)
            col[0].co = avg
            for v in col[1:]:
                if v is not col[0] and v not in targetmap:
                    targetmap[v] = col[0]
        collapsed += len(members) - 1
    if targetmap:
        bmesh.ops.weld_verts(bm, targetmap=targetmap)
    return collapsed


def fix(bm, tolerance=0.35, clamp=0.85, blend=1, iterations=60, fix_all=False, weld=False,
        width=0.0, taper=False, taper_flip=False, merge_threshold=0.0):
    """Fix the selected bevel.

    weld: where a vertex's rail is too short for its slide, merge the conflicting vertex at
          the rail's far end into it instead of squeezing the bevel (like MESHmachine's
          Offset/Chamfer "Merge Perimeter").
    width: change every slide by this much (like MESHmachine's Change Width); with taper the
           change ramps from 0 to `width` along an open strip.
    merge_threshold: collapse neighbouring sections closer than this (like Boolean Cleanup).

    Returns a dict: sections, affected, moved, welded, collapsed."""
    stats = dict(sections=0, affected=0, moved=0, welded=0, collapsed=0)
    sections = build_sections(bm)
    if not sections:
        return stats
    stats["sections"] = len(sections)
    by_id = {id(s): s for s in sections}
    bevel_verts = {v for sec in sections for v in sec.verts}

    def nbr_values(sec, side):
        return [n.s[_side_of(same, side)] for n, same in sec.nbrs if n.ok[_side_of(same, side)]]

    def cap_for(sid, side):
        sec = by_id[sid]
        if weld and _weldable(sec, side, bevel_verts):
            return float("inf")
        return clamp * sec.L[side]

    # 1) detect affected sides
    affected = set()
    for sec in sections:
        for side in (0, 1):
            if not sec.ok[side]:
                continue
            s, L = sec.s[side], sec.L[side]
            bad = fix_all or s <= 1e-9 or s >= L * (1.0 - 1e-4)
            if not bad:
                vals = nbr_values(sec, side)
                if vals:
                    ref = sorted(vals)[len(vals) // 2]
                    if ref > 1e-12 and abs(s - ref) > tolerance * ref:
                        bad = True
            if bad:
                affected.add((id(sec), side))

    # 2) blend into the surroundings
    frontier = set(affected)
    for _ in range(max(0, blend)):
        grow = set()
        for sid, side in frontier:
            for n, same in by_id[sid].nbrs:
                key = (id(n), _side_of(same, side))
                if n.ok[key[1]] and key not in affected:
                    grow.add(key)
        affected |= grow
        frontier = grow
    stats["affected"] = len(affected)

    # 3) smooth the slides of affected sides (healthy ones stay fixed)
    new_s = {k: by_id[k[0]].s[k[1]] for k in affected}
    cap = {k: cap_for(*k) for k in affected}
    for _ in range(iterations if affected else 0):
        for (sid, side) in affected:
            sec = by_id[sid]
            vals = [new_s.get((id(n), _side_of(same, side)), n.s[_side_of(same, side)])
                    for n, same in sec.nbrs if n.ok[_side_of(same, side)]]
            target = sum(vals) / len(vals) if vals else sec.s[side]
            new_s[(sid, side)] = max(1e-6, min(target, cap[(sid, side)]))

    # 3b) change the width of the whole bevel (optionally tapered along the strip)
    if abs(width) > 1e-12:
        pos = _chain_positions(sections) if taper else {}
        for sec in sections:
            for side in (0, 1):
                if not sec.ok[side]:
                    continue
                key = (id(sec), side)
                f = pos.get(id(sec), 1.0) if taper else 1.0
                if taper and taper_flip:
                    f = 1.0 - f
                base = new_s.get(key, sec.s[side])
                cap.setdefault(key, cap_for(*key))
                new_s[key] = max(1e-6, min(base + width * f, cap[key]))

    # 4) welds: sides that now reach past their rail's far vertex
    welds = {}
    for (sid, side), sv in new_s.items():
        sec = by_id[sid]
        if cap[(sid, side)] == float("inf") and sv > clamp * sec.L[side]:
            a = sec.ends[side]
            if a in welds:  # already merged into another bevel vertex: just slide this one
                new_s[(sid, side)] = min(sv, clamp * sec.L[side])
                continue
            welds[a] = sec.verts[0] if side == 0 else sec.verts[-1]

    # 5) apply: slide outer vertices, re-map the profile in between
    for sid in {sid for sid, _ in new_s}:
        sec = by_id[sid]
        ends = [sec.o + sec.rails[side] * new_s[(sid, side)] if (sid, side) in new_s
                else (sec.verts[0] if side == 0 else sec.verts[-1]).co.copy() for side in (0, 1)]
        stats["moved"] += _apply_section(sec, ends)

    if welds:
        targets = set(welds.values())
        bmesh.ops.weld_verts(bm, targetmap=welds)
        stats["welded"] = len(welds)
        near = {e for v in targets if v.is_valid for e in v.link_edges}
        if near:
            size = max((e.calc_length() for e in near), default=1.0)
            bmesh.ops.dissolve_degenerate(bm, dist=size * 1e-4, edges=list(near))

    # 6) collapse sections that ended up (almost) on top of each other
    if merge_threshold > 0:
        stats["collapsed"] = _collapse_thin(bm, sections, merge_threshold)
    return stats
