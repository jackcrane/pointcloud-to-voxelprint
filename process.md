# Install deps

```sh
sudo apt-get install ffmpeg imagemagick
sudo ln -s /usr/bin/convert /usr/bin/magick
```

# slice

```toml
# Example configuration for the native slicer.
#
# Run with:
#   ./bin/slice --config slice/slice.example.toml
#
# You can still override the input and output paths on the command line:
#   ./bin/slice --config slice/slice.example.toml data/sphere.ply sphere/out

# Optional cache file for the kd-tree topology.
# If this file exists, slice will load the kd-tree from it instead of rebuilding.
# If it does not exist yet, slice will build the tree once and save it here.
kd_cache = "/media/sf_Voxel_Ubuntu/ozark_colorized_jc_quantized_rotated-modified.kdcache"

[input]
# Source point cloud in PLY format.
path = "/media/sf_Voxel_Ubuntu/ozark_colorized_jc_quantized_rotated-modified.ply"

[output]
# Directory where out_<layer>.png files will be written.
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_0"

[raster]
# Horizontal XY resolution.
dpi = 300.0

# Distance between layers in nanometers.
layer_height_nm = 27000.0

[physical]
# Multiplies the default build size and default voxel radii.
multiplier = 1.0

# Physical size of the print in inches.
# Use "auto" for any dimension you want inferred from the model bounds.
x_in = 2.5
y_in = "auto"
z_in = "auto"

# Used when any dimension above is set to "auto".
longest_side_in = 2.5

# Extra XY padding around the model bounds as a fraction of the model size.
padding_ratio = 0.0

[sampling.radius_inches]
# Match radii in inches, split by axis and direction.
# Bigger values fill more aggressively in that direction.
x_positive = 0.01
x_negative = 0.01
y_positive = 0.01
y_negative = 0.01
z_positive = 0.1
z_negative = 0.01

[logging]
# Startup progress interval in points. Use 0 to disable startup progress logs.
interval = 100000

```

```sh
./bin/slice --config slice/slice.example.toml
```

# add "shadows"

## add white bottom

```toml
from = "bottom"

[input]
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_0"

[output]
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom"
set_color = [255,255,255]
replace_color = [247, 247, 247, 128]

```

```sh
./bin/shadow --config shadow/white-bottom.toml
```


## add void top

```toml
from = "top"

[input]
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom"

[output]
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-transvoid"
set_color = [0, 0, 0, 0]
replace_color = [247, 247, 247, 128]
```

```sh
./bin/shadow --config shadow/void-air.toml
```

# fillRegion regions we don't want to print

## Top right

```toml
[input]
# Directory containing out_<layer>.png files.
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-transvoid"

[output]
# Directory where the processed PNG stack will be written.
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-shaped"

# Fill color as [r, g, b] or [r, g, b, a].
color = [0, 0, 0, 0]

# XY mode:
# Omit z to fill the same polygon on every layer.
#
[[points]]
x = 750
y = 421

[[points]]
x = 639
y = 19

[[points]]
x = 109
y = 0

[[points]]
x = 750
y = 0
```

```sh
./bin/fillRegion --config fillRegion/fillTR.toml
```

## Bottom left

This step repairs the missing points on the lake surface in the bottom (front?) left of the dataset

```toml
[input]
# Directory containing out_<layer>.png files.
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-shaped"

[output]
# Directory where the processed PNG stack will be written.
directory = "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-fixedlake"

# Fill color as [r, g, b] or [r, g, b, a].
color = [0, 0, 0, 0]

# XY mode:
# Omit z to fill the same polygon on every layer.
#
[[points]]
x = 0
y = 195

[[points]]
x = 31
y = 397

[[points]]
x = 37
y = 431

[[points]]
x = 0
y = 431
```

```sh
./bin/fillRegion --config fillRegion/fillBL.toml
```

# Pad the bottom to thicken lake surface

```
./pad.sh --number 50 "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-fixedlake" "/media/sf_Voxel_Ubuntu/ozark_slices_1/rgb_final"
```

# Generate video clip

```
./makevideo.sh --length 15 "/media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-fixedlake" "/media/sf_Voxel_Ubuntu/ozark_slices_1/rgb_final.mp4"
```

# rgb to voxel print util

## Find os icc profile

```sh
find /usr/share/color -iname '*srgb*.icc' 2>/dev/null
```

```sh
./run_profiles.sh /usr/share/color/icc/colord/sRGB.icc /media/sf_Voxel_Ubuntu/ozark_slices_1/whitebottom-fixedlake /media/sf_Voxel_Ubuntu/ozark_slices_1/vox
```