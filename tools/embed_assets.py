#!/usr/bin/env python3
"""Writes a C++ file holding files as byte arrays, for the plugin build (an NSO
has no romfs). usage: embed_assets.py out.cpp name=path [name=path ...]"""
import sys

out, pairs = sys.argv[1], sys.argv[2:]
lines = ["#include <stddef.h>"]
for pair in pairs:
    name, path = pair.split("=", 1)
    data = open(path, "rb").read()
    body = ",".join(str(b) for b in data) or "0"
    lines.append(f"extern const unsigned char {name}[] = {{{body}}};")
    lines.append(f"extern const size_t {name}_size = {len(data)};")
open(out, "w").write("\n".join(lines) + "\n")
