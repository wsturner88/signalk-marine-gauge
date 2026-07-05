"""
Surface-mount POD housing for the Waveshare ESP32-S3-Touch-LCD-4 (V2/REV4) marine gauge.

Self-contained wall pod — no panel cutout. Sticks to a flat surface with 3M VHB tape
(or hangs on two screws via keyhole slots). Designed for the V-berth "boat status"
display near the bunk.

Two printed parts:
  A. pod_body  - one-piece rounded box: front bezel lip retains the glass, board drops
                 in from the rear. USB-C cable notch on the left wall (rear-open);
                 vent slots on top+bottom walls near the back edge (airflow + buzzer).
  B. pod_back  - flat back plate, screws into internal corner gussets. Smooth exterior
                 for VHB tape, 2 keyhole slots, 4 pillars preload the board standoffs.

Board dimensions from Waveshare's official STEP (see gauge_housing.py / README).
Housing frame: Z=0 at the pod's front face, +Z toward the viewer, origin at glass center.
When mounted with the terminal-block edge down, screen-up is -Y.

Requires: python3.12, `pip install build123d`.  Run: python pod_housing.py
"""

from build123d import (
    Box, Compound, Cylinder, Location, Polygon, Pos, RectangleRounded,
    export_step, export_stl, extrude, loft,
)
import os

# ----------------------------------------------------------------------------- params
BODY_W = 92.0           # pod outer square
BODY_R = 5.0            # outer corner radius (keeps >=3 mm corner wall vs the cavity)
BODY_D = 28.0           # pod depth: front face to wall
OPEN_W = 80.5           # bezel window (glass 84.2 sq; lip overlaps ~1.85/side)
LIP_T = 2.5             # lip in front of the glass
CAVITY_W = 84.8         # interior square (glass slab 84.2 + 0.3/side)
WALL_INNER = CAVITY_W / 2       # 42.4
WALL_OUTER = BODY_W / 2         # 46.0
BACK_SEAT = -24.0       # back plate front face (plate is 4 thick -> flush at -28)
BACK_T = 4.0

TERMINAL_NOTCH = False  # set True to open the bottom wall for the 12V terminal plug

# board landmarks (housing coords; from the Waveshare STEP, see gauge_housing.py)
GLASS_FRONT = -LIP_T
STANDOFF_XY = [(x, y) for x in (-33.25, 33.25) for y in (33.28, -35.22)]
USB_Y = (2.2, 19.2)     # usb-c notch span on the left (X-) wall
USB_Z_TOP = -8.5
TERM_X = (-22.35, 23.65)
TERM_Z_TOP = -6.5
BUZZER_XY = (-24.45, 12.9)

GUSSET_LEG = 10.0
LID_HOLE_XY = 39.9      # back-plate screws / gusset pilot bores
PILOT_D = 2.5           # M3 self-tapping pilot

PLATE_W = 84.4
PILLAR_H = 9.2          # BACK_SEAT (-24) up to 0.3 shy of the standoffs (-14.5)
KEYHOLE_X = 20.0        # two keyholes at (+-20, 5), slots pointing screen-up (-Y)

EXPORT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "export")
TO_BOARD = (0.0, -0.83, 5.7)    # housing -> board STEP coordinate frame


def cutbox(x0, x1, y0, y1, z0, z1):
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def corner_triangle(sx, sy, leg):
    cx, cy = sx * WALL_INNER, sy * WALL_INNER
    return Polygon(*[(cx, cy), (cx - sx * leg, cy), (cx, cy - sy * leg)], align=None)


# =============================================================== A. pod body
body = extrude(RectangleRounded(BODY_W, BODY_W, BODY_R), amount=-BODY_D)

# bezel window + interior cavity (open through the rear)
body -= extrude(RectangleRounded(OPEN_W, OPEN_W, 3.0), amount=-5.0) \
    .moved(Location((0, 0, 0.5)))
body -= cutbox(-CAVITY_W / 2, CAVITY_W / 2, -CAVITY_W / 2, CAVITY_W / 2,
               -BODY_D - 1, GLASS_FRONT)

# chamfer on the window's front edge
body -= loft([Pos(0, 0, 0.01) * RectangleRounded(83.5, 83.5, 4.0),
              Pos(0, 0, -1.3) * RectangleRounded(OPEN_W, OPEN_W, 3.0)])

# internal corner gussets (45-degree tapered starts -> no supports face-down)
for sx in (-1, 1):
    for sy in (-1, 1):
        body += loft([Pos(0, 0, -12.0) * corner_triangle(sx, sy, 2.0),
                      Pos(0, 0, -20.0) * corner_triangle(sx, sy, GUSSET_LEG)])
        body += extrude(corner_triangle(sx, sy, GUSSET_LEG), amount=-4.0) \
            .moved(Location((0, 0, -20.0)))
        body -= Pos(sx * LID_HOLE_XY, sy * LID_HOLE_XY, -19.0) * Cylinder(PILOT_D / 2, 10.0)

# USB-C cable notch, left wall, open to the rear edge (right-angle cable sits flush)
body -= cutbox(-WALL_OUTER - 1, -WALL_INNER + 0.6, USB_Y[0], USB_Y[1],
               -BODY_D - 1, USB_Z_TOP)

if TERMINAL_NOTCH:  # optional 12V terminal-plug notch, bottom wall
    body -= cutbox(TERM_X[0], TERM_X[1], WALL_INNER - 0.6, WALL_OUTER + 1,
                   -BODY_D - 1, TERM_Z_TOP)

# vent slots on top and bottom walls, tucked near the back edge (also buzzer outlet)
for y0, y1 in [(WALL_INNER - 0.6, WALL_OUTER + 1), (-WALL_OUTER - 1, -WALL_INNER + 0.6)]:
    for cx in (-20.0, 0.0, 20.0):
        body -= cutbox(cx - 7, cx + 7, y0, y1, -25.0, -21.0)

# =============================================================== B. back plate
# modeled exterior-face-down (z=0 is the wall/tape face) for a smooth first layer
plate = extrude(RectangleRounded(PLATE_W, PLATE_W, 2.0), amount=BACK_T)

for sx in (-1, 1):  # gusset screw holes + head counterbores on the wall face
    for sy in (-1, 1):
        plate -= Pos(sx * LID_HOLE_XY, sy * LID_HOLE_XY, BACK_T / 2) * Cylinder(3.4 / 2, BACK_T + 2)
        plate -= Pos(sx * LID_HOLE_XY, sy * LID_HOLE_XY, 1.1) * Cylinder(6.2 / 2, 2.2)

for kx in (-KEYHOLE_X, KEYHOLE_X):  # keyhole slots (screw-hang alternative to tape)
    plate -= Pos(kx, 5.0, BACK_T / 2) * Cylinder(8.0 / 2, BACK_T + 2)      # head pass-through
    plate -= cutbox(kx - 2.0, kx + 2.0, -7.0, 5.0, -1, BACK_T + 1)         # shank slide (screen-up)
    plate -= Pos(kx, -7.0, BACK_T / 2) * Cylinder(2.0, BACK_T + 2)         # rounded slot end

for px, py in STANDOFF_XY:  # pressure pillars against the board standoffs
    plate += Pos(px, py, BACK_T + PILLAR_H / 2) * Box(7, 7, PILLAR_H)

# =============================================================== export
os.makedirs(EXPORT_DIR, exist_ok=True)
for name, part in [("pod_body", body), ("pod_back", plate)]:
    export_step(part, os.path.join(EXPORT_DIR, f"{name}.step"))
    export_stl(part, os.path.join(EXPORT_DIR, f"{name}.stl"))
    bb = part.bounding_box()
    print(f"{name:10s} bbox ({bb.size.X:.1f} x {bb.size.Y:.1f} x {bb.size.Z:.1f}) mm")

# assembly in the board STEP's frame (plate exterior lands at housing z=-28)
assembly = Compound(children=[
    body.moved(Location(TO_BOARD)),
    plate.moved(Location((TO_BOARD[0], TO_BOARD[1], TO_BOARD[2] - BODY_D))),
])
export_step(assembly, os.path.join(EXPORT_DIR, "pod_assembly_boardframe.step"))
print("assembly   pod_assembly_boardframe.step (aligned to Waveshare board STEP)")
