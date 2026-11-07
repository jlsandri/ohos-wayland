#!/usr/bin/env python3
"""Writes replay fixtures for scroll measurement from the recorded swipe.

  fixtures.py <out-dir>

Each fixture is one gesture of apps/testing/fixtures/touchpad-swipe.tsv,
replayed as recorded, then the coasting events ArkUI would send after that
lift (CoastingAxisEventGenerator, arkui_ace_engine 21b150e7,
frameworks/core/event/coasting_axis_event_generator.cpp), with frame
captures for measure.py. A `term-` fixture first types `seq 1 5000` so a
terminal has scrollback, and scrolls toward older lines.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECORDING = ROOT / "apps/testing/fixtures/touchpad-swipe.tsv"

# coasting_axis_event_generator.cpp
VELOCITY_SCALE = 2.25
FRICTION = 1.2
FRICTION_SCALE = 4.2
START_VELOCITY = 240.0  # units/s; below it ArkUI does not coast, and coasting ends
FRAME_MS = 1000 / 120
# ArkUI's VelocityTracker fits recent points; this window is an approximation.
VELOCITY_WINDOW_MS = 100

CAPTURE_EVERY_MS = 60
SETTLE_MS = 3000

# evdev codes for typing `seq 1 5000` and Enter.
KEYS = {"s": 31, "e": 18, "q": 16, " ": 57, "1": 2, "5": 6, "0": 11, "\n": 28}


def gestures():
    """The recording as lists of (absolute ms, value), split at each stop."""
    at, current, found = 0.0, [], []
    for line in RECORDING.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        ms, axis, value, _step, _source, stop = line.split("\t")
        at += float(ms)
        if stop == "1":
            if axis == "0" and current:
                found.append((current, at))
            current = []
        elif axis == "0":
            current.append((at, float(value)))
    return found


def lift_velocity(moves, lift_at):
    """Least-squares slope (units/s) of the summed position near the lift.
    ArkUI adds the lift itself as a still point, so a pause before lifting
    leaves no velocity."""
    position, points = 0.0, []
    for t, value in moves + [(lift_at, 0.0)]:
        position += value
        if t >= lift_at - VELOCITY_WINDOW_MS:
            points.append((t / 1000, position))
    if len(points) < 2:
        return 0.0
    mean_t = sum(t for t, _ in points) / len(points)
    mean_p = sum(p for _, p in points) / len(points)
    num = sum((t - mean_t) * (p - mean_p) for t, p in points)
    den = sum((t - mean_t) ** 2 for t, _ in points)
    return num / den if den else 0.0


def coasting(velocity):
    """ArkUI's coasting deltas after a lift: a critically damped spring to
    v/(FRICTION*FRICTION_SCALE), which is exp(-wt) decay at w = 5.04/s."""
    v0 = VELOCITY_SCALE * velocity
    if abs(v0) <= START_VELOCITY:
        return []
    omega = FRICTION * FRICTION_SCALE
    final = v0 / omega
    deltas, last, t = [], 0.0, 0.0
    while True:
        t += FRAME_MS / 1000
        if abs(v0) * math.exp(-omega * t) < START_VELOCITY:
            return deltas
        position = final * (1 - math.exp(-omega * t))
        deltas.append(position - last)
        last = position


def fixture(moves, lift_at, sign, terminal):
    events = []  # (absolute ms, fields...)
    start = 0.0
    if terminal:
        t = 6000.0
        for char in "seq 1 5000\n":
            events.append((t, "key", KEYS[char], 1))
            events.append((t + 20, "key", KEYS[char], 0))
            t += 60
        start = t + 3000
    else:
        start = 2500.0
    events.append((start, "capture", "base"))
    offset = start + 200 - moves[0][0]
    for t, value in moves:
        events.append((t + offset, 0, f"{sign * value:.2f}", 3, 1, 0))
    last = moves[-1][0] + offset
    events.append((last + 1, "capture", "drag"))
    lift = lift_at + offset
    events.append((lift, 0, "0.00", 3, 1, 1))
    events.append((lift, 1, "0.00", 3, 1, 1))
    t = lift + FRAME_MS
    deltas = coasting(lift_velocity(moves, lift_at))
    for i, delta in enumerate(deltas):
        events.append((t, "coast", 1 if i == 0 else 2, "0", f"{sign * delta:.3f}"))
        t += FRAME_MS
    if deltas:
        events.append((t, "coast", 3, "0", "0"))
    first = moves[0][0] + offset
    end = t + SETTLE_MS
    c = first + CAPTURE_EVERY_MS
    while c < end - 500:
        events.append((c + 0.5, "capture", f"t{int(c - first):05d}"))
        c += CAPTURE_EVERY_MS
    events.append((end, "capture", "settled"))
    events.sort(key=lambda e: e[0])
    lines, previous = [], 0.0
    for e in events:
        lines.append("\t".join([f"{e[0] - previous:.0f}"] + [str(f) for f in e[1:]]))
        previous += round(e[0] - previous)
    finger = sum(v for _, v in moves)
    coast = sum(deltas)
    header = (f"# finger {sign * finger:.1f} vp over {moves[-1][0] - moves[0][0]:.0f} ms, "
              f"lift velocity {sign * lift_velocity(moves, lift_at):.0f}/s, coasting {sign * coast:.1f} "
              f"in {len(deltas)} frames\n")
    return header + "\n".join(lines) + "\n", sign * finger, sign * coast


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    found = gestures()
    chosen = {"slow": found[0], "flick": found[2]}
    for name, (moves, lift_at) in chosen.items():
        for terminal in (False, True):
            # A terminal sits at the end of its scrollback, so it scrolls up.
            total = sum(v for _, v in moves)
            sign = (-1 if total > 0 else 1) if terminal else (1 if total > 0 else -1)
            text, finger, coast = fixture(moves, lift_at, sign, terminal)
            path = out / f"{'term-' if terminal else ''}{name}.tsv"
            path.write_text(text)
            print(f"{path.name}\tfinger={finger:.1f}\tcoast={coast:.1f}")


if __name__ == "__main__":
    main()
