#!/usr/bin/env python3
"""Asset converter for the PS3 port.

Reads the original Expo project's OBJ models + palette textures and audio,
and writes source/assets_data.c + include/assets_data.h.

* Models: each triangle gets a single baked colour sampled from its texture.
  Triangles whose UV area covers several colours (road stripes, grass tufts)
  are subdivided until each piece is one colour. The PS3 renderer then only
  needs flat-coloured triangles - no texture sampling at runtime.
* Audio: decoded with ffmpeg to 48 kHz mono signed 16-bit (PS3 native rate).

Everything is emitted as C arrays so the PS3 build needs no file I/O and
has no endianness issues (the compiler lays the data out big-endian).
"""
import os, sys, subprocess, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Expo-Crossy-Road")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..")
A = os.path.join(SRC, "assets")

MODELS = [
    # name, obj, png
    ("grass_light", "models/environment/grass/model.obj", "models/environment/grass/light-grass.png"),
    ("grass_dark", "models/environment/grass/model.obj", "models/environment/grass/dark-grass.png"),
    ("road_stripes", "models/environment/road/model.obj", "models/environment/road/stripes-texture.png"),
    ("road_blank", "models/environment/road/model.obj", "models/environment/road/blank-texture.png"),
    ("river", "models/environment/river/0.obj", "models/environment/river/0.png"),
    ("railroad", "models/environment/railroad/0.obj", "models/environment/railroad/0.png"),
    ("lily_pad", "models/environment/lily_pad/0.obj", "models/environment/lily_pad/0.png"),
    ("tree0", "models/environment/tree/0/0.obj", "models/environment/tree/0/0.png"),
    ("tree1", "models/environment/tree/1/0.obj", "models/environment/tree/1/0.png"),
    ("tree2", "models/environment/tree/2/0.obj", "models/environment/tree/2/0.png"),
    ("tree3", "models/environment/tree/3/0.obj", "models/environment/tree/3/0.png"),
    ("boulder0", "models/environment/boulder/0/0.obj", "models/environment/boulder/0/0.png"),
    ("boulder1", "models/environment/boulder/1/0.obj", "models/environment/boulder/1/0.png"),
    ("log0", "models/environment/log/0/0.obj", "models/environment/log/0/0.png"),
    ("log1", "models/environment/log/1/0.obj", "models/environment/log/1/0.png"),
    ("log2", "models/environment/log/2/0.obj", "models/environment/log/2/0.png"),
    ("log3", "models/environment/log/3/0.obj", "models/environment/log/3/0.png"),
    ("light_off", "models/environment/train_light/inactive/0.obj", "models/environment/train_light/inactive/0.png"),
    ("light_on0", "models/environment/train_light/active/0/0.obj", "models/environment/train_light/active/0/0.png"),
    ("light_on1", "models/environment/train_light/active/1/0.obj", "models/environment/train_light/active/1/0.png"),
    ("car_police", "models/vehicles/police_car/0.obj", "models/vehicles/police_car/0.png"),
    ("car_blue", "models/vehicles/blue_car/0.obj", "models/vehicles/blue_car/0.png"),
    ("truck_blue", "models/vehicles/blue_truck/0.obj", "models/vehicles/blue_truck/0.png"),
    ("car_green", "models/vehicles/green_car/0.obj", "models/vehicles/green_car/0.png"),
    ("car_orange", "models/vehicles/orange_car/0.obj", "models/vehicles/orange_car/0.png"),
    ("car_purple", "models/vehicles/purple_car/0.obj", "models/vehicles/purple_car/0.png"),
    ("truck_red", "models/vehicles/red_truck/0.obj", "models/vehicles/red_truck/0.png"),
    ("car_taxi", "models/vehicles/taxi/0.obj", "models/vehicles/taxi/0.png"),
    ("train_front", "models/vehicles/train/front/0.obj", "models/vehicles/train/front/0.png"),
    ("train_middle", "models/vehicles/train/middle/0.obj", "models/vehicles/train/middle/0.png"),
    ("train_back", "models/vehicles/train/back/0.obj", "models/vehicles/train/back/0.png"),
    ("chicken", "models/characters/chicken/0.obj", "models/characters/chicken/0.png"),
]

SOUNDS = [
    ("buck1", "buck1.wav"), ("buck2", "buck2.wav"), ("buck3", "buck3.wav"),
    ("buck4", "buck4.wav"), ("buck5", "buck5.wav"), ("buck6", "buck6.wav"),
    ("buck7", "buck7.wav"), ("buck8", "buck8.wav"), ("buck9", "buck9.wav"),
    ("buck10", "buck10.wav"), ("buck11", "buck11.wav"), ("buck12", "buck12.wav"),
    ("chicken_die0", "chickendeath.wav"), ("chicken_die1", "chickendeath2.wav"),
    ("car_hit0", "carhit.mp3"), ("car_hit1", "carsquish3.wav"),
    ("car_horn", "car-horn.wav"),
    ("water", "watersplashlow.mp3"),
    ("train_pass", "train_pass_no_horn.wav"),
    ("train_die", "trainsplat.wav"),
    ("pop1", "Pop_1.wav"), ("pop2", "Pop_2.wav"),
    ("banner", "bannerhit3-g.wav"),
]


def fl(x):
    s = f"{x:g}"
    if "." not in s and "e" not in s and "inf" not in s and "nan" not in s:
        s += ".0"
    return s + "f"


def parse_obj(path):
    vs, vts, tris = [], [], []
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            vs.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == "vt":
            vts.append((float(p[1]), float(p[2])))
        elif p[0] == "f":
            idx = []
            for c in p[1:]:
                parts = c.split("/")
                vi = int(parts[0]); vi = vi - 1 if vi > 0 else len(vs) + vi
                ti = None
                if len(parts) > 1 and parts[1]:
                    ti = int(parts[1]); ti = ti - 1 if ti > 0 else len(vts) + ti
                idx.append((vi, ti))
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1]))
    return vs, vts, tris


class Baker:
    def __init__(self, png):
        im = Image.open(png).convert("RGB")
        self.w, self.h = im.size
        self.px = np.asarray(im).astype(np.uint32)
        self.packed = (self.px[:, :, 0] << 16) | (self.px[:, :, 1] << 8) | self.px[:, :, 2]

    def uv2px(self, uv):
        return (uv[0] * self.w, (1.0 - uv[1]) * self.h)

    def region_colors(self, t):
        """Colours of texels whose centres fall inside UV triangle t (pixel coords).
        Falls back to the texel under the centroid for tiny triangles."""
        (x0, y0), (x1, y1), (x2, y2) = t
        minx = max(int(math.floor(min(x0, x1, x2))), 0)
        maxx = min(int(math.ceil(max(x0, x1, x2))), self.w - 1)
        miny = max(int(math.floor(min(y0, y1, y2))), 0)
        maxy = min(int(math.ceil(max(y0, y1, y2))), self.h - 1)
        cx = (x0 + x1 + x2) / 3.0; cy = (y0 + y1 + y2) / 3.0
        cen = self.packed[min(max(int(cy), 0), self.h - 1), min(max(int(cx), 0), self.w - 1)]
        if maxx < minx or maxy < miny:
            return np.array([cen])
        xs = np.arange(minx, maxx + 1) + 0.5
        ys = np.arange(miny, maxy + 1) + 0.5
        X, Y = np.meshgrid(xs, ys)
        def edge(ax, ay, bx, by):
            return (bx - ax) * (Y - ay) - (by - ay) * (X - ax)
        e0 = edge(x0, y0, x1, y1); e1 = edge(x1, y1, x2, y2); e2 = edge(x2, y2, x0, y0)
        inside = ((e0 >= -1e-6) & (e1 >= -1e-6) & (e2 >= -1e-6)) | ((e0 <= 1e-6) & (e1 <= 1e-6) & (e2 <= 1e-6))
        cols = self.packed[miny:maxy + 1, minx:maxx + 1][inside]
        if cols.size == 0:
            return np.array([cen])
        return cols

    def bake(self, P, UV, out, depth=0):
        t = [self.uv2px(uv) for uv in UV]
        cols = self.region_colors(t)
        vals, counts = np.unique(cols, return_counts=True)
        a = np.array(P[0]); b = np.array(P[1]); c = np.array(P[2])
        area = 0.5 * np.linalg.norm(np.cross(b - a, c - a))
        # Small faces (the voxel models): one colour per face. Use the
        # dominant texel colour when it clearly wins, else the mean.
        # Large faces (the 25-unit-wide ground strips): subdivide so stripes,
        # tufts and rails survive.
        if area < 0.25 or len(vals) == 1 or depth >= 10 or area < 0.004:
            if counts.max() / counts.sum() >= 0.6 or area >= 0.25:
                out.append((P, int(vals[np.argmax(counts)])))
            else:
                r = int(np.mean((cols >> 16) & 255)); g = int(np.mean((cols >> 8) & 255)); bb = int(np.mean(cols & 255))
                out.append((P, (r << 16) | (g << 8) | bb))
            return
        if counts.max() / counts.sum() > 0.97:
            out.append((P, int(vals[np.argmax(counts)])))
            return
        lens = [math.dist(t[i], t[(i + 1) % 3]) for i in range(3)]
        i = int(np.argmax(lens)); j = (i + 1) % 3; k = (i + 2) % 3
        mp = tuple((P[i][q] + P[j][q]) / 2 for q in range(3))
        muv = ((UV[i][0] + UV[j][0]) / 2, (UV[i][1] + UV[j][1]) / 2)
        self.bake((P[i], mp, P[k]), (UV[i], muv, UV[k]), out, depth + 1)
        self.bake((mp, P[j], P[k]), (muv, UV[j], UV[k]), out, depth + 1)


GROUND = {"grass_light", "grass_dark", "road_stripes", "road_blank", "river", "railroad"}


def build_mesh(objp, pngp, name=""):
    vs, vts, tris = parse_obj(os.path.join(A, objp))
    baker = Baker(os.path.join(A, pngp))
    pieces = []
    if name not in GROUND:
        # remastered: faces rebuilt from the painted texture (tools/remaster.py)
        import remaster
        pieces = remaster.remaster_mesh(vs, vts, tris, baker, baker.bake)
    else:
        for tri in tris:
            P = tuple(vs[i] for i, _ in tri)
            UV = tuple(vts[t] if t is not None else (0, 0) for _, t in tri)
            baker.bake(P, UV, pieces)
    # weld vertices, compute normals, drop degenerate triangles
    vmap, verts, out = {}, [], []
    for P, col in pieces:
        a = np.array(P[0]); b = np.array(P[1]); c = np.array(P[2])
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n = n / ln
        ids = []
        for p in P:
            key = tuple(round(x, 5) for x in p)
            if key not in vmap:
                vmap[key] = len(verts); verts.append(key)
            ids.append(vmap[key])
        out.append((ids, col, tuple(n)))
    vv = np.array(verts)
    return verts, out, vv.min(axis=0), vv.max(axis=0)


def decode_audio(fname):
    exe = "ffmpeg"
    try:
        import imageio_ffmpeg
        exe = imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    raw = subprocess.run([exe, "-v", "error", "-i", os.path.join(A, "audio", fname),
                          "-ac", "1", "-ar", "48000", "-f", "s16le", "-"],
                         check=True, capture_output=True).stdout
    s = np.frombuffer(raw, dtype="<i2")
    # trim leading/trailing near-silence so effects fire immediately
    nz = np.nonzero(np.abs(s) > 64)[0]
    if nz.size:
        s = s[max(nz[0] - 32, 0): nz[-1] + 1]
    return s


def main():
    os.makedirs(os.path.join(OUT, "source"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "include"), exist_ok=True)
    c = ['/* Generated by tools/convert.py - do not edit. */', '#include "assets_data.h"', '']
    h = ['/* Generated by tools/convert.py - do not edit. */', '#ifndef ASSETS_DATA_H', '#define ASSETS_DATA_H',
         '#include "mesh.h"', '']
    total_tris = 0
    names = []
    for name, objp, pngp in MODELS:
        verts, tris, mn, mx = build_mesh(objp, pngp, name)
        total_tris += len(tris)
        names.append(name)
        print(f"{name:14s} verts={len(verts):5d} tris={len(tris):5d} "
              f"min=({mn[0]:.2f},{mn[1]:.2f},{mn[2]:.2f}) max=({mx[0]:.2f},{mx[1]:.2f},{mx[2]:.2f})")
        c.append(f"static const float {name}_v[] = {{")
        c.append(",".join(f"{fl(x)},{fl(y)},{fl(z)}" for x, y, z in verts))
        c.append("};")
        c.append(f"static const MeshTri {name}_t[] = {{")
        c.append(",\n".join(f"{{{{{a},{b},{cc}}},0x{col:06X}u,{{{n[0]:.5f}f,{n[1]:.5f}f,{n[2]:.5f}f}}}}"
                            for (a, b, cc), col, n in tris))
        c.append("};")
        c.append(f"const Mesh mesh_{name} = {{ {name}_v, {name}_t, {len(verts)}, {len(tris)}, "
                 f"{{{fl(mn[0])},{fl(mn[1])},{fl(mn[2])}}}, {{{fl(mx[0])},{fl(mx[1])},{fl(mx[2])}}} }};")
        c.append("")
        h.append(f"extern const Mesh mesh_{name};")
    print("total triangles:", total_tris)

    h.append("")
    h.append("typedef struct { const short *data; int len; } SoundData;")
    h.append("enum {")
    total_samples = 0
    for name, fname in SOUNDS:
        s = decode_audio(fname)
        total_samples += len(s)
        print(f"sound {name:14s} {len(s)/48000:.2f}s")
        c.append(f"static const short snd_{name}_d[{len(s)}] = {{")
        rows = [",".join(str(int(x)) for x in s[i:i + 32]) for i in range(0, len(s), 32)]
        c.append(",\n".join(rows))
        c.append("};")
        h.append(f"  SND_{name.upper()},")
    h.append("  SND_COUNT")
    h.append("};")
    h.append("extern const SoundData sound_table[SND_COUNT];")
    c.append("const SoundData sound_table[SND_COUNT] = {")
    for name, fname in SOUNDS:
        c.append(f"  {{ snd_{name}_d, (int)(sizeof(snd_{name}_d)/sizeof(short)) }},")
    c.append("};")
    print(f"total audio: {total_samples/48000:.1f}s, {total_samples*2/1e6:.2f} MB")

    h.append("")
    h.append("#endif")
    open(os.path.join(OUT, "source", "assets_data.c"), "w").write("\n".join(c) + "\n")
    open(os.path.join(OUT, "include", "assets_data.h"), "w").write("\n".join(h) + "\n")


if __name__ == "__main__":
    main()
