# SPDX-License-Identifier: GPL-2.0-or-later
"""Quad patches from a closed outline of corner points, fitted onto the surfaces.

4 corners: a grid (opposite sides share a segment count).
Any other count: one quad region per corner, meeting at a centre pole (corner, middle of the next
side, centre, middle of the previous side) - all quads, like subdividing the n-gon once.
Every region is filled with a Coons patch built from its (surface-projected) borders, then the
whole patch is projected onto the surface and relaxed.
"""

import bmesh
from mathutils import Vector


def _project(surf, co):
    loc, _n = surf.nearest(co) if surf else (co, None)
    return loc


def curve(surf, a, b, n):
    """n segments from a to b, each point projected onto the surface (ends kept exact)."""
    pts = [a.copy()]
    for k in range(1, n):
        pts.append(_project(surf, a.lerp(b, k / n)))
    pts.append(b.copy())
    return pts


def coons(bottom, top, left, right):
    """Grid points [j][i] from 4 border point lists:
    bottom/top run left->right (len nu+1), left/right run bottom->top (len nv+1)."""
    nu, nv = len(bottom) - 1, len(left) - 1
    p00, p10, p01, p11 = bottom[0], bottom[nu], top[0], top[nu]
    grid = []
    for j in range(nv + 1):
        v = j / nv
        row = []
        for i in range(nu + 1):
            u = i / nu
            if j == 0:
                row.append(bottom[i].copy())
            elif j == nv:
                row.append(top[i].copy())
            elif i == 0:
                row.append(left[j].copy())
            elif i == nu:
                row.append(right[j].copy())
            else:
                p = ((1 - v) * bottom[i] + v * top[i] + (1 - u) * left[j] + u * right[j]
                     - ((1 - u) * (1 - v) * p00 + u * (1 - v) * p10 + (1 - u) * v * p01 + u * v * p11))
                row.append(p)
        grid.append(row)
    return grid


def _counts_for_quad(corners, resolution):
    lens = [(corners[(k + 1) % 4] - corners[k]).length for k in range(4)]
    a = (lens[0] + lens[2]) / 2
    b = (lens[1] + lens[3]) / 2
    longest = max(a, b, 1e-12)
    nu = max(1, round(resolution * a / longest))
    nv = max(1, round(resolution * b / longest))
    return nu, nv


def patch_grids(surf, corners, resolution, layout='AUTO', fixed=None):
    """List of point grids (one per region) for the outline `corners` (world space).
    fixed: {side k: [points from corner k to corner k+1]} - sides that must reuse existing
    vertices (an open border of the mesh), which also sets the segment counts."""
    n = len(corners)
    fixed = dict(fixed or {})
    corners = [c.copy() if any(k in fixed for k in (i, (i - 1) % n)) else _project(surf, c)
               for i, c in enumerate(corners)]
    if n == 4 and layout in {'AUTO', 'GRID'}:
        c0, c1, c2, c3 = corners
        nu, nv = _counts_for_quad(corners, resolution)
        for pair, setter in (((0, 2), 'u'), ((1, 3), 'v')):
            counts = [len(fixed[k]) - 1 for k in pair if k in fixed]
            if counts:
                if setter == 'u':
                    nu = counts[0]
                else:
                    nv = counts[0]
                for k in pair:  # the opposite side can't follow a border with another count
                    if k in fixed and len(fixed[k]) - 1 != counts[0]:
                        del fixed[k]

        def side(k, a, b, cnt, reverse=False):
            if k in fixed:
                pts = [p.copy() for p in fixed[k]]
                return list(reversed(pts)) if reverse else pts
            return curve(surf, b, a, cnt) if reverse else curve(surf, a, b, cnt)

        bottom = side(0, c0, c1, nu)
        right = side(1, c1, c2, nv)
        top = side(2, c2, c3, nu, reverse=True)      # c3 -> c2
        left = side(3, c3, c0, nv, reverse=True)     # c0 -> c3
        return [coons(bottom, top, left, right)]

    m = max(1, round(resolution / 2))
    even = [len(pts) - 1 for pts in fixed.values() if (len(pts) - 1) % 2 == 0 and len(pts) > 2]
    if even:
        m = even[0] // 2
    fixed = {k: pts for k, pts in fixed.items() if len(pts) - 1 == 2 * m}
    centre = _project(surf, sum(corners, Vector()) / n)
    mids, first_half, second_half = [], [], []
    for k in range(n):
        if k in fixed:
            pts = [p.copy() for p in fixed[k]]
            mids.append(pts[m])
            first_half.append(pts[:m + 1])
            second_half.append(pts[m:])
        else:
            mid = _project(surf, (corners[k] + corners[(k + 1) % n]) / 2)
            mids.append(mid)
            first_half.append(curve(surf, corners[k], mid, m))                    # c_k -> m_k
            second_half.append(curve(surf, mid, corners[(k + 1) % n], m))         # m_k -> c_k+1
    spokes = [curve(surf, mids[k], centre, m) for k in range(n)]                  # m_k -> C
    grids = []
    for k in range(n):
        prev = (k - 1) % n
        bottom = first_half[k]                          # c_k -> m_k
        right = spokes[k]                               # m_k -> C
        left = list(reversed(second_half[prev]))        # c_k -> m_prev
        top = spokes[prev]                              # m_prev -> C
        grids.append(coons(bottom, top, left, right))
    return grids


def build(bm, surf, corners, resolution, relax=3, layout='AUTO', weld_to=(), weld_dist=0.0, mw_inv=None,
          fixed=None):
    """Create the patch in `bm` (object-local via mw_inv). Returns the new faces."""
    grids = patch_grids(surf, corners, resolution, layout, fixed)
    to_local = (lambda p: mw_inv @ p) if mw_inv is not None else (lambda p: p)
    new_verts, new_faces = [], []
    for grid in grids:
        rows = [[bm.verts.new(to_local(p)) for p in row] for row in grid]
        for r in rows:
            new_verts += r
        for j in range(len(rows) - 1):
            for i in range(len(rows[0]) - 1):
                new_faces.append(bm.faces.new((rows[j][i], rows[j][i + 1], rows[j + 1][i + 1], rows[j + 1][i])))
    # stitch the regions together (their shared borders are identical points)
    size = max((max(c.length for c in corners), 1.0))
    res = bmesh.ops.remove_doubles(bm, verts=new_verts, dist=1e-6 * size)
    new_verts = [v for v in new_verts if v.is_valid]
    new_faces = [f for f in new_faces if f.is_valid]
    bmesh.ops.recalc_face_normals(bm, faces=new_faces)

    if relax and surf:
        _relax(bm, new_verts, surf, relax, mw_inv)
    if weld_to and weld_dist > 0:
        targets = [v for v in weld_to if v.is_valid]
        bmesh.ops.remove_doubles(bm, verts=[v for v in new_verts if v.is_valid] + targets, dist=weld_dist)
        new_faces = [f for f in new_faces if f.is_valid]
    _orient(bm, new_faces, surf, mw_inv)
    return new_faces


def _relax(bm, verts, surf, iterations, mw_inv):
    """Even out the interior (boundary and poles' neighbours keep their borders), re-projecting."""
    mw = mw_inv.inverted() if mw_inv is not None else None
    interior = [v for v in verts if v.is_valid and not v.is_boundary]
    for _ in range(iterations):
        new = {}
        for v in interior:
            nb = [e.other_vert(v).co for e in v.link_edges]
            if nb:
                new[v] = sum(nb, Vector()) / len(nb)
        for v, co in new.items():
            w = mw @ co if mw is not None else co
            w = _project(surf, w)
            v.co = mw_inv @ w if mw_inv is not None else w


def _orient(bm, faces, surf, mw_inv):
    """Point the patch's normals the same way as the surface underneath."""
    if not faces or not surf:
        return
    mw = mw_inv.inverted() if mw_inv is not None else None
    score = 0.0
    for f in faces[:: max(1, len(faces) // 50)]:
        c = f.calc_center_median()
        w = mw @ c if mw is not None else c
        _loc, n = surf.nearest(w)
        if n is not None:
            fn = (mw.to_3x3() @ f.normal) if mw is not None else f.normal
            score += fn.dot(n)
    if score < 0:
        bmesh.ops.reverse_faces(bm, faces=faces)
