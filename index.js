// index.js — slice colored PLY to PNG stack with dot size defined in inches
// ES modules, named exports, arrow functions.

import { PLY } from "./classes/ply.js";
import { PNG } from "./classes/png.js";
import { Point3D } from "./classes/Point3D.js";
import cliProgress from "cli-progress";
const { SingleBar, Presets } = cliProgress;

/** ---- physical settings ---- */
const DPI = 300; // dots per inch for X/Y
const LAYER_HEIGHT_NM = 27_000; // Z slice height
const NM_PER_INCH = 25_400_000; // exact

// === Physical build size (inches) ===
const X_IN = 1.5;
const Y_IN = 1.5;
const Z_IN = 0.75;
// const LONGEST_SIDE_IN = 3.0; // optional "fit longest side" helper

// === Dot radius (inches) ===
// This defines how big each point appears in the physical print.
// Example: 0.01 = 0.01 inches (~0.25 mm), 0.05 = fat dots.
const VOXEL_RADIUS_INCHES = 0.003;

/** ---- main ---- */
export const run = async () => {
  const ply = new PLY("./data/nexrad/joplin_points_colored.ply");
  const { min, max } = ply.getBounds();
  console.log(min, max);

  // optional padding (model units)
  const paddingRatio = 0;
  const xPad = (max.x - min.x) * paddingRatio;
  const yPad = (max.y - min.y) * paddingRatio;
  // const zPad = (max.z - min.z) * paddingRatio;
  const zPad = 0.0;

  min.x -= xPad;
  min.y -= yPad;
  min.z -= zPad;
  max.x += xPad;
  max.y += yPad;
  max.z += zPad;

  const xSize = max.x - min.x;
  const ySize = max.y - min.y;
  const zSize = max.z - min.z;

  // --- choose physical inches for each axis ---
  let xIn = X_IN,
    yIn = Y_IN,
    zIn = Z_IN;

  if ([xIn, yIn, zIn].some((v) => v == null)) {
    const maxModel = Math.max(xSize, ySize, zSize);
    const LSI = typeof LONGEST_SIDE_IN === "number" ? LONGEST_SIDE_IN : 3.0;
    const scale = LSI / maxModel; // inches per (model unit)
    xIn = xIn ?? xSize * scale;
    yIn = yIn ?? ySize * scale;
    zIn = zIn ?? zSize * scale;
  }

  // --- pixels/layers from inches ---
  const width = Math.max(1, Math.round(xIn * DPI));
  const height = Math.max(1, Math.round(yIn * DPI));
  const depth = Math.max(1, Math.round((zIn * NM_PER_INCH) / LAYER_HEIGHT_NM));

  console.log(
    `Physical (in): ${xIn.toFixed(3)} x ${yIn.toFixed(3)} x ${zIn.toFixed(3)}`
  );
  console.log(
    `Pixels/Layers: ${width} x ${height} x ${depth}  @ ${DPI} dpi, ${LAYER_HEIGHT_NM} nm`
  );

  // --- determine scale (model units per inch) ---
  const MODEL_UNITS_PER_INCH_X = xSize / xIn;
  const MODEL_UNITS_PER_INCH_Y = ySize / yIn;
  const MODEL_UNITS_PER_INCH_Z = zSize / zIn;
  const MODEL_UNITS_PER_INCH =
    (MODEL_UNITS_PER_INCH_X + MODEL_UNITS_PER_INCH_Y + MODEL_UNITS_PER_INCH_Z) /
    3;

  // --- convert voxel radius from inches to model units ---
  const VOXEL_RADIUS = VOXEL_RADIUS_INCHES * MODEL_UNITS_PER_INCH;
  console.log(
    `Voxel radius: ${VOXEL_RADIUS_INCHES.toFixed(
      4
    )} in  →  ${VOXEL_RADIUS.toFixed(2)} model units`
  );

  // ---- progress bar (layers) ----
  const layerBar = new SingleBar(
    {
      format:
        "Slicing |{bar}| {percentage}% | {value}/{total} layers | ETA {eta_formatted}",
      hideCursor: true,
    },
    Presets.shades_classic
  );
  layerBar.start(depth, 0);

  const image = new PNG(width, height);

  for (let z = 0; z < depth; z++) {
    const zWorld = min.z + ((z + 0.5) / depth) * zSize;

    image.floodFillFrom(
      Math.floor(width / 2),
      Math.floor(height / 2),
      247,
      247,
      247,
      128
    );

    for (let column = 0; column < width; column++) {
      const x = min.x + ((column + 0.5) / width) * xSize;

      for (let row = 0; row < height; row++) {
        const y = min.y + ((row + 0.5) / height) * ySize;

        const np = ply.nearestPoint([x, y, zWorld], {
          maxDistance: VOXEL_RADIUS,
        });
        if (!np) continue;
        const { point: p, distance: d } = np;

        if (d > VOXEL_RADIUS) continue;

        // FOR COLOR, ENABLE FOLLOWING LINE AND DISABLE THE ONE BELOW.
        // image.setPixel(column, row, p.r, p.g, p.b);
        image.setPixel(column, row, 1, 1, 1, 255);
      }
    }

    await image.flush(`out/out_${z}.png`);
    image.clear();
    layerBar.increment();
  }

  layerBar.stop();
  const samplePoint = new Point3D(0, 0, 0);
  void samplePoint;
};

// run immediately if executed directly
await run();
