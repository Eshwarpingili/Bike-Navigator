#!/usr/bin/env python3
"""Every widget the UI struct declares must actually be constructed.

A widget that is declared and never built stays NULL, and the first thing that
touches it dereferences NULL - which on this chip halts everything, Bluetooth
included, and leaves a dark screen with no console to ask. That is exactly how
one flash went out dead: a block of construction code was removed by deleting a
range of lines, and the range quietly took the diagnostics screen with it.

Run from the firmware directory:  python tools/check_ui_objects.py
"""
import re
import sys
from pathlib import Path

src = (Path(sys.argv[1]) if len(sys.argv) > 1
       else Path(__file__).resolve().parent.parent / "main" / "ui.c")
text = src.read_text(encoding="utf-8", errors="replace")

# The single anonymous struct holding every widget pointer.
start = text.index("static struct {")
end = text.index("} ui;", start)
body = text[start:end]

declared = re.findall(r"^\s*lv_obj_t\s*\*(\w+)\s*;", body, re.MULTILINE)
after = text[end:]

missing = [name for name in declared
           if not re.search(r"\bui\.%s\s*=" % re.escape(name), after)]

print("%d widgets declared" % len(declared))
if missing:
    print("NEVER CONSTRUCTED (these will be NULL at runtime):")
    for name in missing:
        print("  ui.%s" % name)
    sys.exit(1)
print("all constructed")
