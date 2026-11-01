# Builds the tool icons: each is drawn as a fully constrained sketch on a 24 x 24 grid, saved as
# an editable source (src/<name>.dsk) and exported as SVG (<name>.svg).
#
#   drawapp-cli icons/build_icons.py            (all of them)
#   drawapp-cli icons/build_icons.py tangent    (just some)
#
# Style (after Fusion's, simplified, with thicker lines so they read when small): what the tool
# acts on is RED, what it acts against is GREY. The app swaps the colours for other states (e.g.
# white on the green "active" button), so keep to these two.
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RED = "#e8615c"
GREY = "#d5d9de"
W = 2.5  # line width, in grid units (thick, so they read at 16 px)


def start():
    """A new sketch with the 24 x 24 canvas as a construction frame on the origin."""
    doc.new()
    frame = s.rectangle(s.origin, (24, 24))
    for side in frame:
        side.construction = True
    s.distance(frame[0], value=24)
    s.distance(frame[3], value=24)


def pin(p, x, y):
    """Dimension a point's position from the origin."""
    s.horizontal_distance(s.origin, p, value=x)
    s.vertical_distance(s.origin, p, value=y)


def stroke(curves, color, width=W, dash="solid"):
    s.stroke(curves, color=color, width=width, dash=dash)
    return curves


def line(a, b, color, width=W, dash="solid"):
    l = s.line(a, b)
    stroke(l, color, width, dash)
    return l


def finish(name):
    r = s.solve()
    problems = []
    if r.dof != 0:
        problems.append(f"{r.dof} degrees of freedom left")
    if r.redundant:
        problems.append(f"redundant: {r.redundant}")
    if r.conflicting:
        problems.append(f"conflicting: {r.conflicting}")
    if not r.ok:
        problems.append("didn't solve")
    os.makedirs(os.path.join(HERE, "src"), exist_ok=True)
    doc.save(os.path.join(HERE, "src", name + ".dsk"))
    doc.export_svg(os.path.join(HERE, name + ".svg"), undecorated=False, canvas=(0, 0, 24, 24))
    print(f"{name:20} {'ok' if not problems else 'PROBLEM: ' + '; '.join(problems)}")
    return not problems


# --- constraints ------------------------------------------------------------------------------


def horizontal_vertical():
    """An upright on a level base."""
    start()
    base = line((3, 4.5), (21, 4.5), RED)
    s.horizontal(base)
    pin(base.p1, 3, 4.5)
    s.distance(base, value=18)
    stem = line((12, 4.6), (12, 21.5), RED)
    s.midpoint(stem.p1, base)
    s.vertical(stem)
    s.distance(stem, value=17)
    return finish("horizontal_vertical")


def coincident():
    """Two lines meeting at a red point."""
    start()
    dot = s.rectangle((2.5, 2.5), (7.5, 7.5))  # the point, as a little filled square
    stroke(dot, RED, width=1)
    pin(dot[0].p1, 2.5, 2.5)
    s.distance(dot[0], value=5)
    s.distance(dot[1], value=5)
    s.fill(at=(5, 5), color=RED)
    upright = line((5, 7.6), (5, 21.5), RED)
    s.midpoint(upright.p1, dot[2])  # from the middle of the square's top
    s.vertical(upright)
    s.distance(upright, value=14)
    level = line((7.6, 5), (21.5, 5), GREY)
    s.midpoint(level.p1, dot[1])  # from the middle of its right side
    s.horizontal(level)
    s.distance(level, value=14)
    return finish("coincident")


def tangent():
    """A red circle touched by a grey line."""
    start()
    c = s.circle((9.5, 9.5), 7)
    stroke(c, RED)
    pin(c.center, 9.5, 9.5)
    s.radius(c, value=7)
    touch = line((7.73, 21.17), (21.17, 7.73), GREY)
    s.tangent(touch, c)
    s.angle(touch, s.x_axis, value=45)
    s.distance(touch, value=19)
    s.horizontal_distance(s.origin, touch.p1, value=7.78)  # centred on where it touches
    return finish("tangent")


def equal():
    """Two equal bars, one red."""
    start()
    top = line((3.5, 16), (20.5, 16), RED, width=3.5)
    bottom = line((3.5, 8), (20.5, 8), GREY, width=3.5)
    s.horizontal(top)
    s.horizontal(bottom)
    s.equal(top, bottom)
    s.distance(top, value=17)
    pin(top.p1, 3.5, 16)
    pin(bottom.p1, 3.5, 8)
    return finish("equal")


def parallel():
    """Two parallel slanted lines, one red."""
    start()
    a = line((3.5, 6), (13.25, 19.93), RED)
    b = line((10.5, 3), (20.25, 16.93), GREY)
    s.angle(a, s.x_axis, value=55)
    s.distance(a, value=17)
    pin(a.p1, 3.5, 6)
    s.parallel(a, b)
    s.equal(a, b)
    pin(b.p1, 10.5, 3)
    return finish("parallel")


def perpendicular():
    """A red line standing square on the middle of a grey one."""
    start()
    grey = line((3, 3), (21, 21), GREY)
    pin(grey.p1, 3, 3)
    s.angle(grey, s.x_axis, value=45)
    s.distance(grey, value=25.46)
    red = line((12.05, 12.05), (5, 19), RED)
    s.midpoint(red.p1, grey)
    s.perpendicular(red, grey)
    s.distance(red, value=9.9)
    return finish("perpendicular")


def fix():
    """A padlock."""
    start()
    body = s.rectangle((4, 2.5), (20, 13.5))
    stroke(body, RED)
    pin(body[0].p1, 4, 2.5)
    s.distance(body[0], value=16)
    s.distance(body[1], value=11)
    s.fill(at=(12, 5), color=RED + "66")  # see-through, so the outline reads
    top = body[2]
    left = line((7.5, 13.5), (7.5, 17), RED)
    right = line((16.5, 13.5), (16.5, 17), RED)
    for leg in (left, right):
        s.coincident(leg.p1, top)
        s.vertical(leg)
    s.horizontal_distance(body[0].p1, left.p1, value=3.5)
    s.horizontal_distance(left.p1, right.p1, value=9)
    s.distance(left, value=3.5)
    s.distance(right, value=3.5)
    shackle = s.arc((12, 17), right.p2, left.p2)  # over the top, right to left
    stroke(shackle, RED)
    # Its centre on the line between the legs' tops; the arc itself keeps it equidistant, so
    # that's halfway (a radius dimension as well would say the same thing twice).
    chord = s.line(left.p2, right.p2)
    chord.construction = True
    s.coincident(shackle.center, chord)
    keyhole = line((12, 5.5), (12, 9.5), RED)
    s.vertical(keyhole)
    s.distance(keyhole, value=4)
    pin(keyhole.p1, 12, 5.5)
    return finish("fix")


def midpoint():
    """Fusion's midpoint triangle."""
    start()
    centre = s.point(12, 9.75)
    pin(centre, 12, 9.75)
    sides = s.polygon(centre, (12, 20.25), sides=3)
    stroke(sides, RED)
    s.horizontal(sides[1])  # the base (sides run from the top vertex)
    guide = [c for c in s.circles if c.construction][0]
    s.radius(guide, value=10.5)
    return finish("midpoint")


def concentric():
    """Two circles on one centre: the outer red."""
    start()
    outer = s.circle((12, 12), 9.5)
    inner = s.circle((12, 12.01), 4.5)
    stroke(outer, RED)
    stroke(inner, GREY)
    pin(outer.center, 12, 12)
    s.radius(outer, value=9.5)
    s.radius(inner, value=4.5)
    s.concentric(outer, inner)
    return finish("concentric")


def collinear():
    """Two segments along one line: grey, then red."""
    start()
    grey = line((3, 3), (10.5, 10.5), GREY)
    red = line((13.5, 13.5), (21, 21), RED)
    s.collinear(grey, red)
    s.equal(grey, red)
    s.angle(grey, s.x_axis, value=45)
    s.distance(grey, value=10.61)
    pin(grey.p1, 3, 3)
    s.distance(grey.p2, red.p1, value=4.24)
    return finish("collinear")


def symmetric():
    """A red bracket mirrored into a grey one across a dashed line."""
    start()
    axis = line((12, 1.5), (12, 22.5), RED, width=2, dash="dashed")
    s.vertical(axis)
    pin(axis.p1, 12, 1.5)
    s.distance(axis, value=21)
    top = line((8, 21), (3.5, 21), RED)
    side = line(top.p2, (3.5, 3), RED)
    bottom = line(side.p2, (8, 3), RED)
    s.horizontal(top)
    s.horizontal(bottom)
    s.vertical(side)
    pin(top.p2, 3.5, 21)
    s.distance(side, value=18)
    s.distance(top, value=4.5)
    s.distance(bottom, value=4.5)
    stroke(s.mirror([top, side, bottom], axis), GREY)
    return finish("symmetric")


def arrowhead(tip, along, length, width):
    """A filled arrowhead at `tip` pointing back along `along` (a line through the tip)."""
    x, y = tip.pos
    sign = 1 if along.p1 == tip else -1  # which way the arrow sits along the line
    dx = sign * length
    b1 = s.point(x + dx, y + width / 2)
    b2 = s.point(x + dx, y - width / 2)
    edges = [line(tip, b1, RED, width=1), line(b1, b2, RED, width=1), line(b2, tip, RED, width=1)]
    s.symmetric(b1, b2, along)
    s.distance(b1, b2, value=width)
    s.distance(tip, edges[1], value=length)
    s.fill(at=(x + dx / 2, y), color=RED)
    return edges


def dimension():
    """A dimension: extension lines with an arrowed line between."""
    start()
    left = line((3, 4), (3, 20), GREY)
    right = line((21, 4), (21, 20), GREY)
    for ext, x in ((left, 3), (right, 21)):
        s.vertical(ext)
        pin(ext.p1, x, 4)
        s.distance(ext, value=16)
    dim = line((3, 12), (21, 12), RED)
    s.midpoint(dim.p1, left)
    s.midpoint(dim.p2, right)
    arrowhead(dim.p1, dim, 5.5, 7)
    arrowhead(dim.p2, dim, 5.5, 7)
    return finish("dimension")


ICONS = {
    f.__name__: f
    for f in [horizontal_vertical, coincident, tangent, equal, parallel, perpendicular, fix, midpoint, concentric,
              collinear, symmetric, dimension]
}

wanted = sys.argv[1:] or list(ICONS)
failed = [name for name in wanted if not ICONS[name]()]
doc.new()
if failed:
    raise SystemExit(f"not fully constrained: {', '.join(failed)}")
