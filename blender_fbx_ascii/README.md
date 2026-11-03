# ASCII FBX Importer for Blender

Blender's built-in FBX importer only reads **binary** FBX and rejects ASCII files
("ASCII FBX files are not supported"). This extension adds
**File > Import > FBX (ASCII + Binary) (.fbx)**, which reads both.

## How it works

ASCII files are parsed and re-encoded as a temporary binary FBX (using Blender's own
FBX encoder), then loaded by Blender's own FBX importer. All the usual import options
(scale, axes, animation, armature, materials…) are available and behave the same.
The temporary file is written next to the source (so relative texture paths still
work) and deleted right after the import. Binary files are passed straight through.

## Install (Blender 4.2+)

1. Download `fbx_ascii_import.zip` from this folder.
2. Blender: **Edit > Preferences > Get Extensions**, click the **⌄** menu (top right) > **Install from Disk…**, pick the zip.
3. Use **File > Import > FBX (ASCII + Binary) (.fbx)**.

Rebuild the zip after editing the code: `python build_zip.py`

## Limitations

- FBX **7.x** ASCII only (the common format, FBX 2011 and newer). FBX 6.x ASCII
  (very old, pre-2011) has a different layout and is rejected with a message —
  same as the built-in importer does for binary 6.x.
- Needs the bundled "FBX format" add-on enabled (it is by default).
- Tested with Blender 5.2: meshes, UVs, vertex colors, materials, embedded
  textures, cameras, lights, armatures, skinning and animation.
