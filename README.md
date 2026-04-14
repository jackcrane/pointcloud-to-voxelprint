# pointcloud

This repository contains Node.js scripts for converting point-cloud and volume-derived data into:

- quantized PLY point clouds
- RGBA PNG slice stacks
- chamfered slice stacks for print preparation

It is not structured as a library or packaged CLI. Most workflows are driven by direct `node` execution against individual scripts, and several physical parameters are hard-coded in source.

## Scope

The main workflows in this repository are:

- `index.js`: original JavaScript slicer reference
- `slice/`: native C rewrite of the PLY-to-PNG slicer, built as `bin/slice`
- `bin/xsection`: native cross-section extractor for existing slice stacks
- `bin/fillRegion`: native region painter for existing slice stacks
- `bin/shadow`: native directional color replacement for existing slice stacks
- `translate/translate.c`: stream a colorized LAS into an ASCII PLY
- `quantize/quantize.js`: quantize an input PLY into a fixed physical voxel grid and write a new PLY
- `chamfer.js`: remove material from the edges/corners of an RGBA PNG slice stack using a physical chamfer radius
- `data/ct/*`: exploratory NRRD-to-PLY conversion utilities for CT-style voxel data
- `util/generate-pointcloud-sphere.js`: generate a synthetic colored sphere point cloud for testing

## Runtime Requirements

- Node.js 20+
- ESM enabled at the repository root via [`package.json`](./package.json)
- Top-level dependencies declared in [`package.json`](./package.json)
- `sharp` available in the environment if you want to run [`chamfer.js`](./chamfer.js)
- a C compiler such as `cc` if you want to build the native tools

Notes:

- There are no meaningful `npm` scripts in the root package. Run the files directly with `node`.
- [`chamfer.js`](./chamfer.js) imports `sharp`, but `sharp` is not currently declared in the root `package.json`.
- The repository already contains checked-in output folders and sample artifacts. They are useful as references, not as authoritative build outputs.

## Repository Layout

- [`index.js`](./index.js): PLY to PNG-slice conversion
- [`translate/translate.c`](./translate/translate.c): streaming LAS to PLY converter for large colorized LAS files
- [`quantize/quantize.js`](./quantize/quantize.js): streaming quantizer for large PLY files
- [`chamfer.js`](./chamfer.js): chamfer processor for PNG slice directories
- [`classes/ply.js`](./classes/ply.js): in-memory PLY loader with nearest-neighbor queries
- [`classes/png.js`](./classes/png.js): minimal PNG writer used by the slicer
- [`util/ply-format.js`](./util/ply-format.js): PLY header parsing and numeric readers
- [`util/generate-pointcloud-sphere.js`](./util/generate-pointcloud-sphere.js): synthetic test data generator
- [`data/ct/index.js`](./data/ct/index.js): example CT conversion entrypoint
- [`data/ct/nrrdToPLY.js`](./data/ct/nrrdToPLY.js): NRRD payload to ASCII PLY conversion
- [`sphere/`](./sphere): checked-in sample slice outputs

## Data Conventions

### PLY Support

The main PLY utilities support:

- `format ascii 1.0`
- `format binary_little_endian 1.0`

Required vertex properties:

- `x`
- `y`
- `z`

Optional color properties:

- `red` or `r`
- `green` or `g`
- `blue` or `b`
- `alpha` or `a`

Color normalization rules in [`util/ply-format.js`](./util/ply-format.js):

- values in `[0, 1]` are scaled to `0..255`
- other numeric values are clamped to `0..255`

### PNG Slice Semantics

For slice stacks processed by [`chamfer.js`](./chamfer.js):

- files must be PNG
- slices are ordered by natural filename sort, so `out_2.png` comes before `out_10.png`
- alpha `0` means void
- alpha `> 0` means material

### Physical Units

The print-oriented scripts use physical units directly in source.

Current defaults in the native slicer (`./bin/slice`):

- XY resolution: `300 DPI`
- layer height: `27,000 nm`
- build volume: `2.5" x 1.0" x 0.75"`
- voxel radius: `0.010"`

Current defaults in [`quantize/quantize.js`](./quantize/quantize.js):

- target size: `0.5" x 0.5" x 0.25"`
- target resolution: `300 x 300 x 300 DPI-equivalent`

Current defaults in [`chamfer.js`](./chamfer.js):

- XY resolution: `300 DPI`
- layer height: `27,000 nm`

Those values now live in the slice TOML config instead of source edits.

## Usage Reference

### 1. Generate a Test Sphere

Generate a simple colored ASCII PLY for local testing:

```bash
node util/generate-pointcloud-sphere.js
```

By default this writes `sphere.ply` in the repository root.

The generator can also be imported and customized from another script if you want control over:

- point count
- sphere radius
- center
- surface vs volume sampling
- random vs gradient coloring

### 2. Slice a PLY into PNG Layers

Build:

```bash
sudo apt-get update
sudo apt-get install -y libcpptoml-dev
make slice
```

Command:

```bash
./bin/slice --config <slice.toml> [input.ply] [output_dir]
```

Example:

```bash
./bin/slice --config slice/slice.example.toml
```

What the native slicer does:

- loads the entire PLY into memory through a native ASCII/binary parser
- computes point bounds
- maps the model bounds onto the configured physical build size
- builds a kd-tree for nearest-point lookup
- rasterizes each Z layer into an RGBA PNG
- writes files as `out_<layer>.png`

Operational details:

- slicer settings now live in a TOML config file; see [`slice/slice.example.toml`](./slice/slice.example.toml)
- optional top-level `kd_cache = "path/to/tree.kdcache"` reuses a saved kd-tree topology and writes it on first build
- the output directory is created if missing
- the input file must exist
- CLI positional paths can override `input.path` and `output.directory` from the TOML file
- every layer is generated sequentially
- the background is flood-filled to `rgba(247, 247, 247, 128)` before point rendering
- per-pixel occupancy is based on nearest-point distance against six directional voxel radii
- colored pixels render fully opaque; only the flood-filled background remains semi-transparent

Performance and scaling:

- this path is memory-heavy because it loads the full point cloud and kd-tree in-process
- it is appropriate for moderate inputs, not for arbitrarily large scans

### 2a. Build a Cross-Section from Existing Slice PNGs

Build:

```bash
make xsection
```

Command:

```bash
./bin/xsection --config <slice.toml> --plane <xz|yz> --dist <index> <output.png>
```

Example:

```bash
./bin/xsection --config slice/slice.example.toml --plane xz --dist 200 cross_section.png
```

What `./bin/xsection` does:

- loads the same slice TOML file used by `./bin/slice`
- reads `output.directory` from that config and scans `out_<layer>.png` files
- computes horizontal DPI from `raster.dpi`
- computes vertical DPI from `raster.layer_height_nm` as `25,400,000 / layer_height_nm`
- for `xz`, copies one source row from every layer into the output image
- for `yz`, copies one source column from every layer into the output image
- treats `--dist` as a source-image row or column index counted from the top-left
- places `out_0.png` on the bottom row of the destination image so the vertical stack matches slice order

Important behaviors:

- the final positional argument is the output PNG path
- all source slices must have identical dimensions
- the output height equals the number of slice PNGs found in the configured output directory
- the output width is the source width for `xz` and the source height for `yz`
- the PNG reader currently supports the uncompressed RGBA PNGs written by the native slicer

### 2b. Fill a Polygon Region Across an Existing Slice Stack

Build:

```bash
make fillRegion
```

Command:

```bash
./bin/fillRegion --config <fillRegion.toml>
```

Example:

```bash
./bin/fillRegion --config fillRegion/fillRegion.example.toml
```

What `./bin/fillRegion` does:

- reads `input.directory`, `output.directory`, optional `layer.first` / `layer.last`, `color`, and `[[points]]` from its TOML config
- scans `out_<layer>.png` files from the input directory and writes the processed stack to the output directory
- preserves layer filenames and image dimensions
- copies untouched layers through unchanged
- fills affected layers by overwriting pixels inside the configured polygon with the configured RGBA color

Point modes:

- XY mode: omit `z` from every point to fill the same 2D polygon on every layer
- XYZ mode: include `z` on every point to define polygon keyframes by layer number
- in XYZ mode, points are grouped by shared `z` and interpolated between adjacent z-groups
- every z-group must contain the same number of vertices in the same winding order

Important behaviors:

- the PNG reader supports the same uncompressed RGBA PNGs written by the native slicer
- `layer.first` and `layer.last` are optional inclusive limits for where painting is allowed
- layers outside the configured layer range are copied without painting
- layers outside the XYZ keyframe range are copied without painting
- `z` is interpreted as the source layer number, while `x` and `y` are measured from the image top-left

### 2c. Shadow an Existing Slice Stack from the Top or Bottom

Build:

```bash
make shadow
```

Command:

```bash
./bin/shadow --config <shadow.toml>
./bin/shadow --setColor <r,g,b[,a]> --replaceColor <r,g,b[,a]> --from <top|bottom> <input_dir> <output_dir>
```

Example:

```bash
./bin/shadow --setColor 0,128,32 --replaceColor 1,1,1 --from top input_dir output_dir
```

Config example:

```bash
./bin/shadow --config shadow/shadow.example.toml
```

What `./bin/shadow` does:

- scans `out_<layer>.png` files from the input directory and writes the processed stack to the output directory
- keeps a per-pixel stop map across the whole stack
- starts from `out_0.png` when `--from bottom` is used, or from the highest numbered layer when `--from top` is used
- for each exposed pixel position, replaces `replaceColor` with `setColor`
- marks a pixel position as blocked forever once a layer contains any non-matching color there

Important behaviors:

- all source slices must have identical dimensions
- RGB colors default alpha to `255`; RGBA colors compare and write all four channels
- `--config` reads `input.directory`, `output.directory`, `set_color`, `replace_color`, and `from`
- command-line flags and positional paths override values from the config file
- the PNG reader supports the same uncompressed RGBA PNGs written by the native slicer

### 3. Translate a Colorized LAS to PLY

Build the native translator:

```bash
make translate
```

Command:

```bash
./bin/translate [--log-interval 10000000] <input.las> <output.ply>
```

Example:

```bash
./bin/translate scans/site_scan.las prepared/site_scan_ascii.ply
```

What [`translate/translate.c`](./translate/translate.c) does:

- reads the LAS public header and validates that the file is an uncompressed colorized LAS
- supports LAS point formats `2`, `3`, `5`, `7`, `8`, and `10`
- streams point records in batches instead of loading the cloud into memory
- applies the LAS scale and offset to recover source-space coordinates
- writes an ASCII PLY with `double` `x/y/z` and `ushort` `red/green/blue`
- preserves 16-bit LAS RGB values instead of downsampling them to 8-bit

Important behaviors:

- input and output paths must differ
- the output directory is created if missing
- the translator does not support compressed `LAZ`
- periodic progress logs are emitted every `10,000,000` points by default; use `--log-interval N` to change that or `--log-interval 0` to disable them
- the output is intentionally ASCII because that is the requested interchange format, so it will be substantially larger and slower to write than a binary PLY

### 4. Quantize a Dense PLY

Build the native quantizer:

```bash
make quantize
```

Command:

```bash
./bin/quantize [--log-interval 10000000] [--temp-dir DIR] [--steps bounds,shard,reduce|shard,reduce|reduce] <input.ply> <output.ply>
```

Example:

```bash
./bin/quantize --log-interval 10000000 data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
```

Partial-run examples:

```bash
./bin/quantize --temp-dir ./quantize-cache data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
./bin/quantize --temp-dir ./quantize-cache --steps shard,reduce data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
./bin/quantize --temp-dir ./quantize-cache --steps reduce data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
```

What [`quantize/quantize.c`](./quantize/quantize.c) does:

- reads the input PLY header
- supports ASCII and binary little-endian vertex streams
- scans the full file once to determine bounds
- maps points into a fixed 3D grid derived from the hard-coded physical target size and DPI
- shards quantized cells across temporary binary files
- reduces each shard to one output vertex per occupied cell
- writes a binary little-endian PLY with RGBA color

Important behaviors:

- input and output paths must differ
- the output directory is created if missing
- temporary working files are retained after the run; the binary prints the temp directory path before exit
- `--temp-dir DIR` without `--steps` means "put this run's temp files here"
- `--temp-dir DIR` with `--steps shard,reduce` means "rebuild temp files here, then continue"
- `--temp-dir DIR` with `--steps reduce` means "reuse existing temp files from here"
- if `--temp-dir` is omitted, the binary creates a fresh retained temp directory under the current working directory
- retained shard files are plain text records with `cell_id x y z r g b a packed_color`
- if the input contains no usable numeric vertices, the script writes an empty PLY
- output coordinates are written in the normalized target-space dimensions, not the original source-space bounds
- reduce writes one retained binary staging file per shard as `reduced-000.bin` through `reduced-127.bin`
- existing shard cache files remain the same retained plain-text records, so `--steps reduce` can reuse a previously generated cache without rerunning sharding
- progress bars were removed; the native binary prints per-stage timings and the same overall timing summary instead
- periodic progress logs are emitted every `10,000,000` records by default; use `--log-interval N` to change that or `--log-interval 0` to disable them
- `--steps` always writes the final output when applicable
- `--steps bounds,shard,reduce` is the default full run
- `--steps shard,reduce` rescans bounds from the input, reruns sharding into `--temp-dir`, then reduces and writes
- `--steps reduce` reuses existing shard files from `--temp-dir`, then reduces and writes
- `--temp-dir` is required when `--steps` starts at `shard` or `reduce`

Current implementation characteristics:

- shard count: `128`
- shard record size: `12` bytes
- output vertex record size: `16` bytes
- streaming chunk size: `1 MiB`

This binary is the most suitable path in the repository for large PLY inputs because it streams vertex data instead of building a full in-memory point set.

### 5. Chamfer a PNG Slice Stack

Command:

```bash
node chamfer.js <inputDir> <outputDir> <radiusInches> [--debug]
```

Example:

```bash
node chamfer.js sphere/sphere_out sphere/prepared 0.02
```

Debug overlay example:

```bash
node chamfer.js sphere/sphere_out sphere/prepared_debug 0.02 --debug
```

What [`chamfer.js`](./chamfer.js) does:

- scans all slices to compute one global axis-aligned material bounding box
- treats any pixel with alpha `> 0` as material
- computes XY and Z distances in inches from the bounding box faces
- removes material near the 12 box edges and 8 corners according to a linear chamfer rule
- writes a new PNG stack to the output directory

Debug mode:

- draws black hairlines for within-slice chamfer boundaries
- draws black dots where chamfering starts across Z
- draws the debug marks on the material side of the boundary

Important assumptions:

- all slices in the directory must have identical dimensions
- the script only considers PNG files
- if the input directory contains no material, files are copied through unchanged

### 5. Convert CT / NRRD Data to PLY

The CT utilities under [`data/ct/`](./data/ct) are exploratory and currently rely on hard-coded paths and parameters.

Primary files:

- [`data/ct/index.js`](./data/ct/index.js): example entrypoint
- [`data/ct/nrrd-utils.js`](./data/ct/nrrd-utils.js): basic NRRD reader
- [`data/ct/nrrdToPLY.js`](./data/ct/nrrdToPLY.js): writes ASCII PLY from voxel data
- [`data/ct/histogram.js`](./data/ct/histogram.js): histogram/statistics helper

Current example behavior in [`data/ct/index.js`](./data/ct/index.js):

- reads `drone-data/two.nrrd`
- passes the file payload into [`data/ct/nrrdToPLY.js`](./data/ct/nrrdToPLY.js)
- uses:
  - threshold `15`
  - stride `2`
  - output path `ct_cloud.ply`

Command:

```bash
cd data/ct
node index.js
```

NRRD conversion notes:

- [`data/ct/nrrdToPLY.js`](./data/ct/nrrdToPLY.js) expects gzipped uint8 voxel payloads
- output is ASCII PLY
- density values are mapped to RGB using a fixed multi-stop color ramp
- density also maps inversely to alpha, so denser voxels become less opaque
- point decimation is done with the `stride` parameter by skipping voxel coordinates not divisible by the stride

## Implementation Notes

### `slice/`

[`slice/`](./slice/) is the native slicer implementation. It provides:

- ASCII and binary little-endian PLY parsing
- color normalization for integer and float color fields
- balanced kd-tree nearest-point lookup
- an in-repo PNG writer with uncompressed DEFLATE blocks
- CLI flags covering the old `index.js` physical sizing constants

Build it with:

```bash
make slice
```

Run it with:

```bash
./bin/slice --help
```

### `classes/ply.js`

[`classes/ply.js`](./classes/ply.js) is a minimal loader with:

- ASCII and binary little-endian parsing
- optional color extraction
- precomputed bounds
- balanced kd-tree construction
- nearest-point queries with isotropic or anisotropic distance cutoffs

This is used directly by [`index.js`](./index.js) and by the older experimental [`worker.js`](./worker.js).

### `classes/png.js`

[`classes/png.js`](./classes/png.js) is a custom PNG writer that:

- writes 8-bit RGBA PNGs
- uses no PNG row filtering
- uses a zlib wrapper around uncompressed DEFLATE blocks
- exposes direct pixel writes and a flood-fill helper

This keeps PNG writing local to the repository without adding an image encoding dependency for the slicer path.

### `worker.js`

[`worker.js`](./worker.js) appears to be an older experimental worker-thread slicer. It is not wired into the main workflows.

## Known Limitations

- There is no automated test suite.
- There are no production-ready package scripts.
- Key print and quantization settings are hard-coded in source instead of exposed as CLI flags.
- [`chamfer.js`](./chamfer.js) depends on `sharp` but that dependency is not declared in the root package manifest.
- Several directories in the repository are generated outputs or archived experiment data, not source modules.
- The top-level slicer in [`index.js`](./index.js) is computationally expensive because it performs a nearest-point query for every output pixel in every layer.

## Suggested Working Order

For a clean end-to-end workflow:

1. Generate or obtain a colored PLY.
2. If the cloud is too dense or irregular, run [`quantize/quantize.js`](./quantize/quantize.js).
3. Slice the PLY with [`index.js`](./index.js).
4. Post-process the slice stack with [`chamfer.js`](./chamfer.js) if edge relief is needed.

For CT-derived data:

1. Convert NRRD data to PLY under [`data/ct/`](./data/ct).
2. Optionally quantize the resulting PLY.
3. Slice and post-process as above.
