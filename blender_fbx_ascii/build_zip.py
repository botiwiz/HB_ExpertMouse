"""Package the extension into fbx_ascii_import.zip (install via Blender: Edit > Preferences > Get Extensions > Install from Disk)."""
import os
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "fbx_ascii_import")
out = os.path.join(here, "fbx_ascii_import.zip")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        if name.endswith((".py", ".toml")):
            z.write(os.path.join(src, name), name)
print("Wrote", out)
