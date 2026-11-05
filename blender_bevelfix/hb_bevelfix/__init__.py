# SPDX-License-Identifier: GPL-2.0-or-later
"""
HB_BevelFix: fix bevels that overlap where the bevel width is larger than a
neighbouring edge, by edge-sliding only the affected vertices to smoothed offsets
(unlike Clamp Overlap, which shrinks the whole bevel).
"""

bl_info = {
    "name": "HB_BevelFix",
    "author": "Boti Harko",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "Edit Mode > Edge menu / right-click menu > Fix Bevel Overlap (Ctrl+Alt+B)",
    "description": "Fix overlapping bevel geometry by sliding only the affected vertices",
    "category": "Mesh",
}

import bmesh
import bpy
from bpy.props import BoolProperty, FloatProperty, IntProperty

from . import boolbevel, fix, loopselect, turncorner

_keymaps = []


class MESH_OT_hb_bevel_fix(bpy.types.Operator):
    """Fix overlaps in the selected bevel (the faces a bevel leaves selected):
vertices that slid past the end of their edge, or much further than their neighbours,
are edge-slid to a smoothed value; the rest of the bevel is left untouched"""
    bl_idname = "mesh.hb_bevel_fix"
    bl_label = "Fix Bevel Overlap"
    bl_options = {'REGISTER', 'UNDO'}

    clamp: FloatProperty(name="Max Slide", min=0.05, max=1.0, default=0.85, subtype='FACTOR',
                         description="Furthest a fixed vertex may slide, as a fraction of its edge")
    tolerance: FloatProperty(name="Tolerance", min=0.01, max=5.0, default=0.35, subtype='FACTOR',
                             description="How different a slide may be from its neighbours before it is fixed")
    blend: IntProperty(name="Blend", min=0, max=20, default=1,
                       description="Also smooth this many neighbouring vertices for a gradual transition")
    fix_all: BoolProperty(name="Smooth Whole Bevel", default=False,
                          description="Smooth the slide of every bevel vertex, not only the broken ones")
    width: FloatProperty(name="Width", default=0.0, subtype='DISTANCE', unit='LENGTH', step=0.1,
                         description="Widen (or narrow) the whole bevel by sliding along its edges "
                                     "(like MESHmachine's Change Width)")
    taper: BoolProperty(name="Taper", default=False,
                        description="Ramp the width change from nothing at one end of an open bevel to full at the other")
    taper_flip: BoolProperty(name="Flip", default=False, description="Taper from the other end")
    merge_threshold: FloatProperty(name="Merge Threshold", default=0.0, min=0.0, subtype='DISTANCE',
                                   unit='LENGTH', step=0.01,
                                   description="Collapse neighbouring bevel slices closer than this, removing "
                                               "sliver faces (like MESHmachine's Boolean Cleanup)")
    weld: BoolProperty(name="Weld Conflicts", default=False,
                       description="Aggressive: where an edge is too short for the bevel, merge the vertex at its "
                                   "far end into the bevel vertex instead of squeezing the bevel")

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        totals = dict(sections=0, affected=0, moved=0, welded=0, collapsed=0)
        for obj in context.objects_in_mode:
            if obj.type != 'MESH':
                continue
            bm = bmesh.from_edit_mesh(obj.data)
            bm.verts.index_update()
            st = fix.fix(bm, tolerance=self.tolerance, clamp=self.clamp, blend=self.blend,
                         fix_all=self.fix_all, weld=self.weld, width=self.width, taper=self.taper, taper_flip=self.taper_flip,
                         merge_threshold=self.merge_threshold)
            for k in totals:
                totals[k] += st[k]
            if st["moved"] or st["welded"] or st["collapsed"]:
                bm.normal_update()
                bmesh.update_edit_mesh(obj.data)
        if not totals["sections"]:
            self.report({'WARNING'}, "No bevel found: select the bevel's faces (a bevel leaves them selected)")
            return {'CANCELLED'}
        if not (totals["moved"] or totals["welded"] or totals["collapsed"]):
            self.report({'INFO'}, "No overlaps found in %d bevel slices" % totals["sections"])
            return {'FINISHED'}
        self.report({'INFO'}, "Fixed %(affected)d bevel sides: %(moved)d vertices moved, "
                              "%(welded)d welded, %(collapsed)d slices merged" % totals)
        return {'FINISHED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        col = layout.column()
        col.prop(self, "clamp")
        col.prop(self, "tolerance")
        col.prop(self, "blend")
        col.prop(self, "fix_all")
        col.separator()
        col.prop(self, "weld")
        col.prop(self, "merge_threshold")
        col.separator()
        col.prop(self, "width")
        row = col.row()
        row.active = abs(self.width) > 0
        row.prop(self, "taper")
        sub = row.row()
        sub.active = self.taper
        sub.prop(self, "taper_flip", toggle=True)


def menu_func(self, context):
    self.layout.operator(MESH_OT_hb_bevel_fix.bl_idname, icon='MOD_BEVEL')
    self.layout.operator(loopselect.MESH_OT_hb_select_loop.bl_idname, icon='EDGESEL')
    self.layout.operator(turncorner.MESH_OT_hb_turn_corner.bl_idname, icon='MOD_BEVEL')
    self.layout.operator(boolbevel.MESH_OT_hb_boolean_bevel.bl_idname, icon='MOD_BOOLEAN')


def select_menu_func(self, context):
    self.layout.operator(loopselect.MESH_OT_hb_select_loop.bl_idname, icon='EDGESEL')


def register():
    bpy.utils.register_class(MESH_OT_hb_bevel_fix)
    loopselect.register()
    turncorner.register()
    boolbevel.register()
    bpy.types.VIEW3D_MT_select_edit_mesh.append(select_menu_func)
    bpy.types.VIEW3D_MT_edit_mesh_edges.append(menu_func)
    bpy.types.VIEW3D_MT_edit_mesh_context_menu.append(menu_func)
    kc = bpy.context.window_manager.keyconfigs.addon
    if kc:
        km = kc.keymaps.new(name="Mesh", space_type='EMPTY')
        kmi = km.keymap_items.new(MESH_OT_hb_bevel_fix.bl_idname, 'B', 'PRESS', ctrl=True, alt=True)
        _keymaps.append((km, kmi))


def unregister():
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass
    _keymaps.clear()
    bpy.types.VIEW3D_MT_edit_mesh_context_menu.remove(menu_func)
    bpy.types.VIEW3D_MT_edit_mesh_edges.remove(menu_func)
    bpy.types.VIEW3D_MT_select_edit_mesh.remove(select_menu_func)
    boolbevel.unregister()
    turncorner.unregister()
    loopselect.unregister()
    bpy.utils.unregister_class(MESH_OT_hb_bevel_fix)
