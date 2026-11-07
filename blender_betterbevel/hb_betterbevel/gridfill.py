# SPDX-License-Identifier: GPL-2.0-or-later
"""Replace leftover patches (corner fans, slivers) with Grid Fill."""

from collections import defaultdict

import bmesh
import bpy

from . import nodes


def _loops(bm):
    """Closed boundary loops as lists of vertices (loops through branching vertices are skipped)."""
    adj = defaultdict(list)
    for e in bm.edges:
        if e.is_boundary:
            for v in e.verts:
                adj[v].append(e)
    seen, out = set(), []
    for e0 in [e for e in bm.edges if e.is_boundary]:
        if e0 in seen:
            continue
        seen.add(e0)
        start, v = e0.verts
        vs, ok = [start], True
        while v is not start:
            if len(adj[v]) != 2:
                ok = False
                break
            vs.append(v)
            nxt = [e for e in adj[v] if e not in seen]
            if not nxt:
                ok = False
                break
            seen.add(nxt[0])
            v = nxt[0].other_vert(v)
        if ok and len(vs) >= 3:
            out.append(vs)
    return out


def _key(vs):
    return frozenset(tuple(round(c, 5) for c in v.co) for v in vs)


def _select_only(bm, vs):
    for f in bm.faces:
        f.select = False
    for e in bm.edges:
        e.select = False
    for v in bm.verts:
        v.select = False
    vset = set(vs)
    for v in vs:
        v.select = True
        for e in v.link_edges:
            if e.is_boundary and e.other_vert(v) in vset:
                e.select = True


def _fill_one(me, idx):
    """Grid fill one loop (made even first). Falls back to a beauty fill. Returns the method used."""
    bm = bmesh.from_edit_mesh(me)
    bm.verts.ensure_lookup_table()
    vs = [bm.verts[i] for i in idx]
    if len(vs) % 2:
        # split the longest loop edge to get the even count Grid Fill needs
        # (bmesh.utils, not bmesh.ops: ops reallocate the edit mesh and kill our references)
        n = len(vs)
        i = max(range(n), key=lambda k: (vs[k].co - vs[(k + 1) % n].co).length)
        a, b = vs[i], vs[(i + 1) % n]
        edge = next(e for e in a.link_edges if e.other_vert(a) is b)
        _ne, nv = bmesh.utils.edge_split(edge, a, 0.5)
        vs.insert(i + 1, nv)
    _select_only(bm, vs)
    bmesh.update_edit_mesh(me, loop_triangles=False, destructive=True)
    before = len(bm.faces)
    for op, kw, name in ((bpy.ops.mesh.fill_grid, {"use_interp_simple": False}, "grid"),
                         (bpy.ops.mesh.fill, {"use_beauty": True}, "fill")):
        try:
            op(**kw)
        except RuntimeError:
            continue
        bm = bmesh.from_edit_mesh(me)
        if len(bm.faces) > before:
            return name
    return None


def _finish(me):
    bm = bmesh.from_edit_mesh(me)
    lay = bm.faces.layers.bool.get(nodes.FACE_ATTR) if hasattr(bm.faces.layers, "bool") else None
    for f in bm.faces:
        if f.select:
            f.smooth = True
            if lay is not None:
                f[lay] = True
    bmesh.update_edit_mesh(me)


def fill_loops(me, wanted):
    """Grid fill every boundary loop for which wanted(loop_verts) is true. Returns (grid, other, failed)."""
    done, counts = set(), {"grid": 0, "fill": 0, None: 0}
    while True:
        bm = bmesh.from_edit_mesh(me)
        todo = [vs for vs in _loops(bm) if _key(vs) not in done and wanted(vs)]
        if not todo:
            break
        vs = todo[0]
        done.add(_key(vs))
        bm.verts.index_update()
        how = _fill_one(me, [v.index for v in vs])
        counts[how] += 1
        if how:
            _finish(me)
    # whatever is still open (loops through branching vertices): plain hole fill
    bm = bmesh.from_edit_mesh(me)
    rest = [e for e in bm.edges if e.is_boundary and wanted(list(e.verts))]
    if rest:
        for el in (bm.faces, bm.edges, bm.verts):
            for x in el:
                x.select = False
        for e in rest:
            e.select = True
            for v in e.verts:
                v.select = True
        bmesh.update_edit_mesh(me)
        n0 = len(bm.faces)
        try:
            bpy.ops.mesh.fill_holes(sides=0)
        except RuntimeError:
            pass
        bm = bmesh.from_edit_mesh(me)
        if len(bm.faces) > n0:
            counts["fill"] += 1
            _finish(me)
    return counts["grid"], counts["fill"], counts[None]


class MESH_OT_hb_better_bevel_gridfill(bpy.types.Operator):
    """Replace the selected faces (a leftover patch) with a Grid Fill; or Grid Fill the selected open loops"""
    bl_idname = "mesh.hb_better_bevel_gridfill"
    bl_label = "Grid Fill Patch"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        g = f = bad = 0
        for obj in [o for o in context.objects_in_mode if o.type == 'MESH']:
            me = obj.data
            bm = bmesh.from_edit_mesh(me)
            faces = [x for x in bm.faces if x.select]
            if faces:
                # the patch's outline survives; everything inside goes
                rim = {v for x in faces for v in x.verts
                       if any(not lf.select for lf in v.link_faces) or any(e.is_boundary for e in v.link_edges)}
                bmesh.ops.delete(bm, geom=faces, context='FACES')
                bmesh.update_edit_mesh(me)
                keys = {tuple(round(c, 5) for c in v.co) for v in rim if v.is_valid}
            else:
                keys = {tuple(round(c, 5) for c in v.co) for v in bm.verts if v.select}
            if not keys:
                continue

            def wanted(vs, keys=keys):
                return all(tuple(round(c, 5) for c in v.co) in keys for v in vs)

            a, b, c = fill_loops(me, wanted)
            g, f, bad = g + a, f + b, bad + c
        if not (g or f or bad):
            self.report({'WARNING'}, "Select the faces of a patch (or an open edge loop) to refill")
            return {'CANCELLED'}
        msg = "Grid filled %d" % g + (", %d triangle-filled (not griddable)" % f if f else "")
        self.report({'WARNING'} if bad else {'INFO'}, msg + (", %d failed" % bad if bad else ""))
        return {'FINISHED'}


class OBJECT_OT_hb_better_bevel_apply_fill(bpy.types.Operator):
    """Apply the Better Bevel modifier and Grid Fill its corner patches and leftover holes (destructive)"""
    bl_idname = "object.hb_better_bevel_apply_fill"
    bl_label = "Apply Better Bevel & Grid Fill"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        from . import find_mod
        objs = [o for o in context.selected_objects if find_mod(o) is not None]
        if not objs and find_mod(context.object) is not None:
            objs = [context.object]
        if not objs:
            self.report({'WARNING'}, "No selected object has a Better Bevel")
            return {'CANCELLED'}
        g = f = bad = 0
        for obj in objs:
            for o in context.selected_objects:
                o.select_set(o == obj)
            context.view_layer.objects.active = obj
            mod = find_mod(obj)
            with context.temp_override(object=obj, active_object=obj):
                bpy.ops.object.modifier_apply(modifier=mod.name)
            bpy.ops.object.mode_set(mode='EDIT')
            me = obj.data
            bm = bmesh.from_edit_mesh(me)
            fl = bm.faces.layers.bool.get(nodes.FILL_ATTR)
            bl = bm.faces.layers.bool.get(nodes.FACE_ATTR)
            if fl is not None:
                bmesh.ops.delete(bm, geom=[x for x in bm.faces if x[fl]], context='FACES')
                bmesh.update_edit_mesh(me)

            def wanted(vs, bl=bl):  # only holes along the bevel
                return bl is not None and any(lf[bl] for v in vs for lf in v.link_faces)

            a, b, c = fill_loops(me, wanted)
            g, f, bad = g + a, f + b, bad + c
            bm = bmesh.from_edit_mesh(me)
            for x in bm.faces:
                x.select = False
            bmesh.update_edit_mesh(me)
            bpy.ops.object.mode_set(mode='OBJECT')
            if nodes.FILL_ATTR in me.attributes:
                me.attributes.remove(me.attributes[nodes.FILL_ATTR])
        for o in objs:
            o.select_set(True)
        msg = "Applied; grid filled %d" % g + (", %d triangle-filled" % f if f else "")
        self.report({'WARNING'} if bad else {'INFO'}, msg + (", %d left open" % bad if bad else ""))
        return {'FINISHED'}


classes = (MESH_OT_hb_better_bevel_gridfill, OBJECT_OT_hb_better_bevel_apply_fill)
