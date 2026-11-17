"""Generates the Ice feature's tiling volume texture (R8G8_UNORM with mips, DX10 DDS).

R: fracture planes. Periodic Voronoi bisectors at two scales, domain warped so the planes wander, each edge with its own
   strength (some missing), and a soft halo because a plane seen through ice catches light across its width.
G: trapped air. Sparse bubbles from fine to coarse, denser where a low-frequency field says so.

Usage: python tools/ice/make_ice_volume.py [--out features/Ice/Shaders/Ice/IceVolume.dds] [--preview preview.png]
"""
import argparse
import struct
from pathlib import Path

import numpy as np

SIZE = 512


def periodic_noise(rng, size, cells):
    """Smooth tileable value noise in [0, 1]."""
    grid = rng.random((cells, cells))
    t = np.arange(size) / size * cells
    i0 = np.floor(t).astype(int)
    f = t - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    i0 %= cells
    rows = grid[i0][:, i0] * (1 - f)[None, :] + grid[i0][:, i1] * f[None, :]
    rows1 = grid[i1][:, i0] * (1 - f)[None, :] + grid[i1][:, i1] * f[None, :]
    return rows * (1 - f)[:, None] + rows1 * f[:, None]


def fbm(rng, size, base, octaves):
    total = np.zeros((size, size))
    amplitude = 1.0
    norm = 0.0
    for o in range(octaves):
        total += amplitude * periodic_noise(rng, size, base << o)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


def fracture_layer(rng, size, points, width, keep, warp):
    """Distance to the nearest Voronoi bisector on a torus, as a thin line plus halo with per-edge strength."""
    seeds = rng.random((points, 2))
    offsets = np.array([(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)], dtype=np.float64)
    tiled = (seeds[None, :, :] + offsets[:, None, :]).reshape(-1, 2)
    owner = np.tile(np.arange(points), len(offsets))

    # Each unordered pair of cells gets one strength, so a plane keeps its brightness along its length.
    pair_strength = rng.uniform(0.25, 1.0, (points, points))
    pair_strength = np.where(rng.random((points, points)) < keep, pair_strength, 0.0)
    pair_strength = np.maximum(pair_strength, pair_strength.T)

    ys, xs = np.mgrid[0:size, 0:size] / size
    wx = fbm(rng, size, 4, 3) - 0.5
    wy = fbm(rng, size, 4, 3) - 0.5
    px = (xs + warp * wx) % 1.0
    py = (ys + warp * wy) % 1.0

    out = np.zeros((size, size))
    rows_per_chunk = 32
    for r0 in range(0, size, rows_per_chunk):
        p = np.stack([px[r0:r0 + rows_per_chunk].ravel(), py[r0:r0 + rows_per_chunk].ravel()], -1)
        d2 = ((p[:, None, :] - tiled[None, :, :]) ** 2).sum(-1)
        nearest = np.argpartition(d2, 1, axis=1)[:, :2]
        d_sorted = np.take_along_axis(d2, nearest, 1)
        swap = d_sorted[:, 0] > d_sorted[:, 1]
        nearest[swap] = nearest[swap][:, ::-1]
        a = tiled[nearest[:, 0]]
        b = tiled[nearest[:, 1]]
        # Exact distance to the bisector between the two nearest seeds.
        dist = ((((p - b) ** 2).sum(-1) - ((p - a) ** 2).sum(-1)) / (2.0 * np.sqrt(((b - a) ** 2).sum(-1)) + 1e-9)) * size
        strength = pair_strength[owner[nearest[:, 0]], owner[nearest[:, 1]]]
        line = np.exp(-(dist / width) ** 2) + 0.12 * np.exp(-(dist / (width * 3.5)) ** 2)
        out[r0:r0 + rows_per_chunk] = (line * strength).reshape(-1, size)
    return out


def bubbles(rng, size, count):
    density = fbm(rng, size, 2, 3)
    out = np.zeros((size, size))
    ys, xs = np.mgrid[-8:9, -8:9]
    placed = 0
    while placed < count:
        x, y = rng.integers(0, size, 2)
        if rng.random() > density[y, x] ** 2 * 1.6:
            continue
        radius = rng.choice([0.8, 1.2, 1.8, 2.6, 3.6], p=[0.38, 0.28, 0.18, 0.11, 0.05])
        brightness = rng.uniform(0.45, 1.0)
        r = np.sqrt(xs ** 2 + ys ** 2)
        # A bubble is brighter at its rim than its center.
        disc = np.clip(1.0 - np.abs(r - radius * 0.7) / (radius * 0.6 + 0.6), 0.0, 1.0) * (r < radius + 1.0)
        out[np.ix_((y + ys[:, 0]) % size, (x + xs[0]) % size)] = np.maximum(out[np.ix_((y + ys[:, 0]) % size, (x + xs[0]) % size)], disc * brightness)
        placed += 1
    return out


def downsample(image):
    return 0.25 * (image[0::2, 0::2] + image[1::2, 0::2] + image[0::2, 1::2] + image[1::2, 1::2])


def write_dds_r8g8(path, levels):
    """DX10 DDS, DXGI_FORMAT_R8G8_UNORM (49), 2D, full mip chain."""
    height, width = levels[0].shape[:2]
    header = struct.pack(
        "<4sIIIIIII44sIIIIIIIIIIIII",
        b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8, height, width, width * 2, 0, len(levels), b"\0" * 44,
        32, 0x4, struct.unpack("<I", b"DX10")[0], 0, 0, 0, 0, 0,
        0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 49, 3, 0, 1, 0)
    with open(path, "wb") as f:
        f.write(header)
        f.write(dx10)
        for level in levels:
            f.write(np.clip(np.round(level * 255.0), 0, 255).astype(np.uint8).tobytes())


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=root / "features" / "Ice" / "Shaders" / "Ice" / "IceVolume.dds")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--seed", type=int, default=1337)
    args = parser.parse_args()

    rng = np.random.default_rng(args.seed)
    coarse = fracture_layer(rng, SIZE, 9, 1.1, 0.6, 0.16)
    fine = fracture_layer(rng, SIZE, 30, 0.8, 0.4, 0.08)
    cracks = np.clip(coarse + 0.45 * fine, 0.0, 1.0)
    air = np.clip(bubbles(rng, SIZE, 650), 0.0, 1.0)

    image = np.stack([cracks, air], -1)
    levels = [image]
    while levels[-1].shape[0] > 1:
        levels.append(downsample(levels[-1]))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_dds_r8g8(args.out, levels)
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes, {len(levels)} mips)")

    if args.preview:
        from PIL import Image
        tile = np.concatenate([np.concatenate([image] * 2, 1)] * 2, 0)
        rgb = np.stack([tile[..., 0], np.maximum(tile[..., 0], tile[..., 1]), np.maximum(tile[..., 0] * 0.8, tile[..., 1])], -1)
        Image.fromarray((np.clip(rgb, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)).save(args.preview)
        print(f"wrote {args.preview}")


if __name__ == "__main__":
    main()
