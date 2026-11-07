# HB_BetterBevel

Non-destructive bevel for boolean-heavy meshes (Blender 4.2+ extension; tested in 5.2).

Every chosen edge gets an even-thickness tube that is booleaned out of the mesh. The tube walls are
removed and the two clean cut lines are bridged with a strip whose cross-section is a rational
quadratic Bezier, so **Profile 0.5 is a true circular arc**, 0 is a flat chamfer and 1 is sharp.
Everything runs in a Geometry Nodes modifier ("HB Better Bevel"), so it stays live.

## Use it as a tool
Toolbar > **Better Bevel** (Object and Edit Mode). Tool Settings: Width, Segments, Profile, Angle,
plus a ⚙ popover with the rest.

* **Object Mode**
  * Click an object to bevel it with the tool settings.
  * Click an already-bevelled object to load its settings into the tool.
  * Enter, or **Apply to Selected**, applies to every selected mesh.
* **Edit Mode**
  * Select edges and press Enter (or **Mark Selected**) to mark them for the bevel; Alt+Enter clears the mark.
  * Click and drag select as usual.
* **Live** (on by default): changing the tool settings edits the active object's Better Bevel modifier.
* Also available from Object menu > Better Bevel and the Edge menu (Ctrl+E) > Mark / Clear Better Bevel.
* Afterwards it is a normal Geometry Nodes modifier, so every setting is also on the modifier panel.

## Grid Fill for leftover patches
Corners and slivers are closed with a quick fan inside the modifier. Grid Fill can't run inside
Geometry Nodes, so there are two commands that replace those patches with real Grid Fills:

* **Apply & Grid Fill** (Object Mode; destructive):
  * Applies the modifier, deletes the fan patches and Grid Fills every hole along the bevel.
  * Odd loops get one extra vertex. If a hole can't be gridded, it falls back to a beauty fill.
  * Find it in the tool settings button, Shift+Enter with the tool, or Object menu > Apply Better Bevel & Grid Fill.
* **Grid Fill Patch** (Edit Mode; works on any mesh):
  * Select the faces of an ugly patch, run it, and they're replaced by a Grid Fill of their outline.
    It also fills selected open edge loops.
  * Find it in the tool settings button, Shift+Enter with the tool, Face menu (Ctrl+F), or Edge menu (Ctrl+E).

Both are in F3 search too ("Grid Fill Patch", "Apply Better Bevel").

## Settings
| | |
|---|---|
| Width | distance from the edge to where the bevel starts on each face (tube radius) |
| Segments / Profile | rows across the bevel / shape (0.5 = round) |
| By Angle, Angle | bevel every edge sharper than Angle |
| Marked Sharp | bevel edges marked Sharp |
| Marked Better Bevel | bevel edges marked with the tool (edge attribute `hb_better_bevel`) |
| Spacing | row length along the edge (0 = the edge's own vertices) |
| Tube Sides | resolution of the cutting tube |
| Snap to Cut | snap the bevel borders onto the boolean cut |
| Weld / Fill Corners | merge the bevel into the mesh / close the holes where several bevels meet |

The bevel faces get a face attribute `better_bevel` you can use in shaders.

## Notes / limits (v1)
* Uses the Exact boolean, which is slow on dense meshes. The mesh should be closed (manifold).
* Corners where 3 or more bevels meet are closed with a fan, not a true vertex blend. Small pinches
  and slits can remain; use Apply & Grid Fill or Grid Fill Patch on them.
* Faces narrower than Width (thin walls, slivers) can't hold the bevel and come out faceted, same
  as with the native Bevel.

## Install
Run `python build_zip.py`, then Edit > Preferences > Get Extensions > Install from Disk > `hb_betterbevel.zip`.
