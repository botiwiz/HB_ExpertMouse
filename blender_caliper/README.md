# HB_Caliper for Blender

Measurements that **stay visible in every mode** (Object, Edit, Pose, paint modes…),
with draggable handles, **Ctrl snapping** and **X/Y/Z axis locking**.

![preview](preview.png)

## Install (Blender 4.2+)
Edit > Preferences > Get Extensions > ⌄ (top right) > **Install from Disk…** > `hb_caliper.zip`
(If you installed the older "Caliper Measure" version, uninstall it first.).

## Use
Click the **HB_Caliper** button at the bottom of the toolbar (T panel) — available in Object, Edit, Pose and paint modes — or Sidebar (N) > **HB_Caliper** > *HB_Caliper Tool*.

| Action | Result |
|---|---|
| Drag from anywhere | New measurement. Like Blender's Measure tool, both ends follow the surface under the mouse (vertex/edge/face → vertex/edge/face) |
| Drag a handle | Move that end |
| Drag the line | Move the whole measurement |
| Hold **Ctrl** while dragging (or when pressing) | Snap to a vertex, any point along an edge, an edge midpoint, an object origin, the 3D cursor or another handle; otherwise the surface. Hidden geometry is ignored unless X-ray is on |
| **X / Y / Z** while dragging | Lock to that axis (press again to unlock). With Ctrl, the handle slides along the axis to line up with the snap target |
| **Shift+X/Y/Z** while dragging | Lock to the plane (moves in the other two axes) |
| Right-click / Esc | Cancel the drag |
| Del / Backspace over a measurement | Delete it |

The sidebar panel lists all measurements (rename, hide, delete, swap ends,
type exact start/end coordinates), has **Measure Selection** (2 selected vertices
in Edit Mode, or 2 objects), and display options: unit (scene, mm, cm, m, in, ft),
decimals, color, ruler ticks, X/Y/Z deltas, snap radius.

Measurements are saved in the .blend file (per scene) and support undo.

Rebuild the zip after editing: `python build_zip.py` (toolbar icon: `python make_icon.py`)
