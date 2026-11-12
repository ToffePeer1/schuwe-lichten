"""Builds src/web/patterns.json, the compact pattern list the phone page uses.

Reads design/patterns.json (the user's names, categories and finger drawings for all
51 laser patterns, written by tools/pattern_mapper/) and keeps only what the page needs:
number, name, category, duplicateOf and the strokes. Strokes are simplified with
Ramer-Douglas-Peucker and quantised to 2 decimals, so the file stays small enough
to embed in the firmware and serve at /patterns.json.

Run (stdlib only):
    uv run --no-project python tools/build_patterns.py
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "design" / "patterns.json"
TARGET = ROOT / "src" / "web" / "patterns.json"

PATTERN_COUNT = 51
EPSILON = 0.006  # RDP tolerance in drawing units (-1..1); about 0.3 px on a 100 px thumbnail
DECIMALS = 2
SIZE_BUDGET = 60 * 1024

Point = tuple[float, float]


def perpendicular_distance(p: Point, a: Point, b: Point) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    if length == 0:
        return math.hypot(p[0] - a[0], p[1] - a[1])
    return abs(dy * p[0] - dx * p[1] + b[0] * a[1] - b[1] * a[0]) / length


def simplify(points: list[Point], epsilon: float) -> list[Point]:
    """Iterative Ramer-Douglas-Peucker (no recursion limit for long strokes)."""
    if len(points) < 3:
        return list(points)
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        best, index = 0.0, -1
        for i in range(first + 1, last):
            d = perpendicular_distance(points[i], points[first], points[last])
            if d > best:
                best, index = d, i
        if index >= 0 and best > epsilon:
            keep[index] = True
            stack.extend(((first, index), (index, last)))
    return [p for p, k in zip(points, keep) if k]


def quantise(value: float) -> float | int:
    q = round(max(-1.0, min(1.0, value)), DECIMALS)
    return int(q) if q == int(q) else q


def compact_stroke(stroke: list[list[float]]) -> list[list[float | int]]:
    points = [(float(x), float(y)) for x, y in stroke]
    out: list[list[float | int]] = []
    for x, y in simplify(points, EPSILON):
        p = [quantise(x), quantise(y)]
        if not out or out[-1] != p:
            out.append(p)
    return out


def compact_pattern(p: dict) -> dict:
    out = {"number": int(p["number"]), "name": str(p["name"]), "category": str(p.get("category", ""))}
    if p.get("duplicateOf"):
        out["duplicateOf"] = int(p["duplicateOf"])
    out["strokes"] = [s for s in (compact_stroke(s) for s in p.get("strokes", [])) if s]
    return out


def validate(patterns: list[dict]) -> None:
    numbers = [p["number"] for p in patterns]
    if numbers != list(range(1, PATTERN_COUNT + 1)):
        raise SystemExit(f"expected patterns 1..{PATTERN_COUNT} in order, got {numbers}")
    for p in patterns:
        if not p["name"] or not p["category"]:
            raise SystemExit(f"pattern {p['number']} has no name or category")
        if not p["strokes"]:
            raise SystemExit(f"pattern {p['number']} has no drawing")


def main() -> int:
    source = json.loads(SOURCE.read_text(encoding="utf-8"))
    patterns = [compact_pattern(p) for p in source["patterns"]]
    validate(patterns)
    text = json.dumps({"version": 1, "patterns": patterns}, ensure_ascii=False, separators=(",", ":"))
    TARGET.write_text(text + "\n", encoding="utf-8", newline="\n")
    size = TARGET.stat().st_size
    before = sum(len(s) for p in source["patterns"] for s in p.get("strokes", []))
    after = sum(len(s) for p in patterns for s in p["strokes"])
    print(f"{TARGET.relative_to(ROOT)}: {size} bytes ({size / 1024:.1f} KB), points {before} -> {after}")
    if size > SIZE_BUDGET:
        print(f"too big: budget is {SIZE_BUDGET} bytes", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
