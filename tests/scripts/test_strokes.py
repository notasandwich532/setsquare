# Strokes through the API: colours, widths, line styles, and keeping them through edits.
sides = s.rectangle((0, 0), (40, 20))
bottom, right, top, left = sides

assert bottom.stroke is None
s.stroke(sides, color="#ed1c24", width=0.5)
assert bottom.stroke == {"color": "#ed1c24", "width": 0.5, "dash": "solid"}, bottom.stroke
s.stroke(top, dash="dashed")  # only the dash changes
assert top.stroke["dash"] == "dashed" and top.stroke["color"] == "#ed1c24"
s.stroke([left], color=(0, 162, 232, 128), width=0)
assert left.stroke == {"color": "#00a2e880", "width": 0.0, "dash": "solid"}

# Copies look like their originals.
copies = s.copy(sides, dy=30)
assert copies[2].stroke["dash"] == "dashed"

# Bad input is refused.
for bad in [dict(color="red"), dict(dash="wavy"), dict(color=(1, 2))]:
    try:
        s.stroke(bottom, **bad)
        raise AssertionError(f"accepted {bad}")
    except (ValueError, TypeError):
        pass

s.clear_stroke(sides)
assert all(side.stroke is None for side in sides)
assert copies[0].stroke is not None  # only what was asked
print("ok")
