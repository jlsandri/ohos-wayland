#!/usr/bin/env python3
"""Replays a scroll fixture into an installed app and pulls its frames.

  measure.py <app> <fixture.tsv> <out-dir> [--test <name>] [--tuning "<gain> <platform|client>"]

Runs on the host. The app starts hidden (`aa pre-start`), gets the fixture
through the `replay` request, and writes each `capture` line's frame. The
frames come back as grayscale PGMs, every second column, for offsets.py,
together with the scroll lines of hilog. The app is closed at the end.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app/scripts"))
import ohos_wl as w  # noqa: E402

SANDBOX_CACHE = "/data/storage/el2/base/cache"
LOG_KEYS = ("GTK_SCROLL", "[Input]", "COASTING", "SCROLL_TUNING", "INPUT_REPLAY", "FRAME_CAPTURE")


def send(bundle, local, name):
    w.hdc("file", "send", "-b", bundle, str(local), f"{SANDBOX_CACHE}/{name}")


def to_pgm(ppm, pgm):
    data = ppm.read_bytes()
    _magic, size, _max, pixels = data.split(b"\n", 3)
    width, height = (int(v) for v in size.split())
    half = (width + 1) // 2
    gray = bytearray(half * height)
    for y in range(height):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        g = row[1::6]
        gray[y * half:y * half + len(g)] = g
    pgm.write_bytes(b"P5\n%d %d\n255\n" % (half, height) + bytes(gray))
    ppm.unlink()


def main():
    args = sys.argv[1:]
    test = tuning = None
    if "--test" in args:
        i = args.index("--test")
        test = args[i + 1]
        del args[i:i + 2]
    if "--tuning" in args:
        i = args.index("--tuning")
        tuning = args[i + 1]
        del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    app, fixture, out = args[0], Path(args[1]), Path(args[2])
    out.mkdir(parents=True, exist_ok=True)
    bundle = w.bundle_name(app)
    remote = f"/data/app/el2/100/base/{bundle}/cache"
    w.cmd_stop([app])
    w.hdc("shell", f"rm -f {remote}/ohos_wl_replay_* {remote}/ohos_wl_scroll_tuning {remote}/ohos_wl_request")
    if tuning:
        tmp = out / "tuning"
        tmp.write_text(tuning + "\n")
        send(bundle, tmp, "ohos_wl_scroll_tuning")
    send(bundle, fixture, "ohos_wl_scroll.tsv")
    since = w.hdc("shell", "date '+%m-%d %H:%M:%S'").strip()
    if test:
        w.request_background(app, f"test {test}")
        time.sleep(1.5)
    w.request_background(app, f"replay {SANDBOX_CACHE}/ohos_wl_scroll.tsv")
    total = sum(float(line.split("\t")[0]) for line in fixture.read_text().splitlines()
                if line and not line.startswith("#")) / 1000
    deadline = time.monotonic() + total + 20
    time.sleep(total)
    while time.monotonic() < deadline and "INPUT_REPLAY done" not in w.hdc("shell", "hilog -x | tail -400"):
        time.sleep(1)
    time.sleep(1)
    lines = [line for line in w.hdc("shell", "hilog -x").splitlines()
             if line[:14] >= since and any(k in line for k in LOG_KEYS)]
    (out / "hilog.txt").write_text("\n".join(lines) + "\n")
    names = w.hdc("shell", f"ls {remote}").split()
    for name in sorted(n for n in names if n.startswith("ohos_wl_replay_") and n.endswith(".ppm")):
        local = out / name
        w.hdc("file", "recv", f"{remote}/{name}", str(local))
        to_pgm(local, local.with_suffix(".pgm"))
    w.cmd_stop([app])
    print(f"{out}: {len(list(out.glob('*.pgm')))} frames, {len(lines)} log lines")


if __name__ == "__main__":
    main()
