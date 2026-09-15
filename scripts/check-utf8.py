"""Fail if client/server source contains invalid UTF-8 or common mojibake."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
bad = False
for directory in (root / "switch/source", root / "server"):
    for path in directory.rglob("*"):
        if path.suffix not in (".cpp", ".hpp", ".js", ".html", ".css") or path.name == "assets.hpp":
            continue
        try:
            value = path.read_text(encoding="utf-8")
        except UnicodeError:
            print(f"Invalid UTF-8: {path}")
            bad = True
            continue
        for number, line in enumerate(value.splitlines(), 1):
            if re.search(r"\ufffd|Ã[\x80-\xff]|Â[\x80-\xff]|â[€†‡]", line):
                print(f"Mojibake: {path}:{number}")
                bad = True
if bad:
    raise SystemExit(1)
print("PASS: UTF-8 source and messages without mojibake")
