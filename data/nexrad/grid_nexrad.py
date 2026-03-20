# grid_nexrad.py
# Usage: python grid_nexrad.py KSGF20110522_223858_V03 joplin_grid
import sys
import pyart

if len(sys.argv) < 3:
    print("Usage: python grid_nexrad.py <input_file> <output_basename>")
    sys.exit(1)

inbase = sys.argv[1]  # input radar file (already gunzipped)
outbase = sys.argv[2]  # output base name (without extension)

radar = pyart.io.read(inbase)

# ~150 km domain horizontally, 0–20 km vertically; tweak as desired
grid = pyart.map.grid_from_radars(
    radar,
    grid_shape=(41, 301, 301),                       # z, y, x
    grid_limits=((0.0, 20000.0),                     # meters AGL (z)
                 (-150000.0, 150000.0),              # y
                 (-150000.0, 150000.0)),             # x
    fields=['reflectivity'],
    weighting_function='Barnes2',
    roi_func='constant', constant_roi=1000.0         # ~1 km ROI
)

out_path = f"{outbase}.nc"
pyart.io.write_grid(out_path, grid)
print(f"Wrote {out_path}")