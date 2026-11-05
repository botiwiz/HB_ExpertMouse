# SPDX-License-Identifier: GPL-2.0-or-later
"""
HB_Workplane: a temporary construction plane (like Plasticity's). Press Space to put
the workplane on the selected face(s) / the face under the mouse; Move (G) and Rotate (R)
are then constrained to it. Without a custom plane, the world plane facing the view is used.
"""

bl_info = {
    "name": "HB_Workplane",
    "author": "HB",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Space (set workplane), Sidebar > HB_Workplane",
    "description": "Temporary construction plane from the selection, with constrained Move/Rotate and a grid",
    "category": "3D View",
}

import bpy
from bpy.props import BoolProperty

from . import props, ops, gridmove, draw, ui

_modules = (props, ops, gridmove, draw, ui)
_keymaps = []  # (keymap, keymap_item)

# (keymap name, operator, key args, properties)
KEYS_SET = (
    ("3D View", "hb_workplane.set", {"type": 'SPACE', "value": 'PRESS'}, {}),
    ("3D View", "hb_workplane.clear", {"type": 'SPACE', "value": 'PRESS', "alt": True}, {}),
)
TRANSFORM_KEYMAPS = ("Object Mode", "Mesh", "Curve", "Armature", "Pose", "Lattice", "Metaball")
KEYS_TRANSFORM = tuple(
    item for km in TRANSFORM_KEYMAPS for item in (
        (km, "hb_workplane.move", {"type": 'G', "value": 'PRESS'}, {}),
        (km, "hb_workplane.rotate", {"type": 'R', "value": 'PRESS'}, {}),
    )) + (("Mesh", "hb_workplane.extrude", {"type": 'E', "value": 'PRESS'}, {}),)


def _register_keymaps():
    kc = bpy.context.window_manager.keyconfigs.addon
    if kc is None:  # background mode
        return
    prefs = _prefs()
    items = list(KEYS_SET)
    if prefs is None or prefs.override_transform_keys:
        items += KEYS_TRANSFORM
    for km_name, idname, args, op_props in items:
        km = kc.keymaps.new(name=km_name, space_type='VIEW_3D' if km_name == "3D View" else 'EMPTY')
        kmi = km.keymap_items.new(idname, **args)
        for k, v in op_props.items():
            setattr(kmi.properties, k, v)
        _keymaps.append((km, kmi))


def _unregister_keymaps():
    for km, kmi in _keymaps:
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass
    _keymaps.clear()


def _prefs():
    addon = bpy.context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


def _update_keys(self, context):
    _unregister_keymaps()
    _register_keymaps()


class HBWorkplanePreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    override_transform_keys: BoolProperty(
        name="Constrain G / R / E", default=True, update=_update_keys,
        description="Make G (Move), R (Rotate) and E (Extrude) follow the workplane. "
                    "The toggles in the sidebar can switch this per scene")

    def draw(self, context):
        import rna_keymap_ui
        layout = self.layout
        layout.prop(self, "override_transform_keys")
        layout.label(text="Shortcuts (click to change):")
        kc = context.window_manager.keyconfigs.user
        col = layout.column()
        seen = set()
        for km, kmi in _keymaps:
            if kmi.idname in seen and kmi.idname != "hb_workplane.set":
                continue
            seen.add(kmi.idname)
            ukm = kc.keymaps.get(km.name)
            if ukm is None:
                continue
            for ukmi in ukm.keymap_items:
                if ukmi.idname == kmi.idname:
                    col.context_pointer_set("keymap", ukm)
                    rna_keymap_ui.draw_kmi([], kc, ukm, ukmi, col, 0)
                    break


def register():
    bpy.utils.register_class(HBWorkplanePreferences)
    for m in _modules:
        m.register()
    _register_keymaps()


def unregister():
    _unregister_keymaps()
    for m in reversed(_modules):
        m.unregister()
    bpy.utils.unregister_class(HBWorkplanePreferences)
