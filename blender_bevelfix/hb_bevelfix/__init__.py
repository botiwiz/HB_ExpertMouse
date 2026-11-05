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

from . import fix, loopselect

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
    weld: BoolProperty(name="Weld Conflicts", default=False,
                       description="Aggressive: where an edge is too short for the bevel, merge the vertex at its "
                                   "far end into the bevel vertex instead of squeezing the bevel")

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        total_sec = total_aff = total_moved = total_weld = 0
        for obj in context.objects_in_mode:
            if obj.type != 'MESH':
                continue
            bm = bmesh.from_edit_mesh(obj.data)
            bm.verts.index_update()
            n_sec, n_aff, n_moved, n_weld = fix.fix(bm, tolerance=self.tolerance, clamp=self.clamp,
                                                   blend=self.blend, fix_all=self.fix_all, weld=self.weld)
            total_weld += n_weld
            total_sec += n_sec
            total_aff += n_aff
            total_moved += n_moved
            if n_moved or n_weld:
                bm.normal_update()
                bmesh.update_edit_mesh(obj.data)
        if not total_sec:
            self.report({'WARNING'}, "No bevel found: select the bevel's faces (a bevel leaves them selected)")
            return {'CANCELLED'}
        if not total_moved and not total_weld:
            self.report({'INFO'}, "No overlaps found in %d bevel sections" % total_sec)
            return {'FINISHED'}
        self.report({'INFO'}, "Fixed %d bevel sides (%d vertices moved, %d welded)"
                    % (total_aff, total_moved, total_weld))
        return {'FINISHED'}


def menu_func(self, context):
    self.layout.operator(MESH_OT_hb_bevel_fix.bl_idname, icon='MOD_BEVEL')
    self.layout.operator(loopselect.MESH_OT_hb_select_loop.bl_idname, icon='EDGESEL')


def select_menu_func(self, context):
    self.layout.operator(loopselect.MESH_OT_hb_select_loop.bl_idname, icon='EDGESEL')


def register():
    bpy.utils.register_class(MESH_OT_hb_bevel_fix)
    loopselect.register()
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
    loopselect.unregister()
    bpy.utils.unregister_class(MESH_OT_hb_bevel_fix)
