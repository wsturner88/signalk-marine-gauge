"""
Flush panel-mount housing for the Waveshare ESP32-S3-Touch-LCD-4 (V2/REV4) marine gauge.

Three printed parts:
  A. bezel_can    - front bezel flange + through-panel can. Board drops in from the rear,
                    glass retained by the bezel lip.
  B. clamp_frame  - slides over the can from behind the panel; 4x M3 screws into the
                    can's side bosses pull it forward, clamping the panel.
  C. rear_lid     - vented lid, screws into internal corner gussets; 4 pillars press the
                    board's corner standoffs so nothing rattles.

All dimensions taken from Waveshare's official STEP (esp32-s3-touch-lcd-4_asm.stp) and
dimension drawing. Housing coordinate frame: Z=0 at bezel front face, +Z out of the
panel, origin centered on the glass. The combined assembly export is translated into the
board STEP's coordinate frame so both can be overlaid directly in CAD.

Requires: python3.12, `pip install build123d`.
Run:      python gauge_housing.py          -> writes STEP + STL into ./export/
"""

from build123d import (
    Box, Compound, Cylinder, Location, Polygon, Pos, Rectangle, RectangleRounded,
    export_step, export_stl, extrude, loft,
)
import os

# ----------------------------------------------------------------------------- params
PANEL_T = 12.0          # panel/bulkhead thickness the clamp frame is set up for (mm)

FLANGE_W = 106.0        # bezel flange square
FLANGE_T = 3.5          # flange thickness (also = how far bezel face sits off the panel)
FLANGE_R = 8.0          # flange corner radius
OPEN_W = 80.5           # bezel window (glass is 84.2 sq; lip overlaps ~1.85/side)
LIP_T = 2.5             # lip thickness in front of the glass (glass face recessed by this)

CAN_W = 89.8            # can outer square (panel cutout: 92 x 92, corner radius <= 5)
CAN_R = 2.5
CAVITY_W = 84.8         # interior square (glass slab 84.2 + 0.3/side)
CAN_END = -26.5         # can rear edge (from bezel face)
LID_SEAT = -24.0        # lid front face sits here, lid is flush with the can rear
WALL_INNER = CAVITY_W / 2       # 42.4
WALL_OUTER = CAN_W / 2          # 44.9

# board landmarks converted to housing coords (board STEP -> housing: y+0.83, z-5.7)
GLASS_FRONT = -LIP_T                      # -2.5
MODULE_CLEAR_Z = -11.9                    # full-width module needs clearance to here
STANDOFF_Z = -14.5                        # rear face of the 4 corner standoffs
STANDOFF_XY = [(x, y) for x in (-33.25, 33.25) for y in (33.28, -35.22)]
USB_Y = (2.2, 19.2)                       # usb-c notch span (shell at y 5.9..15.5)
USB_Z_TOP = -8.5                          # notch: from can rear edge up to here
TERM_X = (-22.35, 23.65)                  # terminal-plug notch span (block x -17.4..18.7)
TERM_Z_TOP = -6.5                         # block top at -7.0; plug slides in below this
BUZZER_XY = (-24.45, 12.9)                # buzzer center -> grille in the lid

# clamp bosses on the can's outer walls (half-round ribs w/ pilot bores).
# placed off-center where needed to dodge the USB / terminal notches.
BOSS_XY = [(47.4, 0.0), (-47.4, -10.0), (30.0, 47.4), (0.0, -47.4)]
BOSS_D = 9.0
BOSS_END = -(PANEL_T + FLANGE_T + 4.0 - 0.3)   # frame (4 thick) + 0.3 squeeze
PILOT_D = 2.5                                   # M3 self-tapping pilot

GUSSET_LEG = 10.0       # internal corner gussets carrying the lid screws
LID_HOLE_XY = 39.9      # lid screws / gusset pilot bores at (+-39.9, +-39.9)

FRAME_W = 104.0         # clamp frame outer square
FRAME_T = 4.0
FRAME_WIN = 90.6        # window sliding over the can

LID_W = 84.4
LID_T = 2.5
PILLAR_H = 9.2          # LID_SEAT (-24) up to 0.3 shy of the standoffs (-14.5)

EXPORT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "export")

# board-frame offset: housing solids -> board STEP coordinates
TO_BOARD = (0.0, -0.83, 5.7)


def cutbox(x0, x1, y0, y1, z0, z1):
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def corner_triangle(sx, sy, leg):
    """Right triangle filling the cavity corner (sx,sy = +-1)."""
    cx, cy = sx * WALL_INNER, sy * WALL_INNER
    pts = [(cx, cy), (cx - sx * leg, cy), (cx, cy - sy * leg)]
    return Polygon(*pts, align=None)


# =============================================================== A. bezel + can
flange = extrude(RectangleRounded(FLANGE_W, FLANGE_W, FLANGE_R), amount=-FLANGE_T)
can = extrude(RectangleRounded(CAN_W, CAN_W, CAN_R), amount=CAN_END)
part_a = flange + can

# clamp bosses (ribs) on the outer walls, flange back down to the frame seat
BOSS_LEN = -FLANGE_T - BOSS_END          # e.g. 15.7 for PANEL_T=12
for bx, by in BOSS_XY:
    part_a += Pos(bx, by, (-FLANGE_T + BOSS_END) / 2) * Cylinder(BOSS_D / 2, BOSS_LEN)

# bezel window + interior cavity (open through the rear)
part_a -= extrude(RectangleRounded(OPEN_W, OPEN_W, 3.0), amount=-(FLANGE_T + 1)) \
    .moved(Location((0, 0, 0.5)))
part_a -= cutbox(-CAVITY_W / 2, CAVITY_W / 2, -CAVITY_W / 2, CAVITY_W / 2,
                 CAN_END - 1, GLASS_FRONT)

# chamfer on the window's front edge (subtract a lofted wedge)
part_a -= loft([Pos(0, 0, 0.01) * RectangleRounded(83.5, 83.5, 4.0),
                Pos(0, 0, -1.3) * RectangleRounded(OPEN_W, OPEN_W, 3.0)])

# internal corner gussets for the lid screws (45-degree tapered start so they print
# without support when the bezel face is on the bed)
for sx in (-1, 1):
    for sy in (-1, 1):
        part_a += loft([Pos(0, 0, -12.0) * corner_triangle(sx, sy, 2.0),
                        Pos(0, 0, -20.0) * corner_triangle(sx, sy, GUSSET_LEG)])
        part_a += extrude(corner_triangle(sx, sy, GUSSET_LEG), amount=-4.0) \
            .moved(Location((0, 0, -20.0)))
        part_a -= Pos(sx * LID_HOLE_XY, sy * LID_HOLE_XY, -19.0) * Cylinder(PILOT_D / 2, 10.0)

# cable notches, open to the can's rear edge
part_a -= cutbox(-WALL_OUTER - 1, -WALL_INNER + 0.6, USB_Y[0], USB_Y[1],
                 CAN_END - 1, USB_Z_TOP)                                  # USB-C, left wall
part_a -= cutbox(TERM_X[0], TERM_X[1], WALL_INNER - 0.6, WALL_OUTER + 1,
                 CAN_END - 1, TERM_Z_TOP)                                 # terminal, bottom wall

# pilot bores in the boss ends
for bx, by in BOSS_XY:
    part_a -= Pos(bx, by, BOSS_END + 5.0) * Cylinder(PILOT_D / 2, 10.0)

# =============================================================== B. clamp frame
part_b = extrude(RectangleRounded(FRAME_W, FRAME_W, FLANGE_R), amount=FRAME_T)
part_b -= extrude(RectangleRounded(FRAME_WIN, FRAME_WIN, 3.0), amount=FRAME_T + 2) \
    .moved(Location((0, 0, -1)))
for bx, by in BOSS_XY:
    part_b -= Pos(bx, by, FRAME_T / 2) * Cylinder(3.4 / 2, FRAME_T + 2)

# =============================================================== C. rear lid
part_c = extrude(RectangleRounded(LID_W, LID_W, 2.0), amount=LID_T)
for sx in (-1, 1):                                            # lid screw holes
    for sy in (-1, 1):
        part_c -= Pos(sx * LID_HOLE_XY, sy * LID_HOLE_XY, LID_T / 2) * Cylinder(3.4 / 2, LID_T + 2)
for px, py in STANDOFF_XY:                                    # pressure pillars
    part_c += Pos(px, py, LID_T + PILLAR_H / 2) * Box(7, 7, PILLAR_H)
for i, y in enumerate((-27, -18, -9, 0, 9, 18, 27)):          # vent slots
    part_c -= cutbox(-18, 18, y - 1.25, y + 1.25, -1, LID_T + 1)
bx, by = BUZZER_XY                                            # buzzer grille
for dx, dy in ((0, 0), (4, 0), (-4, 0), (0, 4), (0, -4)):
    part_c -= Pos(bx + dx, by + dy, LID_T / 2) * Cylinder(1.5, LID_T + 2)

# =============================================================== export
os.makedirs(EXPORT_DIR, exist_ok=True)

for name, part in [("bezel_can", part_a), ("clamp_frame", part_b), ("rear_lid", part_c)]:
    export_step(part, os.path.join(EXPORT_DIR, f"{name}.step"))
    export_stl(part, os.path.join(EXPORT_DIR, f"{name}.stl"))
    bb = part.bounding_box()
    print(f"{name:12s} bbox ({bb.size.X:.1f} x {bb.size.Y:.1f} x {bb.size.Z:.1f}) mm")

# assembly in the BOARD STEP's coordinate frame (overlay with Waveshare's model in CAD)
frame_placed = part_b.moved(Location((0, 0, -FLANGE_T - PANEL_T - FRAME_T)))
lid_placed = part_c.moved(Location((0, 0, CAN_END)))
assembly = Compound(children=[
    part_a.moved(Location(TO_BOARD)),
    frame_placed.moved(Location(TO_BOARD)),
    lid_placed.moved(Location(TO_BOARD)),
])
export_step(assembly, os.path.join(EXPORT_DIR, "housing_assembly_boardframe.step"))
print("assembly     housing_assembly_boardframe.step (aligned to Waveshare board STEP)")
print(f"panel thickness configured: {PANEL_T} mm | panel cutout: 92 x 92 mm square")
