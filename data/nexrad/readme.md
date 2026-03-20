# To process these files

> From the data/nexrad directory (until further notice)

## start by downloading from S3 (like this)

```
aws s3 cp s3://unidata-nexrad-level2/2025/05/16/KLSX/KLSX20250516_194943_V06 . --no-sign-request
```

## Enter the python venv:

```
source .venv/bin/activate
```

(or create)

```
python3 -m venv .venv
source .venv/bin/activate
```

## Install requirements

```
pip install arm_pyart netCDF4 numpy
```

## Convert to a grid

Note: You might want to change the grid limits to better fit your data.

<details>
<summary>
Default grid limits
</summary>


```py
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
```


</details>

```
python grid_nexrad.py <input_file> <output_basename>
```

```
python grid_nexrad.py KLSX20250516_194943_V06__STL_RAW grid_stl
```

## Convert the grid to a point cloud

```
python grid2ply.py <input_nc_file> <output_filename>
```

```
python grid2ply.py grid_stl.nc grid_stl_colored.ply
```

> From the repo root

## Convert the ply to slices

```
node index.js <input_ply_file> <output_dir>
```

```
node index.js data/nexrad/grid_stl_colored.ply out_stl_color
```

## Process the slices into PJ Voxel Print slices

> This step requires the `pj-voxelprint-colorwheels-v3` repo to be available as a file sibling to this `pointcould` repo.


```
../pj-voxelprint-colorwheels-v3/4/run_profiles.sh <input_dir> <output_dir>
```

```
../pj-voxelprint-colorwheels-v3/4/run_profiles.sh out_stl_color nexrad_stl_color
```

## Leave the Python venv

```
deactivate
```