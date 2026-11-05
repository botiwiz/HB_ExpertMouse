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

from mathutils import Vector
from mathutils.geometry import intersect_line_line, intersect_line_plane

MAX_SEGMENTS = 128


class Section:
    __slots__ = ("verts", "o", "rails", "s", "L", "ok", "free", "nbrs", "key")

    def __init__(self, verts):
        self.verts = verts               # [c0, ..., ck]
        self.o = None                    # original corner
        self.rails = [None, None]        # unit direction o -> rail end, per side
        self.s = [0.0, 0.0]              # slide distance from o, per side
        self.L = [0.0, 0.0]              # rail length from o to its far vertex, per side
        self.ok = [False, False]
        self.free = [False, False]       # no edge leads back to the corner: slide on the surface
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


def fix(bm, tolerance=0.35, clamp=0.85, blend=1, iterations=60, fix_all=False):
    """Fix the selected bevel. Returns (sections, affected sides, moved vertices)."""
    sections = build_sections(bm)
    if not sections:
        return 0, 0, 0

    def nbr_values(sec, side):
        return [n.s[_side_of(same, side)] for n, same in sec.nbrs if n.ok[_side_of(same, side)]]

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
    by_id = {id(s): s for s in sections}
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
    if not affected:
        return len(sections), 0, 0

    # 3) smooth the slides of affected sides (healthy ones stay fixed)
    new_s = {k: by_id[k[0]].s[k[1]] for k in affected}
    for _ in range(iterations):
        for (sid, side) in affected:
            sec = by_id[sid]
            vals = []
            for n, same in sec.nbrs:
                ns = _side_of(same, side)
                if not n.ok[ns]:
                    continue
                vals.append(new_s.get((id(n), ns), n.s[ns]))
            if not vals:  # isolated: just pull inside the rail
                target = sec.s[side]
            else:
                target = sum(vals) / len(vals)
            new_s[(sid, side)] = max(1e-6, min(target, clamp * sec.L[side]))

    # 4) apply: slide outer vertices, re-map the profile in between
    moved = 0
    touched = {sid for sid, _ in affected}
    for sid in touched:
        sec = by_id[sid]
        o = sec.o
        old = [v.co.copy() for v in sec.verts]
        new_ends = []
        for side, c_old in ((0, old[0]), (1, old[-1])):
            key = (sid, side)
            if key in new_s:
                new_ends.append(o + sec.rails[side] * new_s[key])
            else:
                new_ends.append(c_old)
        u_old, w_old = old[0] - o, old[-1] - o
        u_new, w_new = new_ends[0] - o, new_ends[1] - o
        n_old = u_old.cross(w_old)
        n_new = u_new.cross(w_new)
        if n_old.length < 1e-12 or n_new.length < 1e-12:
            continue
        # keep the out-of-plane part of the profile at the same scale relative to the width
        n_old_u = n_old.normalized() * (old[0] - old[-1]).length
        n_new_u = n_new.normalized() * (new_ends[0] - new_ends[1]).length
        from mathutils import Matrix
        basis_old = Matrix((u_old, w_old, n_old_u)).transposed()
        try:
            inv = basis_old.inverted()
        except ValueError:
            continue
        basis_new = Matrix((u_new, w_new, n_new_u)).transposed()
        for i, v in enumerate(sec.verts):
            if i == 0:
                co = new_ends[0]
            elif i == len(sec.verts) - 1:
                co = new_ends[1]
            else:
                coeff = inv @ (old[i] - o)
                co = o + basis_new @ coeff
            if (co - v.co).length > 1e-9:
                v.co = co
                moved += 1
    return len(sections), len(affected), moved
