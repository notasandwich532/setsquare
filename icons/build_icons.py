# Builds the tool icons: each is drawn as a fully constrained sketch on a 24 x 24 grid, saved as
# an editable source (src/<name>.dsk) and exported as SVG (<name>.svg).
#
#   drawapp-cli icons/build_icons.py            (all of them)
#   drawapp-cli icons/build_icons.py tangent    (just some)
#
# Style (after Fusion's, simplified, with thicker lines so they read when small): what the tool
# acts on is RED, what it acts against is GREY; for create tools, the shape is GREY and where you
# click (or what you pick) is BLUE. The app swaps these colours for other states (e.g. white on the
# green "active" button), so keep to these three.
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RED = "#e8615c"
GREY = "#d5d9de"
BLUE = "#4da3ff"  # create tools: where you click, and curves you pick
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


# --- create tools -----------------------------------------------------------------------------
# The shape drawn is GREY; where you click is a BLUE square; curves you pick (e.g. to be tangent
# to) are BLUE lines.

THIN = 1.5  # dashed guides: radii, centre lines, construction circles


def marker(p, size=2.5):
    """A click point: a small square on `p`, stroked thick so it reads as a rounded dot."""
    x, y = p.pos
    sq = s.rectangle_center(p, (x + size / 2, y + size / 2))
    stroke(sq, BLUE)
    s.distance(sq[0], value=size)
    s.distance(sq[1], value=size)
    s.fill(at=(x, y), color=BLUE)
    return sq


def guide(a, b):
    """A thin dashed grey line."""
    return line(a, b, GREY, width=THIN, dash="dashed")


def ray(center, angle, r, curve, shown=False):
    """A point on `curve` at `angle` degrees from `center`, held by a line from the centre."""
    cx, cy = center.pos
    p = s.point(cx + r * math.cos(math.radians(angle)), cy + r * math.sin(math.radians(angle)))
    s.coincident(p, curve)
    l = guide(center, p) if shown else s.line(center, p)
    if not shown:
        l.construction = True
    s.angle(l, s.x_axis, value=abs((angle + 180) % 360 - 180))  # unsigned; the side is kept
    return p


def show_construction_circle(c):
    c.construction = False
    stroke(c, GREY, width=THIN, dash="dashed")


def icon_line():
    start()
    l = line((4, 5), (20, 19), GREY)
    pin(l.p1, 4, 5)
    pin(l.p2, 20, 19)
    marker(l.p1)
    marker(l.p2)
    return finish("line")


def icon_point():
    start()
    p = s.point(12, 12)
    pin(p, 12, 12)
    marker(p, size=6)
    return finish("point")


def icon_rectangle():
    start()
    r = stroke(s.rectangle((3, 5), (21, 19)), GREY)
    pin(r[0].p1, 3, 5)
    s.distance(r[0], value=18)
    s.distance(r[1], value=14)
    marker(r[0].p1)
    marker(r[2].p1)
    return finish("rectangle")


def icon_rectangle_3point():
    start()
    a = s.point(3.5, 12.5)
    pin(a, 3.5, 12.5)
    b = s.point(14.76, 19)
    r = stroke(s.rectangle_3point(a, b, (18.76, 12.07)), GREY)
    s.angle(r[0], s.x_axis, value=30)
    s.distance(r[0], value=13)
    s.distance(r[1], value=9.5)
    marker(a)
    marker(b)
    marker(r[1].p2)
    return finish("rectangle_3point")


def icon_rectangle_center():
    start()
    c = s.point(12, 12)
    pin(c, 12, 12)
    r = stroke(s.rectangle_center(c, (21, 19)), GREY)
    s.distance(r[0], value=18)
    s.distance(r[1], value=14)
    marker(c)
    marker(r[1].p2)
    return finish("rectangle_center")


def icon_circle():
    start()
    c = stroke(s.circle((12, 12), 9.5), GREY)
    pin(c.center, 12, 12)
    s.radius(c, value=9.5)
    p = ray(c.center, 45, 9.5, c, shown=True)
    marker(c.center)
    marker(p)
    return finish("circle")


def icon_circle_2point():
    start()
    d = guide((2.5, 12), (21.5, 12))
    pin(d.p1, 2.5, 12)
    s.horizontal(d)
    s.distance(d, value=19)
    c = stroke(s.circle((12, 12), 9.5), GREY)
    s.midpoint(c.center, d)
    s.coincident(d.p1, c)
    marker(d.p1)
    marker(d.p2)
    return finish("circle_2point")


def icon_circle_3point():
    start()
    c = stroke(s.circle((12, 12), 9.5), GREY)
    pin(c.center, 12, 12)
    s.radius(c, value=9.5)
    points = [ray(c.center, a, 9.5, c) for a in (90, 210, 330)]
    for p in points:
        marker(p)
    return finish("circle_3point")


def icon_circle_tangent2():
    start()
    floor = line((2.5, 3), (21.5, 3), BLUE)
    wall = line(floor.p1, (2.5, 21.5), BLUE)
    pin(floor.p1, 2.5, 3)
    s.horizontal(floor)
    s.vertical(wall)
    s.distance(floor, value=19)
    s.distance(wall, value=18.5)
    c = stroke(s.circle((10.5, 11), 8), GREY)
    s.tangent(c, floor)
    s.tangent(c, wall)
    s.radius(c, value=8)
    return finish("circle_tangent2")


def icon_circle_tangent3():
    start()
    centre = s.point(12, 9)
    pin(centre, 12, 9)
    sides = stroke(s.polygon(centre, (12, 21.5), sides=3), BLUE)
    s.horizontal(sides[1])
    ring = [c for c in s.circles if c.construction][0]
    s.radius(ring, value=12.5)
    c = stroke(s.circle((12, 9.2), 6), GREY)
    for side in sides:
        s.tangent(c, side)
    return finish("circle_tangent3")


def icon_arc():
    start()
    centre = s.point(12, 5)
    pin(centre, 12, 5)
    a = stroke(s.arc(centre, (21.19, 12.71), (2.81, 12.71)), GREY)
    s.radius(a, value=12)
    for end, angle in ((a.start, 40), (a.end, 140)):
        l = guide(centre, end)
        s.angle(l, s.x_axis, value=angle)
    marker(centre)
    marker(a.start)
    marker(a.end)
    return finish("arc")


def icon_arc3():
    start()
    centre = s.point(12, 10)
    pin(centre, 12, 10)
    a = stroke(s.arc(centre, (20.46, 6.92), (3.54, 6.92)), GREY)
    s.radius(a, value=9)
    chord = s.line(a.start, a.end)
    chord.construction = True
    s.horizontal(chord)
    s.horizontal_distance(centre, a.start, value=8.46)
    through = ray(centre, 90, 9, a)
    marker(a.start)
    marker(a.end)
    marker(through)
    return finish("arc3")


def icon_arc_tangent():
    start()
    l = line((2.5, 5), (11, 5), BLUE)
    pin(l.p1, 2.5, 5)
    s.horizontal(l)
    s.distance(l, value=8.5)
    end = s.point(11, 20)
    pin(end, 11, 20)
    stroke(s.arc_tangent(l, l.p2, end), GREY)
    marker(end)
    return finish("arc_tangent")


def polygon_ring(inner):
    """The polygon's construction circle: the one through its corners, or the one inside it."""
    rings = sorted((c for c in s.circles if c.construction), key=lambda c: c.radius)
    return rings[0] if inner else rings[-1]


def icon_polygon_inscribed():
    start()
    centre = s.point(12, 12)
    pin(centre, 12, 12)
    sides = stroke(s.polygon(centre, (12, 21.5), sides=6), GREY)
    s.vertical(sides[1])
    ring = polygon_ring(inner=False)
    s.radius(ring, value=9.5)
    show_construction_circle(ring)
    marker(centre)
    marker(sides[0].p1)
    return finish("polygon_inscribed")


def icon_polygon_circumscribed():
    start()
    centre = s.point(12, 12)
    pin(centre, 12, 12)
    sides = stroke(s.polygon(centre, (12, 20.5), sides=6, circumscribed=True), GREY)
    top = max(sides, key=lambda l: l.p1.y + l.p2.y)
    s.horizontal(top)
    ring = polygon_ring(inner=True)
    s.radius(ring, value=8.5)
    show_construction_circle(ring)
    mid = s.point(12, 20.5)
    s.midpoint(mid, top)
    marker(centre)
    marker(mid)
    return finish("polygon_circumscribed")


def icon_polygon_edge():
    start()
    a = s.point(7.25, 3.75)
    b = s.point(16.75, 3.75)
    pin(a, 7.25, 3.75)
    pin(b, 16.75, 3.75)
    stroke(s.polygon_edge(a, b, sides=6), GREY)
    marker(a)
    marker(b)
    return finish("polygon_edge")


def icon_slot():
    start()
    c1 = s.point(7.25, 12)
    c2 = s.point(16.75, 12)
    pin(c1, 7.25, 12)
    pin(c2, 16.75, 12)
    curves = stroke(s.slot(c1, c2, 9.5), GREY)
    s.radius(curves[1], value=4.75)
    guide(c1, c2)
    marker(c1)
    marker(c2)
    return finish("slot")


def icon_slot_overall():
    start()
    curves = stroke(s.slot_overall((2.5, 12), (21.5, 12), 9.5), GREY)
    top, cap2, _, cap1 = curves
    pin(cap1.center, 7.25, 12)
    pin(cap2.center, 16.75, 12)
    s.radius(cap2, value=4.75)
    overall = guide((2.5, 12), (21.5, 12))
    s.coincident(overall.p1, cap1)
    s.coincident(overall.p2, cap2)
    s.coincident(cap1.center, overall)
    s.coincident(cap2.center, overall)
    marker(overall.p1)
    marker(overall.p2)
    return finish("slot_overall")


def icon_slot_center_point():
    start()
    m = s.point(12, 12)
    pin(m, 12, 12)
    curves = stroke(s.slot_center_point(m, (18.5, 12), 7), GREY)
    cap2 = curves[1]
    centreline = next(l for l in s.lines if l.construction and cap2.center in (l.p1, l.p2))
    s.horizontal(centreline)
    s.distance(centreline, value=13)
    s.radius(cap2, value=3.5)
    guide(m, cap2.center)
    marker(m)
    marker(cap2.center)
    return finish("slot_center_point")


def arc_slot_parts(curves):
    outer, cap_e, inner, cap_s = curves
    return cap_s, cap_e


def icon_arc_slot():
    start()
    centre = s.point(12, 4)
    pin(centre, 12, 4)
    r = 10.5
    pos = lambda a: (12 + r * math.cos(math.radians(a)), 4 + r * math.sin(math.radians(a)))
    curves = stroke(s.arc_slot(centre, pos(35), pos(145), 5.5), GREY)
    cap_s, cap_e = arc_slot_parts(curves)
    s.radius(cap_s, value=2.75)
    for cap, angle in ((cap_s, 35), (cap_e, 145)):
        l = guide(centre, cap.center)
        s.angle(l, s.x_axis, value=angle)
    s.distance(centre, cap_s.center, value=r)  # the other cap is on the same arc
    marker(centre)
    marker(cap_s.center)
    marker(cap_e.center)
    return finish("arc_slot")


def icon_arc_slot_3point():
    start()
    r = 10.5
    cy = 6
    pos = lambda a: (12 + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
    curves = stroke(s.arc_slot_3point(pos(35), pos(145), pos(90), 5.5), GREY)
    cap_s, cap_e = arc_slot_parts(curves)
    s.radius(cap_s, value=2.75)
    centreline = [a for a in s.arcs if a.construction][0]
    pin(centreline.center, 12, cy)
    for cap, angle in ((cap_s, 35), (cap_e, 145)):
        l = s.line(centreline.center, cap.center)
        l.construction = True
        s.angle(l, s.x_axis, value=angle)
    s.radius(centreline, value=r)
    through = ray(centreline.center, 90, r, centreline)
    marker(cap_s.center)
    marker(cap_e.center)
    marker(through)
    return finish("arc_slot_3point")


ICONS = {
    f.__name__: f
    for f in [horizontal_vertical, coincident, tangent, equal, parallel, perpendicular, fix, midpoint, concentric,
              collinear, symmetric, dimension]
}
ICONS.update({
    f.__name__.removeprefix("icon_"): f
    for f in [icon_line, icon_point, icon_rectangle, icon_rectangle_3point, icon_rectangle_center, icon_circle,
              icon_circle_2point, icon_circle_3point, icon_circle_tangent2, icon_circle_tangent3, icon_arc, icon_arc3,
              icon_arc_tangent, icon_polygon_inscribed, icon_polygon_circumscribed, icon_polygon_edge, icon_slot,
              icon_slot_overall, icon_slot_center_point, icon_arc_slot, icon_arc_slot_3point]
})

wanted = sys.argv[1:] or list(ICONS)
failed = [name for name in wanted if not ICONS[name]()]
doc.new()
if failed:
    raise SystemExit(f"not fully constrained: {', '.join(failed)}")
