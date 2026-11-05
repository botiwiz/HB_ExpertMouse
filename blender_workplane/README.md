# HB_Workplane for Blender

A temporary construction plane, like Plasticity's: put the workplane on a face with
**Space**, and Move / Rotate follow it. A grid shows where the plane is.

| Automatic (world plane facing the view) | Custom (Space on the green face) |
|---|---|
| ![auto](preview_auto.png) | ![custom](preview_custom.png) |

## Install (Blender 4.2+)
Edit > Preferences > Get Extensions > ⌄ (top right) > **Install from Disk…** > `hb_workplane.zip`.

## Use
| Key | Action |
|---|---|
| **Space** | Set the workplane: from the selected face(s) in Edit Mode, otherwise from the face under the mouse. Press again on the same plane (or over empty space) to go back to automatic |
| **Alt+Space** | Back to the automatic plane |
| **G** | Move, constrained to the workplane (X/Y/Z during the move still work as usual) |
| **R** | Rotate around the workplane normal |

- **Automatic mode** (no plane set): the world plane (XY / XZ / YZ) that faces the view is used,
  and switches as you orbit. *Ground Bias* in the sidebar controls how much the floor is preferred.
- **Selection rules** (Edit Mode): one/active face → that face; several faces → their average plane;
  3+ vertices → best-fit plane; an edge → plane through the edge facing you; a vertex → its normal.
  The plane's X axis follows the face's longest edge (or the world axis — redo panel *X Axis*).
- The redo panel (bottom-left after Space) can switch the source to **3D Cursor**, **View** or **Clear**.
- With *Set Transform Orientation* on, the workplane also becomes the transform orientation
  "HB_Workplane", so the gizmo and typed axes follow it; clearing restores the previous one.
- Sidebar (N) > **HB_Workplane**: on/off, Set / From 3D Cursor / Clear / Cursor to Workplane,
  Move/Rotate toggles, grid options (opacity, size, color, hide behind geometry).
- Preferences: rebind the shortcuts, or turn off the G/R override.

Note: Space normally plays the animation; in the 3D view it now sets the workplane
(Shift+Space or the timeline still play). Rebind in the add-on preferences if you prefer another key.

Rebuild the zip after editing: `python build_zip.py`
