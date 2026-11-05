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
from mathutils.geometry import intersect_line_line

MAX_SEGMENTS = 128


class Section:
    __slots__ = ("verts", "o", "rails", "s", "L", "ok", "nbrs", "key")

    def __init__(self, verts):
        self.verts = verts               # [c0, ..., ck]
        self.o = None                    # original corner
        self.rails = [None, None]        # unit direction o -> rail end, per side
        self.s = [0.0, 0.0]              # slide distance from o, per side
        self.L = [0.0, 0.0]              # rail length from o to its far vertex, per side
        self.ok = [False, False]
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


def _rail_end(v, inner):
    """Far vertex of the unselected edge at boundary vertex v that best continues the profile."""
    out = (v.co - inner.co)
    cands = [e for e in v.link_edges if _sel_face_count(e) == 0]
    if not cands:
        return None
    if out.length < 1e-12:
        return cands[0].other_vert(v)
    out = out.normalized()
    best = max(cands, key=lambda e: abs((e.other_vert(v).co - v.co).normalized().dot(out))
               if (e.other_vert(v).co - v.co).length > 1e-12 else -1)
    return best.other_vert(v)


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

    # Recover the original corner and the slide of each side.
    for sec in sections:
        c0, ck = sec.verts[0], sec.verts[-1]
        a0 = _rail_end(c0, sec.verts[1])
        a1 = _rail_end(ck, sec.verts[-2])
        if a0 is None or a1 is None:
            continue
        d0 = a0.co - c0.co
        d1 = a1.co - ck.co
        if d0.length < 1e-12 or d1.length < 1e-12:
            continue
        if d0.normalized().cross(d1.normalized()).length < 1e-4:
            continue  # rails parallel: no corner to recover
        res = intersect_line_line(c0.co, a0.co, ck.co, a1.co)
        if not res:
            continue
        p0, p1 = res
        width = (ck.co - c0.co).length
        if (p0 - p1).length > max(1e-6, 0.25 * width):
            continue  # rails don't meet: not a clean bevel section
        o = (p0 + p1) / 2
        sec.o = o
        for side, (c, a) in enumerate(((c0, a0), (ck, a1))):
            ra = a.co - o
            if ra.length < 1e-12:
                continue
            d = ra.normalized()
            sec.rails[side] = d
            sec.L[side] = ra.length
            sec.s[side] = (c.co - o).dot(d)
            sec.ok[side] = True

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
