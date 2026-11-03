"""Package the extension into hb_caliper.zip (install via Blender: Edit > Preferences > Get Extensions > Install from Disk)."""
import os
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "hb_caliper")
out = os.path.join(here, "hb_caliper.zip")

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _dirs, files in os.walk(src):
        for name in sorted(files):
            if name.endswith((".py", ".toml", ".dat")):
                full = os.path.join(root, name)
                z.write(full, os.path.relpath(full, src))
print("Wrote", out)
