# HB_BevelFix for Blender

Fixes bevels that overlap / fold where the bevel width is bigger than a neighbouring
edge, by **edge-sliding only the affected vertices** to a smoothed value — unlike
*Clamp Overlap*, which shrinks the whole bevel.

![before / after](preview.png)

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
