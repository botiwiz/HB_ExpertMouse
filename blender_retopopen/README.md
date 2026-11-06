# HB_RetopoPen for Blender

Draw the outline of a patch on the surface and get a clean quad patch — retopology by drawing.

![patches on a sphere and a cube](preview.png)

## Install (Blender 4.2+)
Edit > Preferences > Get Extensions > ⌄ (top right) > **Install from Disk…** > `hb_retopopen.zip`.

## Use
Pick **Retopo Pen** in the toolbar (Object or Edit Mode). The patches go into the mesh you're
editing, or into an object called *Retopo* (created for you, shown in front / with wireframe).

| Action | Result |
|---|---|
| **Click** | Add a corner — on the surface under the mouse (any visible meshes), or on an existing retopo vertex within *Snap Radius* |
| **Click the first point** / **Enter** | Close the outline → the patch appears |
| **Mouse wheel** / **+ −** | Density (live, before and after closing) |
| **Click / Enter** (patch shown) | Keep it — then draw the next one |
| **Backspace** / **Ctrl+Z** | Remove the last point (or go back from the patch to the outline) |
| **Esc / right-click** | Discard the outline / patch; again to leave the pen |

After keeping a patch, the redo panel (bottom-left) changes its **Resolution**, **Relax** and
**Layout** at any time. Tool Settings bar: Resolution, Relax, Layout, Snap Radius.

- **4 corners**: a grid (each pair of opposite sides gets its own count).
  **Any other number**: one quad region per corner around a centre pole — all quads.
  *Layout: Centre Pole* uses that for 4 corners too.
- **Neighbouring patches stitch**: a side drawn between two vertices of an existing open border
  reuses that border's vertices (and sets the patch's segment count to match), so patches join
  into one mesh.
- Every vertex is projected onto the nearest surface and *Relax* passes even the flow out.
