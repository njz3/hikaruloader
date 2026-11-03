# Hikaruloader (formerly Valkyrie)
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Released under the GNU General Public License, version 3 or (at your
# option) any later version; see LICENSE.

"""The README's comic strips of Star Wars: Racer Arcade's levers
(TWIN_LEVERS = COMBINED): a player with a wheel and a pedal, or with a
gamepad, a case a panel, the two thrust levers the game gets and where the
pod goes. Needs matplotlib.

Usage: python tools/levers_diagram.py [OUT_DIR]   (docs/media by default:
swracer-levers.png, swracer-levers-gamepad.png)"""
import logging
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Arc, Circle, Ellipse, FancyArrowPatch, FancyBboxPatch, Polygon, Rectangle

logging.getLogger("matplotlib.font_manager").setLevel(logging.ERROR)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "media")

INK = "#1f2933"
SOFT = "#52606d"
BLUE = "#2f6fb3"
ORANGE = "#d9822b"
GREEN = "#2e8b57"
METAL = "#cfd8e3"
PAD = "#3e4c59"
LEFT_C = "#7b5ea7"		# the left lever and its engine
RIGHT_C = "#2a9d8f"		# the right ones
SHIRT = "#e07a5f"
PANTS = "#3d405b"
HAIR = "#4a3728"
SKIN = "#f6dcc2"
CAPTION = "#fff6d5"
FONT = ["Comic Sans MS", "Comic Neue", "Humor Sans", "DejaVu Sans"]

W, H = 47.0, 49.0		# a panel
ax = None
OX = OY = 0.0			# the panel's corner
CX, FY, K = 11.0, 6.0, 0.55	# the player's middle, floor and scale in the panel


def P(x, y):
    """A point of the panel"""
    return (OX + x, OY + y)


def C(x, y):
    """A point of the player's"""
    return P(CX + K * x, FY + K * y)


def text(x, y, s, size=11, color=INK, weight="normal", ha="center", va="center"):
    ax.text(OX + x, OY + y, s, fontsize=size, color=color, fontweight=weight, ha=ha, va=va, zorder=30,
            linespacing=1.15)


def line(points, color=INK, lw=3.0, z=4, ls="-"):
    xs, ys = zip(*points)
    ax.plot(xs, ys, color=color, lw=lw, ls=ls, solid_capstyle="round", solid_joinstyle="round", zorder=z)


def arrow(p0, p1, color=SOFT, lw=2.0, ms=16, rad=0.0, z=8):
    ax.add_patch(FancyArrowPatch(p0, p1, arrowstyle="-|>", mutation_scale=ms, color=color, lw=lw, shrinkA=0,
                                 shrinkB=0, connectionstyle="arc3,rad=%g" % rad, zorder=z))


def head(mood):
    """The player's head, facing us"""
    ax.add_patch(Circle(C(0, 44), 5.5 * K, fc=SKIN, ec=INK, lw=2.5, zorder=12))
    hair = [C(5.7 * math.cos(math.radians(a)), 44 + 5.7 * math.sin(math.radians(a))) for a in range(5, 180, 10)]
    hair += [C(-5.6, 45.0), C(-3.2, 46.9), C(-1.0, 45.7), C(1.4, 47.1), C(3.6, 45.9), C(5.6, 45.2)]
    ax.add_patch(Polygon(hair, closed=True, fc=HAIR, ec=INK, lw=1.5, zorder=13))
    for x in (-1.9, 1.9):
        ax.add_patch(Circle(C(x, 43.4), 0.6 * K, fc=INK, ec=INK, zorder=14))
    if mood == "grin":
        ax.add_patch(Polygon([C(2.4 * math.cos(math.radians(a)), 41.6 + 2.0 * math.sin(math.radians(a)))
                              for a in range(180, 361, 15)], closed=True, fc="#8c2f39", ec=INK, lw=1.5, zorder=14))
    elif mood == "smile":
        ax.add_patch(Arc(C(0, 41.9), 4.2 * K, 2.8 * K, theta1=200, theta2=340, color=INK, lw=2, zorder=14))
    elif mood == "o":
        ax.add_patch(Circle(C(0, 40.8), 0.9 * K, fc=INK, ec=INK, zorder=14))
    elif mood == "teeth":
        ax.add_patch(Rectangle(C(-1.9, 40.0), 3.8 * K, 1.7 * K, fc="white", ec=INK, lw=1.5, zorder=14))
        line([C(-1.9, 40.85), C(1.9, 40.85)], lw=1, z=15)
    else:	# worried
        ax.add_patch(Arc(C(0, 39.8), 3.8 * K, 2.4 * K, theta1=25, theta2=155, color=INK, lw=2, zorder=14))


def sweat(x, y):
    ax.add_patch(Polygon([C(x, y + 2.2), C(x - 0.9, y + 0.2), C(x + 0.9, y + 0.2)], closed=True, fc="#9ccfe8",
                         ec="#3a7ca5", lw=1, zorder=15))
    ax.add_patch(Circle(C(x, y), 0.95 * K, fc="#9ccfe8", ec="#3a7ca5", lw=1, zorder=15))


def body():
    ax.add_patch(Polygon([C(-4.6, 38.6), C(4.6, 38.6), C(3.9, 19.0), C(-3.9, 19.0)], closed=True, fc=SHIRT, ec=INK,
                         lw=2, zorder=6))
    line([C(-2.0, 19.5), C(-2.6, 10.0), C(-3.2, 1.6)], color=PANTS, lw=5, z=5)
    ax.add_patch(Ellipse(C(-4.2, 0.9), 4.2 * K, 1.8 * K, fc=INK, ec=INK, zorder=6))
    line([C(-9.0, 0.0), C(15.0, 0.0)], color=SOFT, lw=1.5, z=1)


def right_leg(pedal):
    """His right foot on the pedal, pedal 0 (released) to 1 (floored), or
    standing"""
    if pedal is None:
        line([C(2.0, 19.5), C(2.6, 10.0), C(3.2, 1.6)], color=PANTS, lw=5, z=5)
        ax.add_patch(Ellipse(C(4.2, 0.9), 4.2 * K, 1.8 * K, fc=INK, ec=INK, zorder=6))
        return
    a = math.radians(6 + 34 * (1 - pedal))
    u = (math.cos(a), math.sin(a))
    n = (-u[1], u[0])
    px, py = 5.5, 0.6
    line([C(px, py), C(px + 10 * u[0], py + 10 * u[1])], color=BLUE, lw=8, z=7)
    ax.add_patch(Circle(C(px, py), 1.0 * K, fc=INK, ec=INK, zorder=8))
    shoe = (px + 4.5 * u[0] + 1.4 * n[0], py + 4.5 * u[1] + 1.4 * n[1])
    ankle = (shoe[0] - 1.3 * u[0] + 1.0 * n[0], shoe[1] - 1.3 * u[1] + 1.0 * n[1])
    line([C(2.0, 19.5), C(5.4, 10.5), C(*ankle)], color=PANTS, lw=5, z=5)
    ax.add_patch(Ellipse(C(*shoe), 4.6 * K, 1.9 * K, angle=math.degrees(a), fc=INK, ec=INK, zorder=9))


def arms(hands, z=11):
    for side, h in ((-1, hands[0]), (1, hands[1])):
        sh = (4.2 * side, 36.8)
        el = ((sh[0] + h[0]) / 2 + 2.6 * side, (sh[1] + h[1]) / 2 - 0.8)
        line([C(*sh), C(*el), C(*h)], lw=3.5, z=z)
        ax.add_patch(Circle(C(*h), 1.2 * K, fc=SKIN, ec=INK, lw=1.5, zorder=z + 5))


def wheel(s):
    """The wheel in his hands, s -1 (full left) to +1 (full right)"""
    turn = math.radians(-90 * s)
    cx, cy, r = 0.0, 29.0, 7.2
    ax.add_patch(Circle(C(cx, cy), r * K, fc="white", ec=ORANGE, lw=5, zorder=9))
    for a in (90, 210, 330):
        t = math.radians(a) + turn
        line([C(cx, cy), C(cx + r * math.cos(t), cy + r * math.sin(t))], color=ORANGE, lw=3, z=10)
    ax.add_patch(Circle(C(cx, cy), 1.3 * K, fc=ORANGE, ec=ORANGE, zorder=10))
    arms([(cx + r * math.cos(math.radians(a) + turn), cy + r * math.sin(math.radians(a) + turn)) for a in (150, 30)])


def gamepad(s, t):
    """The gamepad in his hands: its left stick s, its right trigger t"""
    gx, gy = 0.0, 25.0

    def G(x, y):
        return C(gx + x, gy + y)

    def box(x, y, w, h, rounding, **kw):
        ax.add_patch(FancyBboxPatch(G(x, y), w * K, h * K,
                                    boxstyle="round,pad=%g,rounding_size=%g" % (0.3 * K, rounding * K), **kw))

    # The triggers on top, the right one pulled
    box(6.0, 8.6 - 2.0 * t, 7.0, 3.2, 1.2, fc=BLUE if t > 0 else METAL, ec=INK, lw=1.5, zorder=8)
    box(-13.0, 8.6, 7.0, 3.2, 1.2, fc=METAL, ec=INK, lw=1.2, zorder=8)
    if t > 0:
        arrow(G(9.5, 17.0), G(9.5, 12.0 - 2.0 * t), color=BLUE, lw=3, ms=18, z=9)
    for x in (-9.5, 9.5):
        ax.add_patch(Ellipse(G(x, -5.0), 9.0 * K, 14.0 * K, angle=-18 if x > 0 else 18, fc=PAD, ec=INK, lw=1.5,
                             zorder=9))
    box(-14.0, -4.0, 28.0, 12.0, 4.5, fc=PAD, ec=INK, lw=1.5, zorder=10)
    # The left stick, pushed
    sx, sy = -7.5, 2.5
    ax.add_patch(Circle(G(sx, sy), 3.8 * K, fc="#2a333d", ec="#9aa5b1", lw=1.5, zorder=11))
    ax.add_patch(Circle(G(sx + 2.6 * s, sy), 2.2 * K, fc=ORANGE if s else "#9aa5b1", ec=INK, lw=1.5, zorder=12))
    # The buttons, the right stick and the pad
    for (bx, by), col in (((7.5, 5.2), "#e3b23c"), ((5.2, 2.9), "#3c7fe3"), ((9.8, 2.9), "#d64545"),
                          ((7.5, 0.6), "#3fa34d")):
        ax.add_patch(Circle(G(bx, by), 1.1 * K, fc=col, ec=INK, lw=1, zorder=11))
    ax.add_patch(Circle(G(3.5, -2.0), 2.4 * K, fc="#2a333d", ec="#9aa5b1", lw=1.2, zorder=11))
    for dx, dy in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        ax.add_patch(Rectangle(G(-3.0 + 1.3 * dx - 0.6, -2.0 + 1.3 * dy - 0.6), 1.2 * K, 1.2 * K, fc="#2a333d",
                               ec="#9aa5b1", lw=0.8, zorder=11))
    arms([(gx - 10.5, gy - 6.5), (gx + 10.5, gy - 6.5)], z=7)


def bubble(x, y, w, h, s, tail, size=11):
    ax.add_patch(Polygon([P(x - w * 0.12, y - h * 0.3), P(x + w * 0.04, y - h * 0.32), P(*tail)], closed=True,
                         fc="white", ec=INK, lw=1.5, zorder=17))
    ax.add_patch(Ellipse(P(x, y), w, h, fc="white", ec=INK, lw=1.5, zorder=18))
    text(x, y, s, size=size, weight="bold")


def caption(s, y=42.0, h=6.0):
    ax.add_patch(Rectangle(P(1.0, y), W - 2.0, h, fc=CAPTION, ec=INK, lw=1.5, zorder=19))
    text(2.2, y + h / 2, s, size=12, ha="left")


def levers(left, right, values=True):
    """The cabinet's two levers, 0 at rest to 1 pushed all the way"""
    for x, v, label, col in ((25.5, left, "L", LEFT_C), (30.5, right, "R", RIGHT_C)):
        ax.add_patch(Rectangle(P(x - 0.6, 13.0), 1.2, 16.0, fc="#e4e8ee", ec=SOFT, lw=1.2, zorder=5))
        y = 13.0 + 16.0 * v
        ax.add_patch(FancyBboxPatch(P(x - 1.8, y - 1.0), 3.6, 2.0, boxstyle="round,pad=0.15,rounding_size=0.6",
                                    fc=col, ec=INK, lw=1.2, zorder=6))
        text(x, 10.6, label, size=13, weight="bold", color=col)
        if values:
            text(x, 31.6, "%d%%" % round(v * 100), size=10, weight="bold", color=col)


def pod(left, right, turn):
    """The pod seen from above, flying up the page: its engines' flames, and
    where it goes (turn 0 straight, +1 right, -1 left, -2 sharp left, None
    nowhere)"""
    px, py = 40.0, 19.0
    for x, thrust, col in ((px - 3.3, left, LEFT_C), (px + 3.3, right, RIGHT_C)):
        if thrust > 0:
            ax.add_patch(Polygon([P(x - 0.8, py - 3.2), P(x + 0.8, py - 3.2), P(x, py - 3.2 - 7.0 * thrust)],
                                 closed=True, fc=ORANGE, ec="#b8601a", lw=1, zorder=5))
        ax.add_patch(FancyBboxPatch(P(x - 1.1, py - 3.2), 2.2, 6.4, boxstyle="round,pad=0.15,rounding_size=0.9",
                                    fc=col, ec=INK, lw=1.2, zorder=6))
    line([P(px - 3.3, py + 0.5), P(px, py - 4.5), P(px + 3.3, py + 0.5)], color=SOFT, lw=1, z=4)
    ax.add_patch(FancyBboxPatch(P(px - 1.2, py - 6.8), 2.4, 2.8, boxstyle="round,pad=0.15,rounding_size=0.8",
                                fc=METAL, ec=INK, lw=1.2, zorder=6))
    if turn is None:
        return
    if turn == 0:
        arrow(P(px, py + 4.0), P(px, py + 5.0 + 6.0 * (left + right) / 2), color=GREEN, lw=3, ms=20)
    elif turn == 1:
        arrow(P(px, py + 4.0), P(px + 5.5, py + 10.0), color=GREEN, lw=3, ms=20, rad=-0.35)
    elif turn == -1:
        arrow(P(px, py + 4.0), P(px - 5.5, py + 10.0), color=GREEN, lw=3, ms=20, rad=0.35)
    else:
        arrow(P(px + 0.5, py + 4.0), P(px - 6.0, py + 5.5), color=GREEN, lw=3, ms=20, rad=0.9)


def panel(col, row, draw):
    global OX, OY
    OX, OY = 2.0 + col * (W + 2.5), 2.0 + (1 - row) * (H + 2.0)
    draw()
    ax.add_patch(Rectangle(P(0, 0), W, H, fc="none", ec=INK, lw=3, zorder=40))


def strip(pad, out):
    """The strip: the wheel and pedal's, or the gamepad's"""
    global ax
    pedal_word, turn_word = ("TRIGGER", "STICK") if pad else ("PEDAL", "WHEEL")
    floored, half, released = (("PULLED", "HALF WAY", "RELEASED") if pad else ("FLOORED", "HALF WAY", "RELEASED"))
    centred = "CENTRED" if pad else "STRAIGHT"

    def player(t, s, mood):
        body()
        right_leg(None if pad else t)
        if pad:
            gamepad(s, t)
        else:
            wheel(s)
        head(mood)

    def first():
        caption("IN THE CABINET: A THRUST LEVER\nFOR EACH OF THE POD'S ENGINES")
        player(0.0, 0.0, "o")
        text(23.0 if pad else 18.5, 31.0, "?", size=20, weight="bold", color=SOFT)
        bubble(12.0, 37.6, 22.0, 6.4, "BUT I'VE ONLY\nGOT A GAMEPAD!" if pad else "BUT I'VE ONLY GOT A\nWHEEL AND A PEDAL!",
               (12.8, 33.4), size=10)
        levers(0.0, 0.0, values=False)
        pod(0.0, 0.0, None)
        ax.add_patch(Rectangle(P(1.0, 0.8), W - 2.0, 4.0, fc=CAPTION, ec=INK, lw=1.5, zorder=19))
        text(2.2, 2.8, "SO HIKARULOADER PUSHES THEM FOR YOU...", size=10, ha="left")

    def case(t, s, left, right, turn, mood, say, what, how):
        def draw():
            caption(how)
            player(t, s, mood)
            if mood == "worried":
                sweat(7.0, 45.5)
            bubble(12.0, 37.6, 20.0, 6.4, say, (12.8, 33.4))
            levers(left, right)
            pod(left, right, turn)
            text(W / 2, 3.4, what, size=10, weight="bold", color=GREEN)
        return draw

    panels = [
        first,
        case(1.0, 0.0, 1.0, 1.0, 0, "grin", "FULL SPEED!", "BOTH LEVERS PUSHED: STRAIGHT AHEAD",
             "%s %s,\n%s %s" % (pedal_word, floored, turn_word, centred)),
        case(0.5, 0.0, 0.5, 0.5, 0, "smile", "CRUISING...", "BOTH HALF WAY: STRAIGHT, SLOWER",
             "%s %s,\n%s %s" % (pedal_word, half, turn_word, centred)),
        case(1.0, 0.5, 1.0, 0.5, 1, "teeth", "RIGHT!", "LEFT ENGINE PUSHES MORE: TURNS RIGHT",
             "%s %s,\n%s A BIT TO THE RIGHT" % (pedal_word, floored, turn_word)),
        case(0.5, -1.0, 0.0, 1.0, -2, "worried", "HARD LEFT!!", "RIGHT ENGINE ALONE: SHARP LEFT",
             "%s %s,\n%s ALL THE WAY LEFT" % (pedal_word, half, turn_word)),
        case(0.0, 0.6, 0.0, 0.0, None, "worried", "HEY! WHY DID\nIT STOP?", "NO THRUST AT ALL: THE POD STOPS",
             "%s %s,\n%s ANYWHERE" % (pedal_word, released, turn_word)),
    ]

    with plt.xkcd(scale=0.8, length=120, randomness=1.5):
        plt.rcParams["font.family"] = FONT
        fig = plt.figure(figsize=(15, 11.2), dpi=120)
        ax = fig.add_axes([0, 0, 1, 1])
        ax.set_xlim(0, 150)
        ax.set_ylim(0, 112)
        ax.set_aspect("equal")
        ax.axis("off")
        fig.patch.set_facecolor("white")
        ax.text(2.0, 107.0, "STAR WARS: RACER ARCADE - TWO LEVERS, ONE %s" % ("GAMEPAD" if pad else "WHEEL"),
                fontsize=21, fontweight="bold", color=INK, ha="left", va="center")
        ax.text(148.0, 107.0, "hikaruloader  -  Levers: Combined", fontsize=11, color=SOFT, ha="right",
                va="center")
        for i, draw in enumerate(panels):
            panel(i % 3, i // 3, draw)
        fig.savefig(out, dpi=120, facecolor="white")
        plt.close(fig)
    print("written", out)


strip(False, os.path.join(OUT_DIR, "swracer-levers.png"))
strip(True, os.path.join(OUT_DIR, "swracer-levers-gamepad.png"))
