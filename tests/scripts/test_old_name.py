# Scripts from before the app was named Setsquare (written for `drawapp`) still run.
import drawapp

assert drawapp is setsquare
g = drawapp.linear(["#ff0000", "#0000ff"], start=s.point(0, 0), end=s.point(10, 0))
assert isinstance(g, setsquare.Gradient)
assert drawapp.document().sketch is not None
print("ok")
