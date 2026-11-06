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
    resolution = 4
    density_mode = 'SEGMENTS'
    edge_length = 0.1
    relax = 3
    side_smooth = 0
    layout = 'AUTO'
    conform = 'SURFACE'
    strength = 1.0
    offset = 0.0
    projection = 'NORMAL'   # 'NORMAL': along the patch's normal direction, 'NEAREST': closest point
    direction = None        # patch normal (world), set by build / outline code
    subdiv = 0              # edge subdivision level: every edge split 2**level times
    subdiv_auto = False     # pick the lowest level whose edges stay within subdiv_tol of the surface
    subdiv_tol = 0.02       # as a fraction of the base (level 0) edge length
    max_subdiv = 4
    level = 0               # level actually used (set by build)
    gap = 0.0               # largest edge-to-surface gap of the result (set by build)

    @classmethod
    def from_props(cls, data):
        p = cls()
        for k in ("resolution", "density_mode", "edge_length", "relax", "side_smooth", "layout",
                  "conform", "strength", "offset", "projection", "subdiv", "subdiv_auto", "subdiv_tol"):
            if hasattr(data, k):
                setattr(p, k, getattr(data, k))
        return p


# ----------------------------------------------------------------------------
# Conforming to the surface

def patch_direction(surf, points):
    """Average surface normal under the given points (world) - the patch's facing direction."""
    n = Vector()
    for q in points:
        _loc, nrm = surf.nearest(q)
        if nrm is not None:
            n += nrm
    return n.normalized() if n.length > 1e-9 else None


def make_conform(surf, p):
    """Function world point -> conformed world point.
    Projection 'NORMAL' moves points straight along the patch direction onto the surface, so they
    don't slide sideways off bumps (closest-point projection would bunch them up in the valleys);
    it falls back to the closest point where that ray finds nothing near."""
    if surf is None or not surf or p.conform == 'NONE':
        return lambda co: co.copy()
    strength, offset = p.strength, p.offset
    d = p.direction if p.projection == 'NORMAL' else None

    def target(co):
        near, n_near = surf.nearest(co)
        if d is None:
            return near, n_near
        if offset and n_near is not None:
            # a point that is already floating at the offset casts from where it came from, so
            # projecting again (relax passes, final pass) doesn't drift sideways
            nn = n_near if n_near.dot(d) >= 0 else -n_near
            co = co - nn * offset
            near, n_near = surf.nearest(co)
        best = None
        for sign in (1.0, -1.0):
            loc, nrm = surf.ray_cast(co, d * sign)
            if loc is not None:
                dist = (loc - co).length
                if best is None or dist < best[0]:
                    best = (dist, loc, nrm)
        near_dist = (near - co).length
        # use the ray hit unless it lands much further away than the closest point (e.g. the ray
        # missed this part of the surface and hit something behind it)
        if best is not None and best[0] <= 4.0 * near_dist + 1e-6 * max(1.0, co.length) + 0.05 * best[0]:
            return best[1], best[2]
        return near, n_near

    def conform(co):
        loc, n = target(co)
        if n is not None and offset:
            if d is not None and n.dot(d) < 0:
                n = -n
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
    mult = 2 ** p.level
    if p.density_mode == 'LENGTH':
        return max(1, round(length / max(p.edge_length, 1e-9))) * mult
    return max(1, round(p.resolution * scale)) * mult


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
    if p.density_mode == 'LENGTH':
        m = max(1, round(avg / max(p.edge_length, 1e-9) / 2)) * 2 ** p.level
    else:
        m = max(1, round(p.resolution / 2)) * 2 ** p.level
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
    """Create the patch in `bm` (object-local via mw_inv). Returns the new faces.
    With p.subdiv_auto the edge subdivision level is raised until every edge stays within
    p.subdiv_tol (x base edge length) of the surface; p.level / p.gap report the result."""
    if p.projection == 'NORMAL' and p.direction is None and surf:
        p.direction = patch_direction(surf, [q for side in sides for q in side[:: max(1, len(side) // 8)]])
    auto = p.subdiv_auto and surf and p.conform == 'SURFACE'
    levels = range(0, p.max_subdiv + 1) if auto else [max(0, min(p.max_subdiv, p.subdiv))]
    base_edge = None
    faces = []
    for level in levels:
        p.level = level
        verts_before = set(bm.verts)
        faces = _build_once(bm, sides, fixed, p, surf, mw_inv, weld_to, weld_dist)
        p.gap, mean_edge = edge_gap(faces, surf, p.offset, mw_inv)
        if base_edge is None:
            base_edge = mean_edge * (2 ** level)
        if not auto or p.gap <= p.subdiv_tol * base_edge or level == levels[-1]:
            return faces
        # not close enough to the surface yet: remove this try and go one level finer
        new_verts = [v for f in faces if f.is_valid for v in f.verts if v not in verts_before]
        bmesh.ops.delete(bm, geom=[f for f in faces if f.is_valid], context='FACES_ONLY')
        bmesh.ops.delete(bm, geom=list({v for v in new_verts if v.is_valid and not v.link_faces}), context='VERTS')
        loose = [e for e in bm.edges if not e.link_faces and (e.verts[0] not in verts_before or e.verts[1] not in verts_before)]
        if loose:
            bmesh.ops.delete(bm, geom=loose, context='EDGES')
    return faces


def edge_gap(faces, surf, offset, mw_inv):
    """(largest distance between an edge's middle and the surface, mean edge length) - world space."""
    if not faces or not surf:
        return 0.0, 0.0
    mw = mw_inv.inverted() if mw_inv is not None else None
    edges = {e for f in faces if f.is_valid for e in f.edges}
    gap, total = 0.0, 0.0
    for e in edges:
        a, b = e.verts[0].co, e.verts[1].co
        if mw is not None:
            a, b = mw @ a, mw @ b
        total += (b - a).length
        mid = (a + b) / 2
        loc, _n = surf.nearest(mid)
        gap = max(gap, abs((mid - loc).length - abs(offset)))
    return gap, total / max(1, len(edges))


def _build_once(bm, sides, fixed, p, surf, mw_inv=None, weld_to=(), weld_dist=0.0):
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
