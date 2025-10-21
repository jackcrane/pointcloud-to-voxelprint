import { PLY } from "./classes/ply.js";
import { PNG } from "./classes/png.js";
import { Point3D } from "./classes/Point3D.js";
import cliProgress from "cli-progress";
const { SingleBar, Presets } = cliProgress;

/** ---- physical settings ---- */
const DPI = 300; // dots per inch for X/Y
const LAYER_HEIGHT_NM = 27_000; // Z slice height
const NM_PER_INCH = 25_400_000; // exact

// === Set your physical build size here (inches) ===
const X_IN = 1;
const Y_IN = 1;
const Z_IN = 1;
// const LONGEST_SIDE_IN = 3.0; // optional "fit longest side" helper

/** ---- main ---- */
export const run = async () => {
  const ply = new PLY("./data/sphere.ply");
  const { min, max } = ply.getBounds();

  // padding in model units (same units as the PLY)
  const paddingRatio = 0.1;
  const xPad = (max.x - min.x) * paddingRatio;
  const yPad = (max.y - min.y) * paddingRatio;
  const zPad = (max.z - min.z) * paddingRatio;

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
    // Map the discrete layer index to a world Z in the padded model bounds
    const zWorld = min.z + ((z + 0.5) / depth) * zSize;

    for (let column = 0; column < width; column++) {
      const x = min.x + ((column + 0.5) / width) * xSize;

      for (let row = 0; row < height; row++) {
        const y = min.y + ((row + 0.5) / height) * ySize;

        const np = ply.nearestPoint([x, y, zWorld], {
          maxDistance: 0.08,
        });
        if (!np) continue;
        const { point: p, distance: d } = np;

        // simple shell thickness visualization
        if (d > 0.07) continue;
        if (d > 0.02) {
          image.setPixel(column, row, 247, 247, 247, 128);
          continue;
        }
        image.setPixel(column, row, p.r, p.g, p.b);
      }
    }

    if (image.countFilledPixels() > 500) {
      image.floodFillFrom(
        Math.floor(width / 2),
        Math.floor(height / 2),
        247,
        247,
        247,
        128
      );
    }

    await image.flush(`out/out_${z}.png`);
    image.clear();

    layerBar.increment();
  }

  layerBar.stop();

  // API parity example
  const samplePoint = new Point3D(0, 0, 0);
  void samplePoint;
};

// run immediately if executed directly
await run();
