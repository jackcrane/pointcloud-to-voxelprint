# grid2ply_colorfix.py
# Usage: python grid2ply_colorfix.py joplin_grid.nc out.ply
# Strict voxel→point conversion, colored by reflectivity (dBZ), ASCII PLY.

import sys, math
import numpy as np
from netCDF4 import Dataset

if len(sys.argv) < 3:
    print("Usage: python grid2ply_colorfix.py joplin_grid.nc out.ply")
    sys.exit(1)

nc_path, ply_path = sys.argv[1], sys.argv[2]

# ---------- helpers ----------
def fetch_reflectivity(ds):
    # 1) flat variable
    if "reflectivity" in ds.variables:
        return ds.variables["reflectivity"][:]
    # 2) groups / attrs containing 'reflectivity' or dbZ hints
    for g in ds.groups.values():
        for vname, v in g.variables.items():
            ln = str(getattr(v, "long_name", "")).lower()
            sd = str(getattr(v, "standard_name", "")).lower()
            un = str(getattr(v, "units", "")).lower()
            if (
                "reflectivity" in vname.lower()
                or "reflectivity" in ln
                or "dbz" in (un + ln + sd)
            ):
                return v[:]
    # 3) fallback: first 3D float-like var
    for vname, v in ds.variables.items():
        if getattr(v, "ndim", 0) == 3 and getattr(v, "dtype", None) is not None:
            if v.dtype.kind in "fc":
                return v[:]
    raise KeyError("Could not locate reflectivity field in netCDF.")

def make_colormap(vals, vmin=-10.0, vmax=70.0):
    # Map vals → [0,1]
    t = (vals - vmin) / (vmax - vmin)
    t = np.clip(t, 0.0, 1.0).astype(np.float32)

    # 5-stop radar-like ramp: B → C → G → Y → R
    stops = np.array(
        [
            [0,   0, 130],  # blue
            [0, 180, 255],  # cyan
            [0, 180,   0],  # green
            [255,220,   0], # yellow
            [255,  0,   0], # red
        ],
        dtype=np.float32,
    )
    spos = np.array([0.0, 0.25, 0.5, 0.75, 1.0], dtype=np.float32)

    seg = np.searchsorted(spos, t, side="right") - 1
    seg = np.clip(seg, 0, len(spos) - 2)
    t0, t1 = spos[seg], spos[seg + 1]
    w = (t - t0) / (t1 - t0 + 1e-12)
    c0, c1 = stops[seg], stops[seg + 1]
    rgb = (c0 * (1.0 - w)[:, None] + c1 * w[:, None]).round()

    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    return rgb

# ---------- load ----------
ds = Dataset(nc_path, "r")
x = np.array(ds.variables["x"][:], dtype=np.float32)
y = np.array(ds.variables["y"][:], dtype=np.float32)
z = np.array(ds.variables["z"][:], dtype=np.float32)
refl = fetch_reflectivity(ds)  # expect shape (z, y, x)
ds.close()

# ---------- mask ----------
if np.ma.is_masked(refl):
    data = refl.filled(np.nan).astype(np.float32)
    valid = ~np.isnan(data)
else:
    data = np.array(refl, dtype=np.float32)
    valid = np.isfinite(data)

# If your reflectivity uses sentinel values (e.g., -9999), ignore them:
valid &= np.isfinite(data) & (data > -9000)

# ---------- coordinates ----------
# Expect (Z, Y, X) indexing for the field
Z, Y, X = np.meshgrid(z, y, x, indexing="ij")

px = X.ravel()[valid.ravel()].astype(np.float32)
py = Y.ravel()[valid.ravel()].astype(np.float32)
pz = Z.ravel()[valid.ravel()].astype(np.float32)
dbz = data.ravel()[valid.ravel()].astype(np.float32)
n = px.size
if n == 0:
    raise RuntimeError("No valid points to write (mask removed everything).")

# ---------- color ----------
rgb = make_colormap(dbz)                   # uint8 (N,3)
alpha = np.full((n, 1), 255, dtype=np.uint8)  # some viewers require alpha for color to show
rgba = np.concatenate([rgb, alpha], axis=1)   # (N,4)

# ---------- write ASCII PLY with RGBA ----------
with open(ply_path, "w", encoding="ascii", newline="\n") as f:
    f.write("ply\n")
    f.write("format ascii 1.0\n")  # <-- ASCII, not binary
    f.write(f"element vertex {n}\n")
    f.write("property float x\n")
    f.write("property float y\n")
    f.write("property float z\n")
    # Many viewers key on these exact names; include alpha to avoid greyscale fallbacks
    f.write("property uchar red\n")
    f.write("property uchar green\n")
    f.write("property uchar blue\n")
    f.write("property uchar alpha\n")
    f.write("end_header\n")

    # Stream in chunks to avoid huge Python string joins
    CHUNK = 500_000
    for i in range(0, n, CHUNK):
        j = min(i + CHUNK, n)
        block = [
            f"{float(px[k]):.3f} {float(py[k]):.3f} {float(pz[k]):.3f} {int(rgba[k,0])} {int(rgba[k,1])} {int(rgba[k,2])} {int(rgba[k,3])}\n"
            for k in range(i, j)
        ]
        f.writelines(block)

print(f"Wrote ASCII PLY: {ply_path}  ({n} points, colored via RGBA)")