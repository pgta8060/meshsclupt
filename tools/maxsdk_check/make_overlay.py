#!/usr/bin/env python3
"""Writes a clang VFS overlay that makes header lookups case-insensitive.

The 3ds Max SDK (and code written for it) includes headers with whatever
capitalisation works on Windows. Clang on Linux needs an overlay with
'case-sensitive': 'false' to resolve them.

Usage: make_overlay.py OUTPUT.yaml DIR [DIR ...]
"""
import json
import os
import sys


def tree(path):
    items = []
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isdir(full):
            items.append({"name": name, "type": "directory", "contents": tree(full)})
        elif os.path.isfile(full):
            items.append({"name": name, "type": "file", "external-contents": full})
    return items


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    output, dirs = sys.argv[1], [os.path.realpath(d) for d in sys.argv[2:]]
    overlay = {
        "version": 0,
        "case-sensitive": "false",
        "roots": [{"name": d, "type": "directory", "contents": tree(d)} for d in dirs if os.path.isdir(d)],
    }
    with open(output, "w", encoding="utf-8") as f:
        json.dump(overlay, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
