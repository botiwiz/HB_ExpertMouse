# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
from bpy.props import EnumProperty, IntVectorProperty
from mathutils import Matrix, Vector
from bpy_extras import view3d_utils

from . import plane

ORIENT_NAME = "HB_Workplane"


def tag_redraw_all(context):
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


def view_rv3d(context):
    rv3d = context.region_data
    if rv3d is None and context.area and context.area.type == 'VIEW_3D':
        rv3d = context.area.spaces.active.region_3d
    return rv3d


def current_matrix(context, rv3d=None):
    """The active workplane matrix (custom, or automatic from the view)."""
    s = context.scene.hb_workplane
    if s.mode == 'CUSTOM':
        return Matrix(s.matrix)
    rv3d = rv3d or view_rv3d(context)
    if rv3d is None:
        return Matrix.Identity(4)
    m, axis = plane.auto_matrix(rv3d.view_rotation, s.axis_bias)
    # Center the automatic grid under the view focus.
    n = m.col[2].to_3d()
    focus = Vector(rv3d.view_location)
    m.translation = focus - n * focus.dot(n)
    return m


# ----------------------------------------------------------------------------
# Transform orientation sync

def _sync_orientation(context, m):
    s = context.scene.hb_workplane
    if not s.sync_orientation:
        return
    slot = context.scene.transform_orientation_slots[0]
    if slot.type != ORIENT_NAME:
        s.prev_orientation = slot.type
    try:
        slot.type = ORIENT_NAME
    except TypeError:  # does not exist yet
        try:
            bpy.ops.transform.create_orientation(name=ORIENT_NAME, use_view=True, use=True, overwrite=True)
        except RuntimeError:
            return
    if slot.type == ORIENT_NAME and slot.custom_orientation:
        slot.custom_orientation.matrix = m.to_3x3()


def _restore_orientation(context):
    s = context.scene.hb_workplane
    slot = context.scene.transform_orientation_slots[0]
    if slot.type == ORIENT_NAME:
        try:
            slot.type = s.prev_orientation or 'GLOBAL'
        except TypeError:
            slot.type = 'GLOBAL'


def set_custom(context, m):
    s = context.scene.hb_workplane
    s.matrix = m
    s.mode = 'CUSTOM'
    _sync_orientation(context, m)
    tag_redraw_all(context)


def clear_custom(context):
    s = context.scene.hb_workplane
    s.mode = 'AUTO'
    _restore_orientation(context)
    tag_redraw_all(context)


# ----------------------------------------------------------------------------
# Operators

class HB_WORKPLANE_OT_set(bpy.types.Operator):
    """Set the workplane from the selection (Edit Mode), else the face under the mouse.
Press again on the same plane (or over nothing) to go back to the automatic plane"""
    bl_idname = "hb_workplane.set"
    bl_label = "Set Workplane"
    bl_options = {'REGISTER', 'UNDO'}

    source: EnumProperty(name="Source", items=(
        ('AUTO', "Selection / Under Mouse", "Selection in Edit Mode, otherwise the face under the mouse"),
        ('SELECTION', "Selection", "Selected faces / edges / vertices (Edit Mode)"),
        ('HOVER', "Under Mouse", "Face under the mouse"),
        ('CURSOR', "3D Cursor", "The 3D cursor's position and rotation"),
        ('VIEW', "View", "Plane facing the view, through the 3D cursor"),
        ('CLEAR', "Clear", "Back to the automatic world plane"),
    ), default='AUTO')
    x_axis: EnumProperty(name="X Axis", items=(
        ('EDGE', "Longest Edge", "Align X with the longest edge of the (active) face"),
        ('WORLD', "World", "Align X with the closest world axis"),
    ), default='EDGE')
    toggle: bpy.props.BoolProperty(name="Toggle", default=True, options={'SKIP_SAVE'},
                                   description="Clear the workplane if it is already set to this plane")
    mouse: IntVectorProperty(size=2, default=(-1, -1), options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def invoke(self, context, event):
        if context.region and context.region.type == 'WINDOW':
            self.mouse = (event.mouse_region_x, event.mouse_region_y)
        return self.execute(context)

    def _view_dir(self, context):
        rv3d = view_rv3d(context)
        if rv3d is None:
            return Vector((0, 0, -1))
        return rv3d.view_rotation @ Vector((0, 0, -1))

    def _from_hover(self, context):
        rv3d = view_rv3d(context)
        region = context.region
        if rv3d is None or region is None or region.type != 'WINDOW' or self.mouse[0] < 0:
            return None
        co = Vector(self.mouse)
        origin = view3d_utils.region_2d_to_origin_3d(region, rv3d, co)
        direction = view3d_utils.region_2d_to_vector_3d(region, rv3d, co)
        hit, loc, normal, index, obj, _mw = context.scene.ray_cast(
            context.evaluated_depsgraph_get(), origin, direction)
        if not hit:
            return None
        if normal.dot(direction) > 0:
            normal = -normal
        return plane.from_hit(context, loc, normal, obj, index, self.x_axis)

    def _from_selection(self, context):
        if context.mode != 'EDIT_MESH':
            return None
        objs = list(getattr(context, "objects_in_mode", None) or [context.edit_object])
        for o in objs:
            o.update_from_editmode()
        return plane.from_edit_selection([o for o in objs if o], self._view_dir(context), self.x_axis)

    def execute(self, context):
        if self.source == 'CLEAR':
            clear_custom(context)
            return {'FINISHED'}
        m = None
        if self.source in {'AUTO', 'SELECTION'}:
            m = self._from_selection(context)
        if m is None and self.source in {'AUTO', 'HOVER'}:
            m = self._from_hover(context)
        if self.source == 'CURSOR':
            m = context.scene.cursor.matrix.copy()
            if self.x_axis == 'WORLD':
                n = m.col[2].to_3d()
                m = plane.make_matrix(m.translation, n, plane.world_x_hint(n))
        if self.source == 'VIEW':
            vd = self._view_dir(context)
            m = plane.make_matrix(context.scene.cursor.location, -vd, plane.world_x_hint(vd))

        s = context.scene.hb_workplane
        if m is None:
            if self.toggle and s.mode == 'CUSTOM':
                clear_custom(context)
                self.report({'INFO'}, "Workplane: automatic")
                return {'FINISHED'}
            self.report({'WARNING'}, "Nothing selected or under the mouse to set a workplane from")
            return {'CANCELLED'}
        if self.toggle and s.mode == 'CUSTOM' and plane.same_plane(Matrix(s.matrix), m):
            clear_custom(context)
            self.report({'INFO'}, "Workplane: automatic")
            return {'FINISHED'}
        set_custom(context, m)
        return {'FINISHED'}


class HB_WORKPLANE_OT_clear(bpy.types.Operator):
    """Go back to the automatic workplane (world plane facing the view)"""
    bl_idname = "hb_workplane.clear"
    bl_label = "Clear Workplane"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        clear_custom(context)
        return {'FINISHED'}


class HB_WORKPLANE_OT_cursor_to_workplane(bpy.types.Operator):
    """Move and rotate the 3D cursor to the workplane"""
    bl_idname = "hb_workplane.cursor_to_workplane"
    bl_label = "3D Cursor to Workplane"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        context.scene.cursor.matrix = current_matrix(context)
        return {'FINISHED'}


def _wrap(result):
    # The nested transform keeps running on its own; this wrapper is done.
    return {'CANCELLED'} if result == {'CANCELLED'} else {'FINISHED'}


def _orient_kwargs(m):
    return dict(orient_type='GLOBAL', orient_matrix=m.to_3x3(), orient_matrix_type='GLOBAL')


class HB_WORKPLANE_OT_move(bpy.types.Operator):
    """Move, constrained to the workplane (press X/Y/Z during the move to change it as usual)"""
    bl_idname = "hb_workplane.move"
    bl_label = "Move (Workplane)"
    bl_options = {'INTERNAL'}

    def invoke(self, context, event):
        s = context.scene.hb_workplane
        if not (s.enabled and s.constrain_move):
            return _wrap(bpy.ops.transform.translate('INVOKE_DEFAULT'))
        if s.grid_snap and context.mode in {'OBJECT', 'EDIT_MESH'}:
            return _wrap(bpy.ops.hb_workplane.grid_move('INVOKE_DEFAULT', axis='PLANE'))
        m = current_matrix(context)
        return _wrap(bpy.ops.transform.translate('INVOKE_DEFAULT', constraint_axis=(True, True, False),
                                                 **_orient_kwargs(m)))


class HB_WORKPLANE_OT_rotate(bpy.types.Operator):
    """Rotate around the workplane normal"""
    bl_idname = "hb_workplane.rotate"
    bl_label = "Rotate (Workplane)"
    bl_options = {'INTERNAL'}

    def invoke(self, context, event):
        s = context.scene.hb_workplane
        if not (s.enabled and s.constrain_rotate):
            return _wrap(bpy.ops.transform.rotate('INVOKE_DEFAULT'))
        m = current_matrix(context)
        return _wrap(bpy.ops.transform.rotate('INVOKE_DEFAULT', orient_axis='Z',
                                              constraint_axis=(False, False, True), **_orient_kwargs(m)))


class HB_WORKPLANE_OT_extrude(bpy.types.Operator):
    """Extrude: vertices/edges move along the workplane, faces along its normal
(each can be switched off in the sidebar to get Blender's normal extrude)"""
    bl_idname = "hb_workplane.extrude"
    bl_label = "Extrude (Workplane)"
    bl_options = {'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def invoke(self, context, event):
        import bmesh
        s = context.scene.hb_workplane
        has_faces = any(f.select for o in (context.objects_in_mode or [])
                        if o.type == 'MESH' for f in bmesh.from_edit_mesh(o.data).faces)
        use = s.enabled and (s.constrain_extrude_face if has_faces else s.constrain_extrude_vert)
        if not use:
            return _wrap(bpy.ops.view3d.edit_mesh_extrude_move_normal('INVOKE_DEFAULT'))
        axis = 'NORMAL' if has_faces else 'PLANE'
        if s.grid_snap:
            if bpy.ops.mesh.extrude_region() == {'CANCELLED'}:
                return {'CANCELLED'}
            return _wrap(bpy.ops.hb_workplane.grid_move('INVOKE_DEFAULT', axis=axis))
        m = current_matrix(context)
        constraint = (False, False, True) if has_faces else (True, True, False)
        return _wrap(bpy.ops.mesh.extrude_region_move(
            'INVOKE_DEFAULT',
            TRANSFORM_OT_translate=dict(constraint_axis=constraint, **_orient_kwargs(m))))


classes = (HB_WORKPLANE_OT_set, HB_WORKPLANE_OT_extrude, HB_WORKPLANE_OT_clear, HB_WORKPLANE_OT_cursor_to_workplane,
           HB_WORKPLANE_OT_move, HB_WORKPLANE_OT_rotate)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
