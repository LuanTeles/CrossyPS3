#!/usr/bin/env python3
"""Remaster pass: rebuild each model face from its painted texture as clean
axis-aligned colour rectangles.

The original models are low-poly boxes whose detail (windows, lights, eyes,
bark, stripes) is painted into a texture atlas. The PS3 build has no texture
sampling, so the old converter approximated the paint by splitting triangles
along their longest edge, which produced jagged diagonal streaks. Here:

  1. Triangles that share a plane and the same UV->3D mapping are grouped
     (the two halves of each textured rectangle, or several).
  2. If the group's UV footprint is a rectangle, the texels inside it are
     merged greedily into same-colour rectangles, each emitted as two
     triangles mapped back to 3D through the shared affine map. Exactly
     the painted look, crisp edges, nothing diagonal.
  3. Anything else falls back to the previous method.
  4. Near-identical colours are snapped to the model's dominant palette
     colour (cleans texture noise without changing the look).

Output: same Mesh format as convert.py.
"""
import math
import numpy as np


def _affine(P, UV):
    """3D = M @ (u, v, 1) for this triangle, or None if UV is degenerate."""
    U = np.array([[UV[0][0], UV[0][1], 1.0], [UV[1][0], UV[1][1], 1.0], [UV[2][0], UV[2][1], 1.0]])
    if abs(np.linalg.det(U)) < 1e-12:
        return None
    return np.linalg.solve(U, np.array(P)).T  # 3x3


def _plane_key(M):
    return tuple(np.round(M.flatten(), 4))


def _snap_palette(grid, counts_all):
    """Snap colours within a small distance of a much more common colour."""
    common = sorted(counts_all.items(), key=lambda kv: -kv[1])
    out = grid.copy()
    pal = [c for c, _ in common]
    rgb = {c: np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], dtype=np.int32) for c in pal}
    snap = {}
    for i, c in enumerate(pal):
        for d in pal[:i]:
            if counts_all[d] >= 4 * counts_all[c] and np.abs(rgb[c] - rgb[d]).max() <= 6:
                snap[c] = d
                break
    if snap:
        flat = out.reshape(-1)
        for k, v in snap.items():
            flat[flat == k] = v
    return out


def _greedy_rects(grid, valid):
    """Merge same-colour cells into rectangles. grid/valid: HxW arrays."""
    H, W = grid.shape
    used = ~valid.copy()
    rects = []
    for y in range(H):
        x = 0
        while x < W:
            if used[y, x]:
                x += 1
                continue
            c = grid[y, x]
            x1 = x
            while x1 + 1 < W and not used[y, x1 + 1] and grid[y, x1 + 1] == c:
                x1 += 1
            y1 = y
            while y1 + 1 < H and np.all(~used[y1 + 1, x:x1 + 1]) and np.all(grid[y1 + 1, x:x1 + 1] == c):
                y1 += 1
            used[y:y1 + 1, x:x1 + 1] = True
            rects.append((x, y, x1 + 1, y1 + 1, int(c)))
            x = x1 + 1
    return rects


def remaster_mesh(vs, vts, tris, baker, fallback_bake):
    """vs: verts, vts: uvs, tris: [((vi,ti),(vi,ti),(vi,ti))]. Returns list of (P, colour)."""
    groups = {}
    order = []
    for tri in tris:
        P = [vs[i] for i, _ in tri]
        UV = [vts[t] if t is not None else (0.0, 0.0) for _, t in tri]
        M = _affine(P, UV)
        if M is None:
            order.append(("fb", P, UV))
            continue
        k = _plane_key(M)
        if k not in groups:
            groups[k] = {"M": M, "tris": []}
            order.append(("g", k))
        groups[k]["tris"].append((P, UV))

    pieces = []
    for item in order:
        if item[0] == "fb":
            fallback_bake(tuple(map(tuple, item[1])), tuple(item[2]), pieces)
            continue
        g = groups[item[1]]
        M = g["M"]
        uvs = np.array([uv for _, UV in g["tris"] for uv in UV])
        u0, v0 = uvs.min(axis=0)
        u1, v1 = uvs.max(axis=0)
        # footprint must be the full UV rectangle (sum of triangle areas == bbox area)
        area = sum(abs((UV[1][0] - UV[0][0]) * (UV[2][1] - UV[0][1]) - (UV[2][0] - UV[0][0]) * (UV[1][1] - UV[0][1])) / 2
                   for _, UV in g["tris"])
        if (u1 - u0) * (v1 - v0) <= 0 or abs(area - (u1 - u0) * (v1 - v0)) > 1e-6 * max(1.0, area) + 1e-9:
            for P, UV in g["tris"]:
                fallback_bake(tuple(map(tuple, P)), tuple(UV), pieces)
            continue
        # texel grid covering the rectangle (pixel space, y down)
        px0, px1 = u0 * baker.w, u1 * baker.w
        py0, py1 = (1 - v1) * baker.h, (1 - v0) * baker.h
        cx0, cx1 = int(math.floor(px0 + 1e-6)), int(math.ceil(px1 - 1e-6))
        cy0, cy1 = int(math.floor(py0 + 1e-6)), int(math.ceil(py1 - 1e-6))
        cx0, cy0 = max(cx0, 0), max(cy0, 0)
        cx1, cy1 = min(max(cx1, cx0 + 1), baker.w), min(max(cy1, cy0 + 1), baker.h)
        grid = baker.packed[cy0:cy1, cx0:cx1].copy()
        valid = np.ones(grid.shape, dtype=bool)
        vals, counts = np.unique(grid, return_counts=True)
        grid = _snap_palette(grid, dict(zip(vals.tolist(), counts.tolist())))
        for (gx0, gy0, gx1, gy1, col) in _greedy_rects(grid, valid):
            # rect in pixel space, clipped to the face's exact UV rectangle
            ax0 = max(cx0 + gx0, px0); ax1 = min(cx0 + gx1, px1)
            ay0 = max(cy0 + gy0, py0); ay1 = min(cy0 + gy1, py1)
            if ax1 - ax0 <= 1e-9 or ay1 - ay0 <= 1e-9:
                continue
            ua, ub = ax0 / baker.w, ax1 / baker.w
            va, vb = 1 - ay1 / baker.h, 1 - ay0 / baker.h
            c00 = tuple(M @ np.array([ua, va, 1.0]))
            c10 = tuple(M @ np.array([ub, va, 1.0]))
            c11 = tuple(M @ np.array([ub, vb, 1.0]))
            c01 = tuple(M @ np.array([ua, vb, 1.0]))
            # keep the original winding: compare with the source triangle's normal
            P0 = np.array(g["tris"][0][0])
            n_ref = np.cross(P0[1] - P0[0], P0[2] - P0[0])
            n_new = np.cross(np.array(c10) - np.array(c00), np.array(c11) - np.array(c00))
            if np.dot(n_ref, n_new) >= 0:
                pieces.append(((c00, c10, c11), col))
                pieces.append(((c00, c11, c01), col))
            else:
                pieces.append(((c00, c11, c10), col))
                pieces.append(((c00, c01, c11), col))
    return pieces
