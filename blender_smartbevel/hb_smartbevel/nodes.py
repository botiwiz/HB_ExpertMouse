# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the "HB Smart Bevel" Geometry Nodes group.

Pipeline
  1. pick edges (angle / marked sharp / marked with the tool)
  2. turn them into curves, resample them evenly
  3. sweep an even-thickness tube along them and boolean it out of the mesh
  4. delete the tube walls -> two clean cut lines per edge
  5. bridge the cut lines with a strip whose cross-section is a rational
     quadratic Bezier (Profile 0.5 = true circular arc, 0 = chamfer)
  6. weld the strip to the cut, shade it smooth
"""

import math

import bpy

GROUP_NAME = "HB Smart Bevel"
VERSION = 1  # bump to force a rebuild of existing groups
P = "hb_sb_"  # prefix of the temporary attributes

# (name, socket type, default, min, max, subtype, description)
INPUTS = [
    ("Width", "NodeSocketFloat", 0.1, 0.0, 1e4, "DISTANCE", "Distance from the edge to where the bevel starts on each face"),
    ("Segments", "NodeSocketInt", 4, 1, 64, "NONE", "Number of segments across the bevel"),
    ("Profile", "NodeSocketFloat", 0.5, 0.0, 1.0, "FACTOR", "0 = flat chamfer, 0.5 = round, 1 = sharp"),
    ("Use Angle", "NodeSocketBool", True, None, None, "NONE", "Bevel edges sharper than Angle"),
    ("Angle", "NodeSocketFloat", math.radians(30.0), 0.0, math.pi, "ANGLE", "Minimum angle between faces"),
    ("Use Sharp", "NodeSocketBool", False, None, None, "NONE", "Bevel edges marked Sharp"),
    ("Use Marked", "NodeSocketBool", True, None, None, "NONE", "Bevel edges marked with the Smart Bevel tool"),
    ("Spacing", "NodeSocketFloat", 0.05, 0.0, 1e4, "DISTANCE", "Length of the bevel rows along the edge (0 = keep the edge's own vertices)"),
    ("Tube Resolution", "NodeSocketInt", 16, 3, 256, "NONE", "Sides of the cutting tube"),
    ("Snap", "NodeSocketBool", True, None, None, "NONE", "Snap the bevel's borders onto the boolean cut"),
    ("Weld", "NodeSocketBool", True, None, None, "NONE", "Merge the bevel with the cut mesh"),
]
MARK_ATTR = "hb_smart_bevel"   # edge attribute written by the tool
FACE_ATTR = "smart_bevel"      # face attribute marking the bevel faces (for shaders)


def socket_ids(group):
    """Interface name -> modifier key (identifier)."""
    return {it.name: it.identifier for it in group.interface.items_tree
            if getattr(it, "item_type", "SOCKET") == "SOCKET" and it.in_out == "INPUT"}


class _B:
    """Tiny helper to write node graphs as expressions."""

    def __init__(self, tree):
        self.t = tree
        self.n = 0

    def node(self, kind, inputs=None, **props):
        nd = self.t.nodes.new(kind)
        nd.location = ((self.n % 12) * 220, -(self.n // 12) * 260)
        self.n += 1
        for k, v in props.items():
            if hasattr(nd, k):
                setattr(nd, k, v)
            else:  # 4.x property became a menu socket in 5.x (or vice versa)
                self.menu(nd, k, v)
        for k, v in (inputs or {}).items():
            self.set(nd.inputs[k] if isinstance(k, int) else self._in(nd, k), v)
        return nd

    @staticmethod
    def _in(nd, name):
        for s in nd.inputs:
            if s.name == name and s.enabled:
                return s
        return nd.inputs[name]

    @staticmethod
    def menu(nd, key, value):
        """Set an enum that is a property in some versions and a menu socket in others."""
        label = key.replace("_", " ").title()
        sock = nd.inputs.get(label)
        if sock is None:
            raise AttributeError(f"{nd.bl_idname}: no {key}")
        try:
            sock.default_value = value
        except TypeError:  # menu items are named "Length", not "LENGTH"
            sock.default_value = value.replace("_", " ").title()

    def set(self, sock, v):
        if isinstance(v, bpy.types.NodeSocket):
            self.t.links.new(v, sock)
        elif isinstance(v, bpy.types.Node):
            self.t.links.new(v.outputs[0], sock)
        else:
            sock.default_value = v

    # -- math ------------------------------------------------------------
    def f(self, op, a, b=0.0, c=0.0):
        return self.node("ShaderNodeMath", {0: a, 1: b, 2: c}, operation=op).outputs[0]

    def v(self, op, a, b=(0, 0, 0), s=None):
        nd = self.node("ShaderNodeVectorMath", {0: a, 1: b}, operation=op)
        if s is not None:
            self.set(nd.inputs[3], s)
        return nd.outputs[1] if op in {"DOT_PRODUCT", "LENGTH", "DISTANCE"} else nd.outputs[0]

    def scale(self, a, s):
        return self.v("SCALE", a, s=s)

    def add(self, *vs):
        out = vs[0]
        for x in vs[1:]:
            out = self.v("ADD", out, x)
        return out

    def cmp(self, op, a, b):
        return self.node("FunctionNodeCompare", {0: a, 1: b}, data_type="FLOAT", operation=op).outputs[0]

    def bool(self, op, a, b=False):
        ins = {0: a} if op == "NOT" else {0: a, 1: b}
        return self.node("FunctionNodeBooleanMath", ins, operation=op).outputs[0]

    def switch(self, kind, cond, false, true):
        nd = self.node("GeometryNodeSwitch", input_type=kind)
        self.set(nd.inputs[0], cond)
        self.set(nd.inputs[1], false)
        self.set(nd.inputs[2], true)
        return nd.outputs[0]

    # -- attributes ----------------------------------------------------
    def store(self, geo, name, value, dtype="FLOAT", domain="POINT", sel=True):
        nd = self.node("GeometryNodeStoreNamedAttribute", data_type=dtype, domain=domain)
        self.set(nd.inputs["Geometry"], geo)
        self.set(nd.inputs["Selection"], sel)
        self.set(nd.inputs["Name"], name)
        self.set(self._in(nd, "Value"), value)
        return nd.outputs[0]

    def attr(self, name, dtype="FLOAT"):
        return self.node("GeometryNodeInputNamedAttribute", {"Name": name}, data_type=dtype).outputs[0]

    def sample(self, geo, value, index, dtype="FLOAT", domain="EDGE"):
        nd = self.node("GeometryNodeSampleIndex", data_type=dtype, domain=domain)
        self.set(nd.inputs["Geometry"], geo)
        self.set(self._in(nd, "Value"), value)
        self.set(nd.inputs["Index"], index)
        return nd.outputs[0]


def _build(tree):
    b = _B(tree)
    gin = b.node("NodeGroupInput")
    gout = b.node("NodeGroupOutput")
    I = {s.name: s for s in gin.outputs}

    width, segs, profile = I["Width"], I["Segments"], I["Profile"]
    res = I["Tube Resolution"]

    # ---- 1. edge selection ----------------------------------------------
    eang = b.node("GeometryNodeInputMeshEdgeAngle")
    by_angle = b.bool("AND", I["Use Angle"], b.cmp("GREATER_THAN", eang.outputs[0], I["Angle"]))
    by_sharp = b.bool("AND", I["Use Sharp"], b.attr("sharp_edge", "BOOLEAN"))
    by_mark = b.bool("AND", I["Use Marked"], b.attr(MARK_ATTR, "BOOLEAN"))
    sel = b.bool("OR", b.bool("OR", by_angle, by_sharp), by_mark)
    nb = b.node("GeometryNodeInputMeshEdgeNeighbors")
    two = b.cmp("EQUAL", nb.outputs[0], 2.0)
    sel = b.bool("AND", sel, two)

    geo = I["Geometry"]
    # edge data: averaged face normal, signed angle (+ convex / - concave)
    nrm = b.node("GeometryNodeInputNormal").outputs[0]
    geo = b.store(geo, P + "n", nrm, "FLOAT_VECTOR", "EDGE")
    geo = b.store(geo, P + "a", eang.outputs[1], "FLOAT", "EDGE")
    geo = b.store(geo, P + "sel", sel, "BOOLEAN", "EDGE")
    src = geo

    edges = b.node("GeometryNodeSeparateGeometry", {"Geometry": src, "Selection": b.attr(P + "sel", "BOOLEAN")},
                   domain="EDGE").outputs[0]

    # ---- 2. curves ------------------------------------------------------
    curve = b.node("GeometryNodeMeshToCurve", {"Mesh": edges}).outputs[0]
    rs = b.node("GeometryNodeResampleCurve", {"Curve": curve, "Length": width}, mode="LENGTH")
    b.set(b._in(rs, "Length"), I["Spacing"])
    curve = b.switch("GEOMETRY", b.cmp("GREATER_THAN", I["Spacing"], 0.0), curve, rs.outputs[0])

    # per point frame
    pos = b.node("GeometryNodeInputPosition").outputs[0]
    T = b.v("NORMALIZE", b.node("GeometryNodeInputTangent").outputs[0])
    eid = b.node("GeometryNodeSampleNearest", {"Geometry": edges, "Sample Position": pos}, domain="EDGE").outputs[0]
    n = b.sample(edges, b.attr(P + "n", "FLOAT_VECTOR"), eid, "FLOAT_VECTOR")
    a = b.sample(edges, b.attr(P + "a"), eid, "FLOAT")
    # keep n perpendicular to the tangent
    n = b.v("NORMALIZE", b.v("SUBTRACT", n, b.scale(T, b.v("DOT_PRODUCT", n, T))))
    theta = b.f("ABSOLUTE", a)
    half = b.f("MULTIPLY", theta, 0.5)
    n1 = b.node("ShaderNodeVectorRotate", {"Vector": n, "Axis": T, "Angle": half}, rotation_type="AXIS_ANGLE").outputs[0]
    n2 = b.node("ShaderNodeVectorRotate", {"Vector": n, "Axis": T, "Angle": b.f("MULTIPLY", half, -1.0)},
                rotation_type="AXIS_ANGLE").outputs[0]
    # k = -1 on convex edges (into face 1 goes below face 2), +1 on concave
    k = b.f("SUBTRACT", 1.0, b.f("MULTIPLY", b.f("GREATER_THAN", a, 0.0), 2.0))

    def into_face(ni, nj):
        s = b.v("NORMALIZE", b.v("CROSS_PRODUCT", T, ni))
        sg = b.f("MULTIPLY", b.f("SIGN", b.v("DOT_PRODUCT", s, nj)), k)
        return b.scale(s, sg)

    t1, t2 = into_face(n1, n2), into_face(n2, n1)
    A0 = b.add(pos, b.scale(t1, width))
    B0 = b.add(pos, b.scale(t2, width))

    # ---- 3. tube + boolean -----------------------------------------------
    tilt = b.f("DIVIDE", math.pi, res)
    tcurve = b.node("GeometryNodeSetCurveTilt", {"Curve": curve, "Tilt": tilt}).outputs[0]
    # cut lies in the middle of a tube side: push the polygon out so it lands at Width
    rad = b.f("DIVIDE", width, b.f("COSINE", tilt))
    circ = b.node("GeometryNodeCurvePrimitiveCircle", {"Resolution": res, "Radius": rad}, mode="RADIUS").outputs[0]
    tube = b.node("GeometryNodeCurveToMesh", {"Curve": tcurve, "Profile Curve": circ, "Fill Caps": True}).outputs[0]
    tube = b.store(tube, P + "tube", True, "BOOLEAN", "FACE")
    boo = b.node("GeometryNodeMeshBoolean", {"Mesh 1": src, "Mesh 2": tube, "Self Intersection": True},
                 operation="DIFFERENCE", solver="EXACT")
    cut = b.store(boo.outputs[0], P + "cut", boo.outputs[1], "BOOLEAN", "EDGE")
    cutv = b.store(cut, P + "cutv", b.attr(P + "cut", "BOOLEAN"), "BOOLEAN", "POINT")
    # ---- 4. open the cut ------------------------------------------------
    opened = b.node("GeometryNodeDeleteGeometry", {"Geometry": cutv, "Selection": b.attr(P + "tube", "BOOLEAN")},
                    domain="FACE", mode="ONLY_FACE").outputs[0]
    opened = b.node("GeometryNodeDeleteGeometry", {"Geometry": opened, "Selection": b.bool(
        "NOT", b.cmp("GREATER_THAN", b.node("GeometryNodeInputMeshEdgeNeighbors").outputs[0], 0.0))},
        domain="EDGE", mode="ALL").outputs[0]
    cut_edges = b.node("GeometryNodeSeparateGeometry", {"Geometry": opened, "Selection": b.attr(P + "cut", "BOOLEAN")},
                       domain="EDGE").outputs[0]

    def snap(p):
        prox = b.node("GeometryNodeProximity", target_element="EDGES")
        b.set(prox.inputs[0], cut_edges)
        b.set(b._in(prox, "Sample Position"), p)
        hit = b.bool("AND", I["Snap"], prox.outputs["Is Valid"]) if "Is Valid" in prox.outputs else I["Snap"]
        return b.switch("VECTOR", hit, p, prox.outputs[0])

    A, B = snap(A0), snap(B0)
    M = b.scale(b.add(A, B), 0.5)
    Cs = b.add(M, b.scale(b.v("SUBTRACT", pos, M), b.f("MULTIPLY", profile, 2.0)))
    w = b.f("COSINE", half)

    scurve = curve
    for nm, val, dt in (("A", A, "FLOAT_VECTOR"), ("B", B, "FLOAT_VECTOR"), ("C", Cs, "FLOAT_VECTOR"),
                        ("w", w, "FLOAT"), ("nn", n, "FLOAT_VECTOR")):
        scurve = b.store(scurve, P + nm, val, dt, "POINT")

    # ---- 5. bridge strip ---------------------------------------------------
    line = b.node("GeometryNodeCurvePrimitiveLine", {"Start": (0, 0, 0), "End": (1, 0, 0)}).outputs[0]
    line = b.node("GeometryNodeResampleCurve", {"Curve": line}, mode="COUNT")
    b.set(b._in(line, "Count"), b.f("ADD", segs, 1.0))
    prof = b.store(line.outputs[0], P + "u", b.node("GeometryNodeSplineParameter").outputs[0], "FLOAT", "POINT")
    strip = b.node("GeometryNodeCurveToMesh", {"Curve": scurve, "Profile Curve": prof}).outputs[0]

    u = b.attr(P + "u")
    iu = b.f("SUBTRACT", 1.0, u)
    wa = b.f("MULTIPLY", iu, iu)
    wb = b.f("MULTIPLY", u, u)
    wc = b.f("MULTIPLY", b.f("MULTIPLY", b.f("MULTIPLY", u, iu), 2.0), b.attr(P + "w"))
    num = b.add(b.scale(b.attr(P + "A", "FLOAT_VECTOR"), wa), b.scale(b.attr(P + "C", "FLOAT_VECTOR"), wc),
                b.scale(b.attr(P + "B", "FLOAT_VECTOR"), wb))
    den = b.f("ADD", b.f("ADD", wa, wb), wc)
    strip = b.node("GeometryNodeSetPosition", {"Geometry": strip, "Position": b.scale(num, b.f("DIVIDE", 1.0, den))}).outputs[0]
    # face the strip outwards (along the averaged normal of the edge)
    fn = b.node("GeometryNodeInputNormal").outputs[0]
    nf = b.node("GeometryNodeFieldOnDomain", {0: b.attr(P + "nn", "FLOAT_VECTOR")}, domain="FACE", data_type="FLOAT_VECTOR").outputs[0]
    flip = b.cmp("LESS_THAN", b.v("DOT_PRODUCT", fn, nf), 0.0)
    strip = b.node("GeometryNodeFlipFaces", {"Mesh": strip, "Selection": flip}).outputs[0]
    border = b.bool("OR", b.cmp("LESS_THAN", u, 1e-4), b.cmp("GREATER_THAN", u, 1.0 - 1e-4))
    strip = b.store(strip, P + "edge", border, "BOOLEAN", "POINT")
    strip = b.store(strip, FACE_ATTR, True, "BOOLEAN", "FACE")
    strip = b.node("GeometryNodeSetShadeSmooth", {"Geometry": strip}, domain="FACE").outputs[0]

    # ---- 6. join + weld -------------------------------------------------------
    join = b.node("GeometryNodeJoinGeometry")
    b.set(join.inputs[0], strip)
    b.set(join.inputs[0], opened)
    out = join.outputs[0]
    wsel = b.bool("OR", b.attr(P + "edge", "BOOLEAN"), b.attr(P + "cutv", "BOOLEAN"))
    wdist = b.f("MULTIPLY", width, 0.05)
    merged = b.node("GeometryNodeMergeByDistance", {"Geometry": out, "Selection": wsel, "Distance": wdist}).outputs[0]
    out = b.switch("GEOMETRY", I["Weld"], out, merged)

    rm = b.node("GeometryNodeRemoveAttribute", {"Geometry": out, "Name": P + "*"})
    if rm.inputs.get("Pattern Mode") is not None:
        rm.inputs["Pattern Mode"].default_value = "Wildcard"
        out = rm.outputs[0]
    else:  # older versions: remove one by one
        tree.nodes.remove(rm)
        for nm in ("n", "a", "sel", "tube", "cut", "cutv", "A", "B", "C", "w", "nn", "u", "edge"):
            out = b.node("GeometryNodeRemoveAttribute", {"Geometry": out, "Name": P + nm}).outputs[0]
    b.set(gout.inputs[0], out)


def ensure_group():
    g = bpy.data.node_groups.get(GROUP_NAME)
    if g is not None and g.get("hb_version") == VERSION:
        return g
    if g is None:
        g = bpy.data.node_groups.new(GROUP_NAME, "GeometryNodeTree")
    else:
        g.nodes.clear()
        g.interface.clear()
    iface = g.interface
    iface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    iface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    for name, kind, dflt, lo, hi, sub, desc in INPUTS:
        s = iface.new_socket(name, in_out="INPUT", socket_type=kind)
        s.description = desc
        s.default_value = dflt
        if lo is not None:
            s.min_value, s.max_value = lo, hi
        if sub != "NONE" and hasattr(s, "subtype"):
            try:
                s.subtype = sub
            except (TypeError, AttributeError):
                pass
    if hasattr(g, "is_modifier"):
        g.is_modifier = True
    _build(g)
    g["hb_version"] = VERSION
    return g
