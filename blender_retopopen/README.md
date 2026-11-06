# HB_RetopoPen for Blender

Draw the outline of a patch on the surface and get a clean quad patch — retopology by drawing.

![patches on a sphere and a cube](preview.png)

## Install (Blender 4.2+)
Edit > Preferences > Get Extensions > ⌄ (top right) > **Install from Disk…** > `hb_retopopen.zip`.

## Use
Pick **Retopo Pen** in the toolbar (Object or Edit Mode). The patches go into the mesh you're
editing, or into an object called *Retopo* (created for you, shown in front / with wireframe).

### Drawing the outline
| Action | Result |
|---|---|
| **Click** | Corner — on the surface under the mouse (any visible meshes), or on an existing retopo vertex within *Snap Radius* |
| **Shift + Click** | Curve point — the side through it becomes a smooth curve |
| **Click-drag** | Freehand — the side follows your stroke; where you let go is the next corner |
| **Click the first point** / **Enter** | Close the outline → the patch appears (dragging onto the first point closes too) |
| **Backspace** / **Ctrl+Z** | Remove the last point |
| **Esc / right-click** | Discard the outline; again to leave the pen |

![straight, curved and freehand sides](preview_curves.png)

### Tuning the patch (once the outline is closed)
A panel in the viewport lists everything, live:

| Key | Setting |
|---|---|
| **Wheel / + −** | Density |
| **D** | Density by *Segments* or by *Edge Length* (switching keeps the current size) |
| **L** | Layout: Auto / Grid / Centre Pole |
| **R / Shift+R** | Relax passes |
| **S / Shift+S** | Side smoothing (smooths the drawn outline) |
| **C** | Conform: *Surface* (snap onto it) / *Flat* |
| **G / Shift+G** | Conform strength |
| **F / Shift+F** | Offset above the surface |
| **Click / Enter** | Keep it, then draw the next one · **Backspace**: back to the outline · **Esc**: discard |

After keeping, the redo panel (bottom-left) has all of these too, and the Tool Settings bar holds
the defaults (⚙ for the full list, plus *Snap Radius*).

- **4 corners**: a grid (each pair of opposite sides gets its own count).
  **Any other number**: one quad region per corner around a centre pole — all quads.
- **Every vertex lands on the surface**: after the patch is built and relaxed, each resulting
  vertex is projected onto the geometry underneath (with *Strength* / *Offset*), so a dense grid
  follows detail the outline didn't capture instead of clipping through it. Vertices shared with
  an existing border stay exactly where they are.
- **Neighbouring patches stitch**: a straight side drawn between two vertices of an existing open
  border reuses that border's vertices (curved or not), so patches join into one mesh.
