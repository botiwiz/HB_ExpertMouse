# SPDX-License-Identifier: GPL-2.0-or-later
"""HB_BetterBevel: non-destructive tube-boolean bevel as a tool + Geometry Nodes modifier."""

bl_info = {
    "name": "HB_BetterBevel",
    "author": "Boti Harko",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Toolbar > Better Bevel",
    "description": "Bevel by booleaning even-thickness tubes out of sharp edges and bridging the cut "
                   "with an adjustable profile; stays live as a Geometry Nodes modifier",
    "category": "Mesh",
}

import math

import bmesh
import bpy
from bpy.props import BoolProperty, FloatProperty, IntProperty, PointerProperty
from bpy_extras import view3d_utils

from . import gridfill, nodes

MOD_NAME = "HB Better Bevel"

# tool setting -> modifier input
SOCKETS = {
    "width": "Width", "segments": "Segments", "profile": "Profile",
    "use_angle": "Use Angle", "angle": "Angle", "use_sharp": "Use Sharp", "use_marked": "Use Marked",
    "spacing": "Spacing", "tube_resolution": "Tube Resolution",
    "snap": "Snap", "weld": "Weld", "fill_corners": "Fill Corners",
}


def find_mod(obj):
    if obj is None or obj.type != 'MESH':
        return None
    for m in obj.modifiers:
        if m.type == 'NODES' and m.node_group and m.node_group.name.startswith(nodes.GROUP_NAME):
            return m
    return None


def push(settings, mod):
    for prop, sock in SOCKETS.items():
        try:
            nodes.set_input(mod, sock, getattr(settings, prop))
        except (KeyError, AttributeError, TypeError):
            pass
    mod.id_data.update_tag()


def pull(settings, mod):
    settings.syncing = True
    try:
        for prop, sock in SOCKETS.items():
            try:
                setattr(settings, prop, nodes.get_input(mod, sock))
            except (KeyError, AttributeError, TypeError):
                pass
    finally:
        settings.syncing = False


def _live(self, context):
    if not isinstance(self, HBBetterBevelSettings) or self.syncing or not self.live:
        return
    mod = find_mod(context.object)
    if mod is not None:
        push(self, mod)


def _props():
    return {
        "width": FloatProperty(name="Width", default=0.1, min=0.0, soft_max=1.0, subtype='DISTANCE', unit='LENGTH',
                               update=_live, description="Distance from the edge to where the bevel starts on each face"),
        "segments": IntProperty(name="Segments", default=4, min=1, max=64, update=_live,
                                description="Segments across the bevel"),
        "profile": FloatProperty(name="Profile", default=0.5, min=0.0, max=1.0, subtype='FACTOR', update=_live,
                                 description="0 = flat chamfer, 0.5 = round, 1 = sharp"),
        "use_angle": BoolProperty(name="By Angle", default=True, update=_live,
                                  description="Bevel edges sharper than Angle"),
        "angle": FloatProperty(name="Angle", default=math.radians(30.0), min=0.0, max=math.pi, subtype='ANGLE',
                               update=_live, description="Minimum angle between the faces"),
        "use_sharp": BoolProperty(name="Marked Sharp", default=False, update=_live,
                                  description="Bevel edges marked Sharp"),
        "use_marked": BoolProperty(name="Marked Better Bevel", default=True, update=_live,
                                   description="Bevel edges marked with this tool in Edit Mode"),
        "spacing": FloatProperty(name="Spacing", default=0.05, min=0.0, soft_max=0.5, subtype='DISTANCE',
                                 unit='LENGTH', update=_live,
                                 description="Row length along the edge (0 = the edge's own vertices)"),
        "tube_resolution": IntProperty(name="Tube Sides", default=16, min=3, max=256, update=_live,
                                       description="Sides of the cutting tube"),
        "snap": BoolProperty(name="Snap to Cut", default=True, update=_live,
                             description="Snap the bevel's borders onto the boolean cut"),
        "weld": BoolProperty(name="Weld", default=True, update=_live, description="Merge the bevel with the cut mesh"),
        "fill_corners": BoolProperty(name="Fill Corners", default=True, update=_live,
                                     description="Close the holes where several bevels meet"),
    }


class HBBetterBevelSettings(bpy.types.PropertyGroup):
    __annotations__ = dict(_props(), **{
        "live": BoolProperty(name="Live", default=True,
                             description="Tool settings edit the active object's Better Bevel modifier"),
        "syncing": BoolProperty(options={'HIDDEN', 'SKIP_SAVE'}),
    })


def draw_settings(layout, st, full=True):
    col = layout.column()
    col.prop(st, "width")
    col.prop(st, "segments")
    col.prop(st, "profile", slider=True)
    if not full:
        return
    col.separator()
    col.prop(st, "use_angle")
    sub = col.row()
    sub.active = st.use_angle
    sub.prop(st, "angle")
    col.prop(st, "use_sharp")
    col.prop(st, "use_marked")
    col.separator()
    col.prop(st, "spacing")
    col.prop(st, "tube_resolution")
    col.prop(st, "snap")
    col.prop(st, "weld")
    sub = col.row()
    sub.active = st.weld
    sub.prop(st, "fill_corners")


def apply_to(obj, st, context):
    """Add (or update) the Better Bevel modifier on obj with the tool settings."""
    mod = find_mod(obj)
    if mod is None:
        mod = obj.modifiers.new(MOD_NAME, 'NODES')
        mod.node_group = nodes.ensure_group()
        mod.show_in_editmode = True
    elif mod.node_group.get("hb_version") != nodes.VERSION:
        mod.node_group = nodes.ensure_group()
    push(st, mod)
    return mod


def mark_edges(obj, value):
    """Write the Better Bevel edge mark on the selected edges (Edit Mode)."""
    me = obj.data
    bm = bmesh.from_edit_mesh(me)
    lay = bm.edges.layers.bool.get(nodes.MARK_ATTR) if hasattr(bm.edges.layers, "bool") else None
    if lay is None:
        if hasattr(bm.edges.layers, "bool"):
            lay = bm.edges.layers.bool.new(nodes.MARK_ATTR)
        else:
            lay = bm.edges.layers.int.get(nodes.MARK_ATTR) or bm.edges.layers.int.new(nodes.MARK_ATTR)
    n = 0
    for e in bm.edges:
        if e.select:
            e[lay] = value
            n += 1
    bmesh.update_edit_mesh(me, loop_triangles=False, destructive=False)
    return n


class OBJECT_OT_hb_better_bevel(bpy.types.Operator):
    """Add a Better Bevel modifier with the tool settings (Edit Mode: mark the selected edges for it)"""
    bl_idname = "object.hb_better_bevel"
    bl_label = "Better Bevel"
    bl_options = {'REGISTER', 'UNDO'}
    __annotations__ = _props()

    @classmethod
    def poll(cls, context):
        return any(o.type == 'MESH' for o in context.selected_objects) or (
            context.object is not None and context.object.type == 'MESH')

    def invoke(self, context, event):
        if context.mode == 'EDIT_MESH':
            self.use_marked = True
        return self.execute(context)

    def draw(self, context):
        draw_settings(self.layout, self)

    def execute(self, context):
        st = context.scene.hb_betterbevel
        for k in SOCKETS:  # anything not given explicitly comes from the tool settings
            if not self.properties.is_property_set(k):
                setattr(self, k, getattr(st, k))
        st.syncing = True
        try:
            for k in SOCKETS:
                setattr(st, k, getattr(self, k))
        finally:
            st.syncing = False
        if context.mode == 'EDIT_MESH':
            objs = [o for o in context.objects_in_mode if o.type == 'MESH']
            n = sum(mark_edges(o, True) for o in objs)
            if not n:
                self.report({'WARNING'}, "Select the edges to bevel")
        else:
            objs = [o for o in context.selected_objects if o.type == 'MESH'] or [context.object]
        for o in objs:
            apply_to(o, st, context)
        return {'FINISHED'}


class OBJECT_OT_hb_better_bevel_clear(bpy.types.Operator):
    """Edit Mode: remove the Better Bevel mark from the selected edges. Object Mode: remove the modifier"""
    bl_idname = "object.hb_better_bevel_clear"
    bl_label = "Clear Better Bevel"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        if context.mode == 'EDIT_MESH':
            for o in context.objects_in_mode:
                if o.type == 'MESH':
                    mark_edges(o, False)
        else:
            for o in context.selected_objects:
                mod = find_mod(o)
                if mod is not None:
                    o.modifiers.remove(mod)
        return {'FINISHED'}


class OBJECT_OT_hb_better_bevel_click(bpy.types.Operator):
    """Click an object: add Better Bevel with the tool settings, or pick up the settings of its existing one"""
    bl_idname = "object.hb_better_bevel_click"
    bl_label = "Better Bevel Click"
    bl_options = {'REGISTER', 'UNDO'}

    def invoke(self, context, event):
        region, rv3d = context.region, context.region_data
        if rv3d is None:
            return {'CANCELLED'}
        co = (event.mouse_region_x, event.mouse_region_y)
        org = view3d_utils.region_2d_to_origin_3d(region, rv3d, co)
        vec = view3d_utils.region_2d_to_vector_3d(region, rv3d, co)
        dg = context.evaluated_depsgraph_get()
        hit, _loc, _n, _i, obj, _m = context.scene.ray_cast(dg, org, vec)
        if not hit or obj is None or obj.type != 'MESH':
            if not event.shift:
                bpy.ops.object.select_all(action='DESELECT')
            return {'CANCELLED'}
        obj = bpy.data.objects.get(obj.name) or obj
        if not event.shift:
            for o in context.selected_objects:
                o.select_set(False)
        obj.select_set(True)
        context.view_layer.objects.active = obj
        st = context.scene.hb_betterbevel
        mod = find_mod(obj)
        if mod is not None:
            pull(st, mod)
            self.report({'INFO'}, "Better Bevel: editing %s" % obj.name)
        else:
            apply_to(obj, st, context)
        return {'FINISHED'}


class VIEW3D_PT_hb_betterbevel_options(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Better Bevel Options"

    def draw(self, context):
        self.layout.use_property_split = True
        st = context.scene.hb_betterbevel
        draw_settings(self.layout, st)
        self.layout.prop(st, "live")


def _tool_settings(context, layout, tool):
    st = context.scene.hb_betterbevel
    layout.prop(st, "width")
    layout.prop(st, "segments")
    layout.prop(st, "profile", slider=True)
    layout.prop(st, "angle")
    layout.popover("VIEW3D_PT_hb_betterbevel_options", text="", icon='PREFERENCES')
    if context.mode == 'EDIT_MESH':
        layout.operator("object.hb_better_bevel", text="Mark Selected", icon='CHECKMARK')
        layout.operator("object.hb_better_bevel_clear", text="Clear", icon='X')
        layout.operator("mesh.hb_better_bevel_gridfill", icon='MESH_GRID')
    else:
        layout.operator("object.hb_better_bevel", text="Apply to Selected", icon='CHECKMARK')
        layout.operator("object.hb_better_bevel_apply_fill", text="Apply & Grid Fill", icon='MESH_GRID')


def _make_tool(mode):
    if mode == 'OBJECT':
        keymap = (("object.hb_better_bevel_click", {"type": 'LEFTMOUSE', "value": 'CLICK'}, None),
                  ("object.hb_better_bevel_click", {"type": 'LEFTMOUSE', "value": 'CLICK', "shift": True}, None),
                  ("object.hb_better_bevel", {"type": 'RET', "value": 'PRESS'}, None),
                  ("object.hb_better_bevel_apply_fill", {"type": 'RET', "value": 'PRESS', "shift": True}, None))
        desc = ("Click an object to bevel it with the tool settings (click a bevelled object to edit its "
                "settings). The bevel stays live as a Geometry Nodes modifier. Shift+Enter: apply and Grid Fill "
                "the corner patches")
    else:
        keymap = (("view3d.select", {"type": 'LEFTMOUSE', "value": 'CLICK'}, {"properties": [("deselect_all", True)]}),
                  ("view3d.select", {"type": 'LEFTMOUSE', "value": 'CLICK', "shift": True},
                   {"properties": [("toggle", True)]}),
                  ("view3d.select_box", {"type": 'LEFTMOUSE', "value": 'CLICK_DRAG'}, None),
                  ("view3d.select_box", {"type": 'LEFTMOUSE', "value": 'CLICK_DRAG', "shift": True},
                   {"properties": [("mode", 'ADD')]}),
                  ("view3d.select_box", {"type": 'LEFTMOUSE', "value": 'CLICK_DRAG', "ctrl": True},
                   {"properties": [("mode", 'SUB')]}),
                  ("object.hb_better_bevel", {"type": 'RET', "value": 'PRESS'}, None),
                  ("object.hb_better_bevel_clear", {"type": 'RET', "value": 'PRESS', "alt": True}, None),
                  ("mesh.hb_better_bevel_gridfill", {"type": 'RET', "value": 'PRESS', "shift": True}, None))
        desc = ("Select edges, press Enter (or Mark Selected) to give them a Better Bevel; Alt+Enter clears. "
                "Shift+Enter: replace the selected faces (a leftover patch) with a Grid Fill")
    return type("HB_TL_better_bevel_%s" % mode.lower(), (bpy.types.WorkSpaceTool,), {
        "bl_space_type": 'VIEW_3D',
        "bl_context_mode": mode,
        "bl_idname": "hb_betterbevel.tool",
        "bl_label": "Better Bevel",
        "bl_description": desc,
        "bl_icon": "ops.mesh.bevel",
        "bl_keymap": keymap,
        "draw_settings": _tool_settings,
    })


def _menu_object(self, context):
    self.layout.separator()
    self.layout.operator("object.hb_better_bevel", icon='MOD_BEVEL')
    self.layout.operator("object.hb_better_bevel_apply_fill", text="Apply Better Bevel & Grid Fill", icon='MESH_GRID')


def _menu_edge(self, context):
    self.layout.separator()
    self.layout.operator("object.hb_better_bevel", text="Mark Better Bevel")
    self.layout.operator("object.hb_better_bevel_clear", text="Clear Better Bevel")
    self.layout.operator("mesh.hb_better_bevel_gridfill", text="Grid Fill Patch (Better Bevel)")


def _menu_face(self, context):
    self.layout.separator()
    self.layout.operator("mesh.hb_better_bevel_gridfill", text="Grid Fill Patch (Better Bevel)", icon='MESH_GRID')


_tools = []
classes = (HBBetterBevelSettings, OBJECT_OT_hb_better_bevel, OBJECT_OT_hb_better_bevel_clear,
           OBJECT_OT_hb_better_bevel_click, VIEW3D_PT_hb_betterbevel_options) + gridfill.classes


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.hb_betterbevel = PointerProperty(type=HBBetterBevelSettings)
    bpy.types.VIEW3D_MT_object.append(_menu_object)
    bpy.types.VIEW3D_MT_edit_mesh_edges.append(_menu_edge)
    bpy.types.VIEW3D_MT_edit_mesh_faces.append(_menu_face)
    for mode in ('OBJECT', 'EDIT_MESH'):
        tool = _make_tool(mode)
        try:
            bpy.utils.register_tool(tool, separator=True)
            _tools.append(tool)
        except Exception as ex:
            print("HB_BetterBevel: tool not added for %s (%s)" % (mode, ex))


def unregister():
    for tool in reversed(_tools):
        try:
            bpy.utils.unregister_tool(tool)
        except Exception:
            pass
    _tools.clear()
    bpy.types.VIEW3D_MT_edit_mesh_faces.remove(_menu_face)
    bpy.types.VIEW3D_MT_edit_mesh_edges.remove(_menu_edge)
    bpy.types.VIEW3D_MT_object.remove(_menu_object)
    del bpy.types.Scene.hb_betterbevel
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
