# pointcloud

This repository contains Node.js scripts for converting point-cloud and volume-derived data into:

- quantized PLY point clouds
- RGBA PNG slice stacks
- chamfered slice stacks for print preparation

It is not structured as a library or packaged CLI. Most workflows are driven by direct `node` execution against individual scripts, and several physical parameters are hard-coded in source.

## Scope

The main workflows in this repository are:

- `index.js`: slice a colored PLY point cloud into a stack of RGBA PNG layers
- `quantize.js`: quantize an input PLY into a fixed physical voxel grid and write a new PLY
- `chamfer.js`: remove material from the edges/corners of an RGBA PNG slice stack using a physical chamfer radius
- `data/ct/*`: exploratory NRRD-to-PLY conversion utilities for CT-style voxel data
- `util/generate-pointcloud-sphere.js`: generate a synthetic colored sphere point cloud for testing

## Runtime Requirements

- Node.js 20+
- ESM enabled at the repository root via [`package.json`](./package.json)
- Top-level dependencies declared in [`package.json`](./package.json)
- `sharp` available in the environment if you want to run [`chamfer.js`](./chamfer.js)

Notes:

- There are no meaningful `npm` scripts in the root package. Run the files directly with `node`.
- [`chamfer.js`](./chamfer.js) imports `sharp`, but `sharp` is not currently declared in the root `package.json`.
- The repository already contains checked-in output folders and sample artifacts. They are useful as references, not as authoritative build outputs.

## Repository Layout

- [`index.js`](./index.js): PLY to PNG-slice conversion
- [`quantize.js`](./quantize.js): streaming quantizer for large PLY files
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

Current defaults in [`index.js`](./index.js):

- XY resolution: `300 DPI`
- layer height: `27,000 nm`
- build volume: `1.5" x 1.5" x 0.75"`
- voxel radius: `0.008"`

Current defaults in [`quantize.js`](./quantize.js):

- target size: `0.5" x 0.5" x 0.25"`
- target resolution: `300 x 300 x 300 DPI-equivalent`

Current defaults in [`chamfer.js`](./chamfer.js):

- XY resolution: `300 DPI`
- layer height: `27,000 nm`

If those dimensions are not appropriate for your job, edit the constants in source before running the script.

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

Command:

```bash
node index.js <input.ply> <output_dir>
```

Example:

```bash
node index.js sphere.ply sphere/sphere_out
```

What [`index.js`](./index.js) does:

- loads the entire PLY into memory through [`classes/ply.js`](./classes/ply.js)
- computes point bounds
- maps the model bounds onto the hard-coded physical build size
- builds a kd-tree for nearest-point lookup
- rasterizes each Z layer into an RGBA PNG
- writes files as `out_<layer>.png`

Operational details:

- the output directory is created if missing
- the input file must exist
- every layer is generated sequentially
- the background is flood-filled to `rgba(247, 247, 247, 128)` before point rendering
- per-pixel occupancy is based on nearest-point distance against a hard-coded voxel radius
- point alpha influences output alpha when source PLY contains alpha

Performance and scaling:

- this path is memory-heavy because it loads the full point cloud and kd-tree in-process
- it is appropriate for moderate inputs, not for arbitrarily large scans

### 3. Quantize a Dense PLY

Build the native quantizer:

```bash
make quantize
```

Command:

```bash
./quantize [--log-interval 10000000] [--temp-dir DIR] [--steps bounds,shard,reduce|shard,reduce|reduce] <input.ply> <output.ply>
```

Example:

```bash
./quantize --log-interval 10000000 data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
```

Partial-run examples:

```bash
./quantize --temp-dir ./quantize-cache data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
./quantize --temp-dir ./quantize-cache --steps shard,reduce data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
./quantize --temp-dir ./quantize-cache --steps reduce data/ct/ct_cloud.ply prepared/ct_cloud_quantized.ply
```

What [`quantize.c`](./quantize.c) does:

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
- `--temp-dir DIR` tells the binary exactly where to store or reuse shard files, staged output, and quantizer metadata
- if `--temp-dir` is omitted, the binary creates a fresh retained temp directory under the current working directory
- retained shard files are plain text records with `cell_id x y z r g b a packed_color`
- retained metadata is written to `quantize-metadata.txt` inside the temp directory
- if the input contains no usable numeric vertices, the script writes an empty PLY
- output coordinates are written in the normalized target-space dimensions, not the original source-space bounds
- progress bars were removed; the native binary prints per-stage timings and the same overall timing summary instead
- periodic progress logs are emitted every `10,000,000` records by default; use `--log-interval N` to change that or `--log-interval 0` to disable them
- `--steps` always writes the final output when applicable
- `--steps bounds,shard,reduce` is the default full run
- `--steps shard,reduce` reuses bounds metadata from `--temp-dir`, reruns sharding, then reduces and writes
- `--steps reduce` reuses existing shard files plus metadata from `--temp-dir`, then reduces and writes
- `--temp-dir` is required when `--steps` starts at `shard` or `reduce`

Current implementation characteristics:

- shard count: `128`
- shard record size: `12` bytes
- output vertex record size: `16` bytes
- streaming chunk size: `1 MiB`

This binary is the most suitable path in the repository for large PLY inputs because it streams vertex data instead of building a full in-memory point set.

### 4. Chamfer a PNG Slice Stack

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
2. If the cloud is too dense or irregular, run [`quantize.js`](./quantize.js).
3. Slice the PLY with [`index.js`](./index.js).
4. Post-process the slice stack with [`chamfer.js`](./chamfer.js) if edge relief is needed.

For CT-derived data:

1. Convert NRRD data to PLY under [`data/ct/`](./data/ct).
2. Optionally quantize the resulting PLY.
3. Slice and post-process as above.
