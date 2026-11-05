# SPDX-License-Identifier: GPL-2.0-or-later
import bpy


class VIEW3D_PT_hb_workplane(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "HB_Workplane"
    bl_label = "HB_Workplane"

    def draw_header(self, context):
        self.layout.prop(context.scene.hb_workplane, "enabled", text="")

    def draw(self, context):
        layout = self.layout
        s = context.scene.hb_workplane
        layout.active = s.enabled

        box = layout.box()
        row = box.row()
        row.label(text="Custom plane" if s.mode == 'CUSTOM' else "Automatic (view)",
                  icon='MESH_GRID' if s.mode == 'CUSTOM' else 'VIEW3D')
        col = box.column(align=True)
        op = col.operator("hb_workplane.set", text="Set Workplane", icon='SNAP_FACE')
        op.source = 'AUTO'
        op.toggle = False
        op = col.operator("hb_workplane.set", text="From 3D Cursor", icon='PIVOT_CURSOR')
        op.source = 'CURSOR'
        op.toggle = False
        row = col.row(align=True)
        row.operator("hb_workplane.clear", text="Clear", icon='X')
        row.operator("hb_workplane.cursor_to_workplane", text="Cursor", icon='CURSOR')
        if s.mode == 'AUTO':
            box.prop(s, "axis_bias", slider=True)

        col = layout.column(heading="Constrain")
        row = col.row(align=True)
        row.prop(s, "constrain_move", toggle=True, icon='EMPTY_ARROWS')
        row.prop(s, "constrain_rotate", toggle=True, icon='DRIVER_ROTATIONAL_DIFFERENCE')
        layout.prop(s, "sync_orientation")


class VIEW3D_PT_hb_workplane_grid(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "HB_Workplane"
    bl_label = "Grid"
    bl_parent_id = "VIEW3D_PT_hb_workplane"

    def draw_header(self, context):
        self.layout.prop(context.scene.hb_workplane, "show_grid", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        s = context.scene.hb_workplane
        layout.active = s.show_grid
        layout.prop(s, "show_auto_grid")
        layout.prop(s, "grid_opacity", slider=True)
        layout.prop(s, "grid_lines")
        layout.prop(s, "grid_color")
        layout.prop(s, "occlude")


classes = (VIEW3D_PT_hb_workplane, VIEW3D_PT_hb_workplane_grid)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
