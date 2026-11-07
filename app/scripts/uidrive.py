#!/usr/bin/env python3
"""Drives another app's UI through hdc uitest (used to run DevEco Studio's
automatic signing for generated projects).

  uidrive.py dump [bundle]            list visible controls: type, text, bounds
  uidrive.py click <text> [bundle]    click the centre of the first control whose text contains <text>
  uidrive.py clickxy <x> <y>          click a screen point
  uidrive.py key <keycode...>         send key events (one key, or a combination)
  uidrive.py type <x> <y> <text>      click a field and type text
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

TOOL_HOME = Path(os.environ.get("TOOL_HOME", "/data/service/hnp/hmos-clt.org/hmos-clt_1.0.0"))
HDC = os.environ.get("HDC", str(TOOL_HOME / "sdk/default/openharmony/toolchains/hdc"))
DEVICE = os.environ.get("OHOS_WL_DEVICE", "127.0.0.1:37757")
LOCAL = Path(os.environ.get("TMPDIR", "/data/storage/el2/base/cache")) / "uidrive.json"
DEFAULT_BUNDLE = "com.huawei.devecostudio"


def hdc(*args):
    return subprocess.run([HDC, "-t", DEVICE, *args], capture_output=True, text=True).stdout


def controls(bundle):
    # The merged layout of all visible windows: with -b, a notification
    # window of the bundle can shadow its main window.
    hdc("shell", "uitest dumpLayout -p /data/local/tmp/uidrive.json")
    hdc("file", "recv", "/data/local/tmp/uidrive.json", str(LOCAL))
    found = []

    def walk(node):
        attrs = node.get("attributes", {})
        text = attrs.get("text", "") or attrs.get("description", "")
        bounds = attrs.get("bounds", "")
        if text and bounds:
            found.append((attrs.get("type", ""), text, bounds))
        for child in node.get("children", []):
            walk(child)

    walk(json.loads(LOCAL.read_text()))
    return found


def centre(bounds):
    x1, y1, x2, y2 = (int(v) for v in re.findall(r"-?\d+", bounds))
    return (x1 + x2) // 2, (y1 + y2) // 2


def main(args):
    if not args:
        print(__doc__)
        return 2
    command = args[0]
    if command == "dump":
        for kind, text, bounds in controls(args[1] if len(args) > 1 else DEFAULT_BUNDLE):
            print(f"{kind}\t{text[:70]!r}\t{bounds}")
    elif command == "click":
        bundle = args[2] if len(args) > 2 else DEFAULT_BUNDLE
        for _kind, text, bounds in controls(bundle):
            if args[1] in text:
                x, y = centre(bounds)
                hdc("shell", f"uitest uiInput click {x} {y}")
                print(f"clicked {text[:60]!r} at {x},{y}")
                return 0
        print(f"no control with text {args[1]!r}")
        return 1
    elif command == "clickxy":
        hdc("shell", f"uitest uiInput click {args[1]} {args[2]}")
    elif command == "key":
        hdc("shell", "uitest uiInput keyEvent " + " ".join(args[1:]))
    elif command == "type":
        hdc("shell", f"uitest uiInput inputText {args[1]} {args[2]} '{args[3]}'")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
