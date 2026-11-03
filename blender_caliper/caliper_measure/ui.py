# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
from mathutils import Vector

from . import geom


class CALIPER_UL_measures(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "name", text="", emboss=False, icon='DRIVER_DISTANCE')
        row.label(text=geom.format_length(context, (Vector(item.end) - Vector(item.start)).length))
        row.prop(item, "visible", text="", emboss=False,
                 icon='HIDE_OFF' if item.visible else 'HIDE_ON')


class VIEW3D_PT_caliper(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Caliper"
    bl_label = "Caliper"

    def draw_header(self, context):
        self.layout.prop(context.scene.caliper, "show", text="")

    def draw(self, context):
        layout = self.layout
        s = context.scene.caliper

        col = layout.column(align=True)
        col.operator("wm.tool_set_by_id", text="Caliper Tool", icon='DRIVER_DISTANCE').name = "caliper.tool"
        col.operator("caliper.from_selection", icon='RESTRICT_SELECT_OFF')

        row = layout.row()
        row.template_list("CALIPER_UL_measures", "", s, "measures", s, "active_index", rows=3)
        side = row.column(align=True)
        side.operator("caliper.remove", text="", icon='REMOVE').index = -1
        side.separator()
        side.operator("caliper.swap", text="", icon='ARROW_LEFTRIGHT')
        side.operator("caliper.clear", text="", icon='TRASH')

        if 0 <= s.active_index < len(s.measures):
            m = s.measures[s.active_index]
            d = Vector(m.end) - Vector(m.start)
            box = layout.box()
            box.label(text="Length: " + geom.format_length(context, d.length))
            r = box.row(align=True)
            for ax, v in zip("XYZ", d):
                r.label(text="%s %s" % (ax, geom.format_length(context, abs(v))))
            c = box.column(align=True)
            c.prop(m, "start")
            c = box.column(align=True)
            c.prop(m, "end")


class VIEW3D_PT_caliper_display(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Caliper"
    bl_label = "Display"
    bl_parent_id = "VIEW3D_PT_caliper"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        s = context.scene.caliper
        layout.prop(s, "unit")
        layout.prop(s, "precision")
        layout.prop(s, "color")
        layout.prop(s, "show_ticks")
        layout.prop(s, "show_deltas")
        layout.prop(s, "snap_radius")


classes = (CALIPER_UL_measures, VIEW3D_PT_caliper, VIEW3D_PT_caliper_display)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
