#!/usr/bin/env python3
"""Compare saved benchmark logs; timings are warnings, never CI failures."""
import re
import sys


def measurements(path):
    values = {}
    for line in open(path, encoding="utf-8"):
        fields = dict(re.findall(r"([a-zA-Z0-9_]+)=([0-9]+)", line))
        scale = fields.get("review_scale", "shared")
        for key, value in fields.items():
            if key.endswith(("_us", "_ms")):
                values[scale, key] = int(value)
    return values


before, after = map(measurements, sys.argv[1:3])
for key in sorted(before.keys() & after.keys()):
    old, new = before[key], after[key]
    large = new > max(old * 1.5, old + (1000 if key[1].endswith("_us") else 5))
    print(f"{'REGRESSION WARNING' if large else 'measurement'} scale={key[0]} {key[1]}: {old} -> {new}")
