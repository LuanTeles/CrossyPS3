#!/usr/bin/env python3
"""Pack the original object models for the Blender remaster pass.

For every non-ground model: original OBJ polygons (quads kept) with one colour
per polygon taken from the texture - the *dominant* texel colour, so faces
that straddle a palette seam don't come out as a muddy average.

Writes one base64(zlib(json)) string per model to <out_dir>/<name>.b64.
"""
import os, sys, json, zlib, base64
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import convert  # noqa: E402

SRC = sys.argv[1]
OUT = sys.argv[2]
convert.A = os.path.join(SRC, "assets")
GROUND = {"grass_light", "grass_dark", "road_stripes", "road_blank", "river", "railroad"}


def dominant(baker, uvs):
    cols = []
    # fan-triangulate the polygon in UV space and collect texels
    for k in range(1, len(uvs) - 1):
        t = [baker.uv2px(uvs[0]), baker.uv2px(uvs[k]), baker.uv2px(uvs[k + 1])]
        cols.append(baker.region_colors(t))
    cols = np.concatenate(cols)
    vals, counts = np.unique(cols, return_counts=True)
    return int(vals[np.argmax(counts)])


def parse_polys(path):
    vs, vts, polys = [], [], []
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            vs.append([float(x) for x in p[1:4]])
        elif p[0] == "vt":
            vts.append((float(p[1]), float(p[2])))
        elif p[0] == "f":
            idx = []
            for c in p[1:]:
                parts = c.split("/")
                vi = int(parts[0]); vi = vi - 1 if vi > 0 else len(vs) + vi
                ti = int(parts[1]) - 1 if len(parts) > 1 and parts[1] else None
                idx.append((vi, ti))
            polys.append(idx)
    return vs, vts, polys


os.makedirs(OUT, exist_ok=True)
for name, objp, pngp in convert.MODELS:
    if name in GROUND:
        continue
    vs, vts, polys = parse_polys(os.path.join(convert.A, objp))
    baker = convert.Baker(os.path.join(convert.A, pngp))
    faces, cols = [], []
    for poly in polys:
        uvs = [vts[t] if t is not None else (0, 0) for _, t in poly]
        faces.append([vi for vi, _ in poly])
        cols.append(dominant(baker, uvs))
    data = {"name": name, "v": [[round(c, 5) for c in v] for v in vs], "f": faces, "c": cols}
    blob = base64.b64encode(zlib.compress(json.dumps(data, separators=(",", ":")).encode(), 9)).decode()
    open(os.path.join(OUT, name + ".b64"), "w").write(blob)
    print(f"{name:14s} verts={len(vs):5d} polys={len(faces):5d} colours={len(set(cols)):3d} b64={len(blob)}")
