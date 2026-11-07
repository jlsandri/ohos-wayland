#!/usr/bin/env python3
"""Places an app's window, closes it as its close button does, reopens it,
and prints the WINDOW_RECT lines. A hidden window keeps its size but
reports position 0,0; --visible starts it on screen, without input.

  rect_probe.py <app> <x> <y> <width> <height> [close|force-stop] [--visible] [--state fullscreen|maximize]

Runs on the host with fold-env.sh sourced.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app/scripts"))
import ohos_wl as w  # noqa: E402

app, rect, how = sys.argv[1], sys.argv[2:6], (sys.argv[6] if len(sys.argv) > 6 else "close")
bundle = w.bundle_name(app)
visible = "--visible" in sys.argv


def start(request):
    # A visible start takes the screen, but nothing is typed or clicked.
    if visible and not w.hdc("shell", f"pidof {bundle}").strip():
        w.hdc("shell", f"aa start -a EntryAbility -b {bundle}")
        time.sleep(3)
    w.request_background(app, request)

since = w.hdc("shell", "date '+%m-%d %H:%M:%S'").strip()
w.cmd_stop([app])
start("window " + " ".join(rect))
time.sleep(6)
if "--state" in sys.argv:
    start("state " + sys.argv[sys.argv.index("--state") + 1])
    time.sleep(3)
if how == "close":
    start("close")
    for _ in range(20):
        time.sleep(1)
        if not w.hdc("shell", f"pidof {bundle}").strip():
            break
else:
    w.cmd_stop([app])
    time.sleep(1)
print("alive after close:", bool(w.hdc("shell", f"pidof {bundle}").strip()))
start("noop")
time.sleep(6)
lines = [line for line in w.hdc("shell", "hilog -x").splitlines()
         if line[:14] >= since and bundle[-14:] in line and
         any(k in line for k in ("WINDOW_RECT", "APP_START", "WINDOW_PLACE_ERROR", "Ability onDestroy", "MW-Life",
                                  "SendToplevelClose", "DIRECT_CLIENT_EXIT", "MAIN_EXIT", "toplevels="))]
print("\n".join(line[:200] for line in lines))
w.cmd_stop([app])
