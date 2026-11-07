"""Package the extension into hb_betterbevel.zip (install via Blender: Edit > Preferences > Get Extensions > Install from Disk)."""
import os
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "hb_betterbevel")
out = os.path.join(here, "hb_betterbevel.zip")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        if name.endswith((".py", ".toml")):
            z.write(os.path.join(src, name), name)
print("Wrote", out)
