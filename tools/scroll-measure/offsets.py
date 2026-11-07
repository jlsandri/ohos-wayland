#!/usr/bin/env python3
"""Content movement from a measure.py run, in vp.

  offsets.py <run-dir> <fixture.tsv> <density>

Frames are chained in capture order: each pair's vertical shift is the one
with the least mean absolute difference over their overlap, so a fling of
several screens is measured as long as consecutive frames overlap. A step
past the search window matches a look-alike row instead: a terminal full of
`seq` lines read 18.8 rows for 39. Check terminal runs against the top line
number of the settled frame. Prints
the movement at the last finger event (`drag`) and once settled, as
positive numbers in the fixture's scroll direction.
"""
import sys
from pathlib import Path

import numpy as np

TOP, BOTTOM = 0.12, 0.04  # title bar and bottom edge stay out of the match
BIN = 2                    # columns per bin, on top of measure.py's halving


def read_pgm(path):
    data = path.read_bytes()
    _magic, size, _max, pixels = data.split(b"\n", 3)
    width, height = (int(v) for v in size.split())
    image = np.frombuffer(pixels[:width * height], np.uint8).reshape(height, width).astype(np.float32)
    usable = width // BIN * BIN
    return image[:, :usable].reshape(height, usable // BIN, BIN).mean(axis=2)


def shift(a, b):
    """d such that b[y] == a[y + d]: content moved up by d pixels."""
    height = a.shape[0]
    top, bottom = int(height * TOP), int(height * (1 - BOTTOM))
    rows = bottom - top
    best, best_d = None, 0
    for d in range(-int(rows * 0.75), int(rows * 0.75) + 1):
        lo, hi = max(top, top - d), min(bottom, bottom - d)
        if hi - lo < rows // 4:
            continue
        cost = np.abs(a[lo + d:hi + d] - b[lo:hi]).mean()
        if best is None or cost < best - 1e-6 or (abs(cost - best) <= 1e-6 and abs(d) < abs(best_d)):
            best, best_d = cost, d
    return best_d, best


def capture_times(fixture):
    at, times = 0.0, {}
    for line in fixture.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        at += float(fields[0])
        if fields[1] == "capture":
            times[fields[2]] = at
    return times


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    run, fixture, density = Path(sys.argv[1]), Path(sys.argv[2]), float(sys.argv[3])
    header = fixture.read_text().splitlines()[0]
    sign = -1 if header.split()[2].startswith("-") else 1
    times = capture_times(fixture)
    frames = sorted((times[p.stem.removeprefix("ohos_wl_replay_")], p.stem.removeprefix("ohos_wl_replay_"), p)
                    for p in run.glob("ohos_wl_replay_*.pgm") if p.stem.removeprefix("ohos_wl_replay_") in times)
    if not frames or frames[0][1] != "base":
        sys.exit(f"{run}: no base frame")
    position, previous, track = 0, read_pgm(frames[0][2]), {"base": 0.0}
    worst = 0.0
    for _t, label, path in frames[1:]:
        image = read_pgm(path)
        d, cost = shift(previous, image)
        worst = max(worst, cost)
        position += d
        track[label] = position
        previous = image
    to_vp = 1 / density
    drag = sign * track.get("drag", float("nan")) * to_vp
    settled = sign * track.get("settled", position) * to_vp
    (run / "track.tsv").write_text("".join(f"{label}\t{track[label] * to_vp * sign:.1f}\n"
                                           for _t, label, _p in frames if label in track))
    print(f"{run.name}\tdrag={drag:.1f}\tsettled={settled:.1f}\tframes={len(frames)}\tworst_cost={worst:.1f}")


if __name__ == "__main__":
    main()
