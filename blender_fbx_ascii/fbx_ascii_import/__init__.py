# SPDX-License-Identifier: GPL-2.0-or-later
"""
Import ASCII FBX files in Blender.

Blender's bundled FBX importer only reads binary FBX. This add-on adds
File > Import > "FBX (ASCII + Binary)", which converts ASCII files to a
temporary binary FBX and then runs the bundled importer on it, so every
import option works exactly as in the normal FBX importer.
"""

bl_info = {
    "name": "ASCII FBX Importer",
    "author": "HB",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import > FBX (ASCII + Binary)",
    "description": "Import ASCII FBX files (and binary ones) using Blender's FBX importer",
    "category": "Import-Export",
}

import os
import tempfile

import bpy

from io_scene_fbx import ImportFBX, encode_bin

from . import ascii_fbx


def _temp_path_for(src):
    """Prefer a hidden file next to the source so relative texture paths still resolve."""
    folder, name = os.path.split(src)
    base = os.path.splitext(name)[0]
    candidates = [os.path.join(folder, ".%s.ascii2bin.fbx" % base),
                  os.path.join(tempfile.gettempdir(), "%s.ascii2bin.fbx" % base)]
    for path in candidates:
        try:
            with open(path, 'wb'):
                pass
            return path
        except OSError:
            continue
    raise OSError("No writable location for the temporary binary FBX")


class IMPORT_SCENE_OT_fbx_ascii(ImportFBX):
    """Load an FBX file (ASCII or binary)"""
    bl_idname = "import_scene.fbx_ascii"
    bl_label = "Import FBX (ASCII + Binary)"
    bl_options = {'UNDO', 'PRESET'}

    def _load_one(self, context, path, keywords):
        from io_scene_fbx import import_fbx

        if ascii_fbx.is_binary_fbx(path):
            return import_fbx.load(self, context, filepath=path, **keywords)

        tmp = _temp_path_for(path)
        try:
            try:
                ascii_fbx.convert_file(path, tmp, encode_bin)
            except Exception as ex:
                import traceback
                traceback.print_exc()
                self.report({'ERROR'}, "Could not read ASCII FBX %r: %s" % (path, ex))
                return {'CANCELLED'}
            return import_fbx.load(self, context, filepath=tmp, **keywords)
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass

    def execute(self, context):
        keywords = self.as_keywords(ignore=("filter_glob", "directory", "ui_tab", "filepath", "files"))
        if self.files and self.files[0].name:
            paths = [os.path.join(self.directory, f.name) for f in self.files]
        else:
            paths = [self.filepath]

        ret = {'CANCELLED'}
        for path in paths:
            if self._load_one(context, path, keywords) == {'FINISHED'}:
                ret = {'FINISHED'}
        return ret


def menu_func_import(self, context):
    self.layout.operator(IMPORT_SCENE_OT_fbx_ascii.bl_idname, text="FBX (ASCII + Binary) (.fbx)")


def register():
    bpy.utils.register_class(IMPORT_SCENE_OT_fbx_ascii)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.utils.unregister_class(IMPORT_SCENE_OT_fbx_ascii)
