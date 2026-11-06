# SPDX-License-Identifier: GPL-2.0-or-later
"""Quad patches from a closed outline, fitted onto the surfaces.

The outline is given as one polyline per side (corner k -> corner k+1); sides can be straight,
curved or freehand - anything. A side marked `fixed` (an open border of the mesh) keeps its points
exactly and sets the segment count; other sides are resampled evenly.

4 sides: a grid (opposite sides share a segment count).
Any other count: one quad region per corner around a centre pole (corner, middle of the next side,
centre, middle of the previous side) - all quads.
Each region is filled with a Coons patch from its borders, then everything is conformed to the
surface (strength / offset) and relaxed.
"""

import bmesh
from mathutils import Vector


class Params:
    """Patch settings (same names as the operator / tool properties)."""
    resolution = 8
    density_mode = 'SEGMENTS'
    edge_length = 0.1
    relax = 3
    side_smooth = 0
    layout = 'AUTO'
    conform = 'SURFACE'
    strength = 1.0
    offset = 0.0

    @classmethod
    def from_props(cls, data):
        p = cls()
        for k in ("resolution", "density_mode", "edge_length", "relax", "side_smooth", "layout",
                  "conform", "strength", "offset"):
            if hasattr(data, k):
                setattr(p, k, getattr(data, k))
        return p


# ----------------------------------------------------------------------------
# Conforming to the surface

def make_conform(surf, p):
    """Function world point -> conformed world point."""
    if surf is None or not surf or p.conform == 'NONE':
        return lambda co: co.copy()
    strength, offset = p.strength, p.offset

    def conform(co):
        loc, n = surf.nearest(co)
        if n is not None and offset:
            loc = loc + n * offset
        return co.lerp(loc, strength) if strength < 1.0 else loc
    return conform


# ----------------------------------------------------------------------------
# Polylines

def poly_length(poly):
    return sum((poly[i + 1] - poly[i]).length for i in range(len(poly) - 1))


def resample(poly, n):
    """n segments evenly spaced along the polyline (ends kept)."""
    if n < 1 or len(poly) < 2:
        return [p.copy() for p in poly]
    seg = [(poly[i + 1] - poly[i]).length for i in range(len(poly) - 1)]
    total = sum(seg)
    if total < 1e-12:
        return [poly[0].copy() for _ in range(n + 1)]
    out = [poly[0].copy()]
    i, acc = 0, 0.0
    for k in range(1, n):
        target = total * k / n
        while i < len(seg) - 1 and acc + seg[i] < target:
            acc += seg[i]
            i += 1
        t = (target - acc) / seg[i] if seg[i] > 1e-12 else 0.0
        out.append(poly[i].lerp(poly[i + 1], min(1.0, max(0.0, t))))
    out.append(poly[-1].copy())
    return out


def densify(poly, conform, spacing):
    """Insert points so no gap is longer than `spacing`, conformed to the surface (ends kept)."""
    out = [poly[0].copy()]
    for a, b in zip(poly, poly[1:]):
        steps = max(1, int((b - a).length / spacing))
        for s in range(1, steps):
            out.append(conform(a.lerp(b, s / steps)))
        out.append(b.copy())
    return out


def smooth(poly, iterations, conform):
    """Laplacian smoothing of the inner points (ends fixed), re-conformed each pass."""
    pts = [p.copy() for p in poly]
    for _ in range(iterations):
        pts = [pts[0]] + [conform((pts[i - 1] + pts[i] * 2 + pts[i + 1]) / 4) for i in range(1, len(pts) - 1)] + [pts[-1]]
    return pts


def catmull_rom(points, samples=12):
    """Smooth curve through all points (uniform Catmull-Rom, end tangents mirrored)."""
    if len(points) < 3:
        return [p.copy() for p in points]
    pts = [points[0] * 2 - points[1]] + list(points) + [points[-1] * 2 - points[-2]]
    out = [points[0].copy()]
    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        for s in range(1, samples + 1):
            t = s / samples
            t2, t3 = t * t, t * t * t
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
                              + (-p0 + 3 * p1 - 3 * p2 + p3) * t3))
    return out


# ----------------------------------------------------------------------------
# Grids

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
                row.append((1 - v) * bottom[i] + v * top[i] + (1 - u) * left[j] + u * right[j]
                           - ((1 - u) * (1 - v) * p00 + u * (1 - v) * p10 + (1 - u) * v * p01 + u * v * p11))
        grid.append(row)
    return grid


def _count(length, p, scale=1.0):
    if p.density_mode == 'LENGTH':
        return max(1, round(length / max(p.edge_length, 1e-9)))
    return max(1, round(p.resolution * scale))


def grids_from_sides(sides, fixed, p, conform):
    """Point grids (one per region). sides: n polylines corner k -> k+1 (world space)."""
    n = len(sides)
    lens = [poly_length(s) for s in sides]
    spacing = max(min(lens) / 32, 1e-6)
    work = []
    for k, s in enumerate(sides):
        if fixed[k]:
            work.append([q.copy() for q in s])
        else:
            dense = densify(s, conform, spacing)
            work.append(smooth(dense, p.side_smooth, conform) if p.side_smooth else dense)

    if n == 4 and p.layout in {'AUTO', 'GRID'}:
        a = (lens[0] + lens[2]) / 2
        b = (lens[1] + lens[3]) / 2
        longest = max(a, b, 1e-12)
        nu = _count(a, p, a / longest)
        nv = _count(b, p, b / longest)
        for pair in ((0, 2), (1, 3)):
            counts = [len(work[k]) - 1 for k in pair if fixed[k]]
            if counts:
                if pair == (0, 2):
                    nu = counts[0]
                else:
                    nv = counts[0]
        # a fixed side whose count can't match its opposite fixed side is resampled
        side_pts = []
        for k in range(4):
            cnt = nu if k in (0, 2) else nv
            side_pts.append(work[k] if fixed[k] and len(work[k]) - 1 == cnt else resample(work[k], cnt))
        bottom = side_pts[0]
        right = side_pts[1]
        top = list(reversed(side_pts[2]))     # corner 3 -> corner 2
        left = list(reversed(side_pts[3]))    # corner 0 -> corner 3
        return [coons(bottom, top, left, right)]

    avg = sum(lens) / n
    m = max(1, round(_count(avg, p) / 2)) if p.density_mode == 'LENGTH' else max(1, round(p.resolution / 2))
    even = [len(work[k]) - 1 for k in range(n) if fixed[k] and (len(work[k]) - 1) % 2 == 0 and len(work[k]) > 2]
    if even:
        m = even[0] // 2
    halves1, halves2, mids = [], [], []
    for k in range(n):
        pts = work[k] if fixed[k] and len(work[k]) - 1 == 2 * m else resample(work[k], 2 * m)
        mids.append(pts[m])
        halves1.append(pts[:m + 1])     # c_k -> m_k
        halves2.append(pts[m:])         # m_k -> c_k+1
    centre = conform(sum((s[0] for s in sides), Vector()) / n)
    spokes = [resample(densify([mids[k], centre], conform, spacing), m) for k in range(n)]   # m_k -> C
    grids = []
    for k in range(n):
        prev = (k - 1) % n
        grids.append(coons(halves1[k], spokes[prev], list(reversed(halves2[prev])), spokes[k]))
    return grids


# ----------------------------------------------------------------------------
# Building

def build(bm, sides, fixed, p, surf, mw_inv=None, weld_to=(), weld_dist=0.0):
    """Create the patch in `bm` (object-local via mw_inv). Returns the new faces."""
    conform = make_conform(surf, p)
    before = set(bm.faces)
    grids = grids_from_sides(sides, fixed, p, conform)
    to_local = (lambda q: mw_inv @ q) if mw_inv is not None else (lambda q: q)
    border = {id(q) for g in grids for q in (g[0] + g[-1] + [r[0] for r in g] + [r[-1] for r in g])}
    new_verts, new_faces = [], []
    for grid in grids:
        rows = []
        for row in grid:
            r = []
            for q in row:
                co = q if id(q) in border else conform(q)   # borders are already on the surface
                r.append(bm.verts.new(to_local(co)))
            rows.append(r)
        for r in rows:
            new_verts += r
        for j in range(len(rows) - 1):
            for i in range(len(rows[0]) - 1):
                new_faces.append(bm.faces.new((rows[j][i], rows[j][i + 1], rows[j + 1][i + 1], rows[j + 1][i])))
    size = max(max((q.length for s in sides for q in s), default=1.0), 1.0)
    bmesh.ops.remove_doubles(bm, verts=new_verts, dist=1e-6 * size)
    new_verts = [v for v in new_verts if v.is_valid]
    new_faces = [f for f in bm.faces if f not in before]     # merging rebuilds the seam faces
    bmesh.ops.recalc_face_normals(bm, faces=new_faces)
    if p.relax:
        _relax(new_verts, conform, p.relax, mw_inv)
    # Final projection: every resulting vertex onto the surface underneath (the drawing is only an
    # approximation - the grid can have more detail than the outline had), except vertices that
    # must stay exactly where they are: those on border-following sides and existing vertices.
    keep = [q for k, s in enumerate(sides) if fixed[k] for q in s]
    mw = mw_inv.inverted() if mw_inv is not None else None
    keep += [(mw @ v.co) if mw is not None else v.co.copy() for v in weld_to if v.is_valid]
    project_verts([v for v in new_verts if v.is_valid], conform, mw_inv, keep, max(weld_dist, 1e-6 * size))
    if weld_to and weld_dist > 0:
        targets = [v for v in weld_to if v.is_valid]
        bmesh.ops.remove_doubles(bm, verts=[v for v in new_verts if v.is_valid] + targets, dist=weld_dist)
        new_faces = [f for f in bm.faces if f not in before]
    _orient(bm, new_faces, surf, mw_inv)
    return new_faces


def project_verts(verts, conform, mw_inv, keep=(), keep_dist=1e-6):
    """Project vertices (object space, via mw_inv) onto the surface; vertices within keep_dist of
    a `keep` point (world) are left exactly where they are."""
    from mathutils.kdtree import KDTree
    mw = mw_inv.inverted() if mw_inv is not None else None
    tree = None
    if keep:
        tree = KDTree(len(keep))
        for i, q in enumerate(keep):
            tree.insert(q, i)
        tree.balance()
    for v in verts:
        w = mw @ v.co if mw is not None else v.co.copy()
        if tree is not None:
            _co, _i, dist = tree.find(w)
            if dist is not None and dist <= keep_dist:
                continue
        w = conform(w)
        v.co = mw_inv @ w if mw_inv is not None else w


def _relax(verts, conform, iterations, mw_inv):
    """Even out the interior (borders stay), re-conforming each pass."""
    mw = mw_inv.inverted() if mw_inv is not None else None
    interior = [v for v in verts if v.is_valid and not v.is_boundary]
    for _ in range(iterations):
        new = {}
        for v in interior:
            nb = [e.other_vert(v).co for e in v.link_edges]
            if nb:
                new[v] = sum(nb, Vector()) / len(nb)
        for v, co in new.items():
            w = conform(mw @ co if mw is not None else co)
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
