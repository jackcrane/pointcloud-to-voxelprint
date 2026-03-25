// index.js — slice colored PLY to PNG stack with dot size defined in inches
// ES modules, named exports, arrow functions.

import { PLY } from "./classes/ply.js";
import { PNG } from "./classes/png.js";
import cliProgress from "cli-progress";
import fs from "fs";
const { SingleBar, Presets } = cliProgress;

/** ---- physical settings ---- */
const DPI = 300; // dots per inch for X/Y
const LAYER_HEIGHT_NM = 27_000; // Z slice height
const NM_PER_INCH = 25_400_000; // exact

const multiplier = 1;
// === Physical build size (inches) ===
const X_IN = 1 * multiplier;
const Y_IN = 2.5 * multiplier;
const Z_IN = 0.75 * multiplier;
// const X_IN = 3.85 / 2;
// const Y_IN = 3.34 / 2;
// const Z_IN = 7.74 / 2;

// === Dot radius (inches) ===
const VOXEL_RADIUS_INCHES = 0.01 * multiplier;

/** ---- main ---- */
export const run = async () => {
  const [inputPath, outputDir] = process.argv.slice(2);
  if (!inputPath || !outputDir) {
    console.error("Usage: node index.js <input.ply> <output_dir>");
    process.exit(1);
  }

  if (!fs.existsSync(inputPath)) {
    console.error(`Input file not found: ${inputPath}`);
    process.exit(1);
  }

  fs.mkdirSync(outputDir, { recursive: true });

  console.log("Loading PLY into memory...");
  const ply = await PLY.load(inputPath);
  const { min, max } = ply.getBounds();
  console.log(min, max);

  // optional padding (model units)
  const paddingRatio = 0;
  const xPad = (max.x - min.x) * paddingRatio;
  const yPad = (max.y - min.y) * paddingRatio;
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

  const width = Math.max(1, Math.round(xIn * DPI));
  const height = Math.max(1, Math.round(yIn * DPI));
  const depth = Math.max(1, Math.round((zIn * NM_PER_INCH) / LAYER_HEIGHT_NM));

  console.log(
    `Physical (in): ${xIn.toFixed(3)} x ${yIn.toFixed(3)} x ${zIn.toFixed(3)}`,
  );
  console.log(
    `Pixels/Layers: ${width} x ${height} x ${depth}  @ ${DPI} dpi, ${LAYER_HEIGHT_NM} nm`,
  );

  const MODEL_UNITS_PER_INCH_X = xSize / xIn;
  const MODEL_UNITS_PER_INCH_Y = ySize / yIn;
  const MODEL_UNITS_PER_INCH_Z = zSize / zIn;
  const MODEL_UNITS_PER_INCH =
    (MODEL_UNITS_PER_INCH_X + MODEL_UNITS_PER_INCH_Y + MODEL_UNITS_PER_INCH_Z) /
    3;

  const VOXEL_RADIUS = VOXEL_RADIUS_INCHES * MODEL_UNITS_PER_INCH;
  console.log(
    `Voxel radius: ${VOXEL_RADIUS_INCHES.toFixed(
      4,
    )} in  →  ${VOXEL_RADIUS.toFixed(2)} model units`,
  );

  const layerBar = new SingleBar(
    {
      format:
        "Slicing |{bar}| {percentage}% | {value}/{total} layers | ETA {eta_formatted}",
      hideCursor: true,
    },
    Presets.shades_classic,
  );
  layerBar.start(depth, 0);

  const image = new PNG(width, height);

  const layerThickness = zSize / depth;

  for (let z = 0; z < depth; z++) {
    // const zWorld = min.z + ((z + 0.5) / depth) * zSize;
    const zWorld = min.z + z * layerThickness;
    image.floodFillFrom(
      Math.floor(width / 2),
      Math.floor(height / 2),
      247,
      247,
      247,
      128,
    );

    for (let column = 0; column < width; column++) {
      const x = min.x + ((column + 0.5) / width) * xSize;

      for (let row = 0; row < height; row++) {
        const y = min.y + ((row + 0.5) / height) * ySize;

        const np = ply.nearestPoint([x, y, zWorld], {
          // maxDistance: VOXEL_RADIUS,
          maxDistanceX: VOXEL_RADIUS * 1,
          maxDistanceY: VOXEL_RADIUS * 1,
          maxDistanceZ: VOXEL_RADIUS,
          // maxDistanceZ: (zSize / depth) * 0.5,
          // maxDistanceZ: layerThickness,
        });
        if (!np) continue;
        const { point: p, distance: d } = np;

        if (d > VOXEL_RADIUS) continue;

        // Enable color by uncommenting below line:
        // const alpha = Math.min(
        //   255 - (Number.isFinite(p.a) ? p.a : 1) * 0.5,
        //   200,
        // );
        // const alpha = 250;

        image.setPixel(column, row, p.r, p.g, p.b, alpha);
        // image.setPixel(column, row, 1, 1, 1, 255);
      }
    }

    const outPath = `${outputDir}/out_${z}.png`;
    await image.flush(outPath);
    image.clear();
    layerBar.increment();
  }

  layerBar.stop();
  console.log("Done.");
};

// run immediately if executed directly
await run();
