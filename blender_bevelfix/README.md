# HB_BevelFix for Blender

Fixes bevels that overlap / fold where the bevel width is bigger than a neighbouring
edge, by **edge-sliding only the affected vertices** to a smoothed value — unlike
*Clamp Overlap*, which shrinks the whole bevel.

![before / slide fix / weld fix](preview.png)
*Left to right: overlapping bevel, Fix (slide), Fix with Weld Conflicts.*

## Install (Blender 4.2+)
Edit > Preferences > Get Extensions > ⌄ (top right) > **Install from Disk…** > `hb_bevelfix.zip`.

## Use
1. Bevel as usual (Ctrl+B). Leave *Clamp Overlap* off.
2. With the bevel's faces still selected, run **Fix Bevel Overlap**:
   **Ctrl+Alt+B**, or Edge menu (Ctrl+E), or the right-click menu.
3. Tweak in the redo panel (bottom-left):
   - **Max Slide** – furthest a fixed vertex may go along its edge (fraction of the edge)
   - **Tolerance** – how different a vertex's slide may be from its neighbours before it's fixed
   - **Blend** – also ease this many neighbouring vertices for a gradual transition
   - **Smooth Whole Bevel** – smooth every vertex's slide, not only the broken ones
   - **Weld Conflicts** (aggressive) – where an edge is too short for the bevel, the vertex at its
     far end is merged into the bevel vertex, so the bevel keeps its full, smooth width instead of
     being squeezed (the idea of MESHmachine's Boolean Cleanup, which merges the vertices on one
     side of a seam into the other; MESHmachine's Offset/Chamfer "Merge Perimeter" does the same)
   - **Merge Threshold** – collapse neighbouring bevel slices that ended up closer than this,
     removing sliver faces (MESHmachine's Boolean Cleanup merges loop vertices into their closest
     neighbour within a threshold). 0 = off
   - **Width** – widen or narrow the whole bevel afterwards by sliding along its edges (with the
     same clamping / welding), **Taper** ramps it along an open bevel, **Flip** reverses the ramp
     (MESHmachine's Change Width)

## Select Loop (Angle)
Select > **Select Loop (Angle)** (also in the Edge and right-click menus). Grows edge loops from
the selected edge(s) in the direction they point: at each vertex it continues along the edge that
turns least, up to **Max Angle** (raise it to follow tighter curves). Unlike Blender's loop select
it doesn't need quad topology — handy for picking an edge loop to bevel on messy boolean meshes.
If every selected edge is marked sharp, it selects all connected sharp edges instead
(**Connected Sharps** option). Modelled on MESHmachine's LSelect / Select tools.

Tip: you can still adjust the bevel's own redo panel first — run the fix once you're happy
with the width, since running it ends the bevel's redo.

## How it works
The selected bevel is read as slices across the strip. For each slice the two edges the
outer vertices slid along are intersected to recover the original corner, giving every
outer vertex a *slide distance*. Vertices that slid past the end of their edge, or much
further than their neighbours, get the average of their neighbours' slides (healthy ones
stay put), clamped inside the edge. Where Blender left a vertex inside a face (no edge
back to the corner), it slides along that surface instead. The inner profile vertices are
re-mapped so the bevel keeps its shape.

Works on quad-strip bevels (any segment count). Slices it can't read (e.g. complex miter
patches at corners) are left untouched.
