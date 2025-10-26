// chamfer.js — Fully chamfer a stacked-slice cuboid along all edges, with optional debug edge overlay.
// Run: node chamfer.js <inputDir> <outputDir> <radiusInches> [--debug]
//
// Assumptions
// - Input directory contains PNG slices (RGBA) ordered (natural sort). Each file = one Z-layer.
// - VOID = fully transparent (alpha == 0). Any nonzero alpha = material.
// - DPI (X,Y) = 300; Layer height = 27,000 nm. z-units derived from nm/in.
// - We compute one global axis-aligned bbox from all slices where alpha>0, then apply a
//   uniform bevel radius (inches) to all 12 edges and 8 vertices.
//
// Debug overlay (enabled with --debug)
// - “Horizontal” (within-slice XY) edge changes → draw a 1-px black HAIRLINE along the material side
//   of the chamfer boundary (continuous straight lines).
// - “Vertical” (between-slice Z) edge changes → draw a single 1-px black DOT on the material side
//   when a voxel transitions from non-chamfered at z-1 to chamfered at z (dots stack across layers).
//
// Notes
// - Black hairlines/dots are drawn on the neighboring MATERIAL pixel just inside the chamfer boundary,
//   so the chamfered pixel can still become fully transparent.

import fs from "fs/promises";
import fssync from "fs";
import path from "path";
import sharp from "sharp";
import { fileURLToPath } from "url";

export const DPI_XY = 300; // dots per inch (X,Y)
export const LAYER_HEIGHT_NM = 27_000; // per-slice thickness
export const NM_PER_INCH = 25_400_000; // exact

const isPng = (f) => /\.png$/i.test(f);

// Natural-ish filename sort (so 2 < 10)
const filenameCompare = (a, b) => {
  const ax = a.toLowerCase().match(/\d+|\D+/g) || [a.toLowerCase()];
  const bx = b.toLowerCase().match(/\d+|\D+/g) || [b.toLowerCase()];
  const len = Math.max(ax.length, bx.length);
  for (let i = 0; i < len; i++) {
    const as = ax[i] ?? "";
    const bs = bx[i] ?? "";
    const an = /^\d+$/.test(as);
    const bn = /^\d+$/.test(bs);
    if (an && bn) {
      const diff = Number(as) - Number(bs);
      if (diff) return diff;
    } else {
      const diff = as.localeCompare(bs);
      if (diff) return diff;
    }
  }
  return 0;
};

const ensureDir = async (dir) => fs.mkdir(dir, { recursive: true });

const readRgba = async (pngPath) => {
  const img = sharp(pngPath, { limitInputPixels: false }).ensureAlpha();
  const info = await img.metadata();
  const { data } = await img.raw().toBuffer({ resolveWithObject: true });
  return {
    data,
    width: info.width,
    height: info.height,
    stride: info.width * 4,
  };
};

// Pass 1: global bbox over all slices (alpha>0)
const computeGlobalBBox = async (inDir, files) => {
  let x0 = Infinity,
    y0 = Infinity,
    z0 = 0;
  let x1 = -1,
    y1 = -1,
    z1 = files.length - 1;

  let expectedW = null,
    expectedH = null;

  for (let zi = 0; zi < files.length; zi++) {
    const p = path.join(inDir, files[zi]);
    const { data, width, height, stride } = await readRgba(p);

    if (expectedW == null) {
      expectedW = width;
      expectedH = height;
    }
    if (width !== expectedW || height !== expectedH) {
      throw new Error(
        `Slice dimension mismatch at "${files[zi]}": got ${width}x${height}, expected ${expectedW}x${expectedH}`
      );
    }

    let any = false;
    for (let y = 0; y < height; y++) {
      let row = y * stride;
      for (let x = 0; x < width; x++) {
        const a = data[row + x * 4 + 3];
        if (a > 0) {
          any = true;
          if (x < x0) x0 = x;
          if (x > x1) x1 = x;
          if (y < y0) y0 = y;
          if (y > y1) y1 = y;
        }
      }
    }
    if (any) {
      z1 = Math.max(z1, zi);
    }
  }

  if (!(x1 >= x0 && y1 >= y0)) return null;
  return { x0, y0, x1, y1, z0, z1, width: expectedW, height: expectedH };
};

// Precompute inch distances along x,y for one image
const precomputeXYInches = (bbox, width, height) => {
  const { x0, x1, y0, y1 } = bbox;
  const dxL = new Float32Array(width);
  const dxR = new Float32Array(width);
  const dyT = new Float32Array(height);
  const dyB = new Float32Array(height);
  for (let x = 0; x < width; x++) {
    dxL[x] = (x - x0) / DPI_XY;
    dxR[x] = (x1 - x) / DPI_XY;
  }
  for (let y = 0; y < height; y++) {
    dyT[y] = (y - y0) / DPI_XY;
    dyB[y] = (y1 - y) / DPI_XY;
  }
  return { dxL, dxR, dyT, dyB };
};

// Precompute inch distances along z for all layers
const precomputeZInches = (bbox, layers) => {
  const { z0, z1 } = bbox;
  const layersPerIn = NM_PER_INCH / LAYER_HEIGHT_NM;
  const dzB = new Float32Array(layers);
  const dzT = new Float32Array(layers);
  for (let z = 0; z < layers; z++) {
    dzB[z] = (z - z0) / layersPerIn;
    dzT[z] = (z1 - z) / layersPerIn;
  }
  return { dzB, dzT, layersPerIn };
};

const shouldChamfer = (dxL, dxR, dyT, dyB, dzB, dzT, r) => {
  // 12 edges
  if (dxL + dyT < r) return true;
  if (dxR + dyT < r) return true;
  if (dxL + dyB < r) return true;
  if (dxR + dyB < r) return true;

  if (dzT + dxL < r) return true;
  if (dzT + dxR < r) return true;
  if (dzT + dyT < r) return true;
  if (dzT + dyB < r) return true;

  if (dzB + dxL < r) return true;
  if (dzB + dxR < r) return true;
  if (dzB + dyT < r) return true;
  if (dzB + dyB < r) return true;

  // 8 vertices
  if (dzT + dxL + dyT < r) return true;
  if (dzT + dxR + dyT < r) return true;
  if (dzT + dxL + dyB < r) return true;
  if (dzT + dxR + dyB < r) return true;

  if (dzB + dxL + dyT < r) return true;
  if (dzB + dxR + dyT < r) return true;
  if (dzB + dxL + dyB < r) return true;
  if (dzB + dxR + dyB < r) return true;

  return false;
};

// Pick a neighbor (up/down/left/right) on the material side (non-chamfer)
// Preference: move toward bbox center to stay "inside".
const pickMaterialNeighbor = (
  x,
  y,
  zB,
  zT,
  xyInches,
  radiusInches,
  bbox,
  width,
  height
) => {
  const { dxL, dxR, dyT, dyB } = xyInches;
  const cx = (bbox.x0 + bbox.x1) / 2;
  const cy = (bbox.y0 + bbox.y1) / 2;
  const dirs = [
    { dx: Math.sign(cx - x), dy: 0 }, // toward center horizontally
    { dx: 0, dy: Math.sign(cy - y) }, // toward center vertically
    { dx: -Math.sign(cx - x), dy: 0 },
    { dx: 0, dy: -Math.sign(cy - y) },
  ];
  for (const { dx, dy } of dirs) {
    const nx = x + dx,
      ny = y + dy;
    if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
    const chamferNeighbor = shouldChamfer(
      dxL[nx],
      dxR[nx],
      dyT[ny],
      dyB[ny],
      zB,
      zT,
      radiusInches
    );
    if (!chamferNeighbor) return { nx, ny };
  }
  return null;
};

// Process one slice with known z index. Returns the per-pixel chamfer mask for vertical-edge debug.
const chamferSlice = async (
  srcPngPath,
  dstPngPath,
  zIndex,
  bbox,
  xyInches,
  zInches,
  radiusInches,
  debug,
  prevChamferMask // Uint8Array | null
) => {
  const { data, width, height, stride } = await readRgba(srcPngPath);
  const out = Buffer.from(data);
  const { dxL, dxR, dyT, dyB } = xyInches;
  const { dzB, dzT } = zInches;

  const zB = dzB[zIndex];
  const zT = dzT[zIndex];
  const thisChamferMask = new Uint8Array(width * height); // 1=chamfered

  const idx = (x, y) => y * width + x;
  const pix = (x, y) => y * stride + x * 4;

  for (let y = bbox.y0; y <= bbox.y1; y++) {
    const dy_t = dyT[y];
    const dy_b = dyB[y];
    for (let x = bbox.x0; x <= bbox.x1; x++) {
      const i = pix(x, y);
      if (out[i + 3] === 0) continue; // already VOID

      const dx_l = dxL[x];
      const dx_r = dxR[x];

      const chamferNow = shouldChamfer(
        dx_l,
        dx_r,
        dy_t,
        dy_b,
        zB,
        zT,
        radiusInches
      );
      if (!chamferNow) continue;

      thisChamferMask[idx(x, y)] = 1;

      // ---- DEBUG OVERLAY ----
      if (debug) {
        // Neighbor chamfer states within this slice (for straight "horizontal" hairline)
        const leftChamfer =
          x > 0
            ? shouldChamfer(
                dxL[x - 1],
                dxR[x - 1],
                dy_t,
                dy_b,
                zB,
                zT,
                radiusInches
              )
            : true;
        const rightChamfer =
          x + 1 < width
            ? shouldChamfer(
                dxL[x + 1],
                dxR[x + 1],
                dy_t,
                dy_b,
                zB,
                zT,
                radiusInches
              )
            : true;
        const upChamfer =
          y > 0
            ? shouldChamfer(
                dx_l,
                dx_r,
                dyT[y - 1],
                dyB[y - 1],
                zB,
                zT,
                radiusInches
              )
            : true;
        const downChamfer =
          y + 1 < height
            ? shouldChamfer(
                dx_l,
                dx_r,
                dyT[y + 1],
                dyB[y + 1],
                zB,
                zT,
                radiusInches
              )
            : true;

        const neighborDiff = !(
          leftChamfer &&
          rightChamfer &&
          upChamfer &&
          downChamfer
        );

        // Previous-slice chamfer (for "vertical" dot stack)
        const wasChamfer = prevChamferMask
          ? prevChamferMask[idx(x, y)] === 1
          : false;
        const zTransition = chamferNow && !wasChamfer;

        // Choose where to draw black:
        // - If neighborDiff within the slice → draw a HAIRLINE on the material side.
        // - Else if zTransition only → draw a DOT on the material side.
        if (neighborDiff || zTransition) {
          const neighbor = pickMaterialNeighbor(
            x,
            y,
            zB,
            zT,
            xyInches,
            radiusInches,
            bbox,
            width,
            height
          );
          if (neighbor) {
            const j = pix(neighbor.nx, neighbor.ny);
            out[j + 0] = 0; // black
            out[j + 1] = 0;
            out[j + 2] = 0;
            out[j + 3] = 255; // opaque
          }
        }
      }

      // Apply chamfer: make VOID
      out[i + 3] = 0;
    }
  }

  await ensureDir(path.dirname(dstPngPath));
  await sharp(out, { raw: { width, height, channels: 4 } })
    .png({ compressionLevel: 9, adaptiveFiltering: false })
    .toFile(dstPngPath);

  return thisChamferMask;
};

export const processFolder = async (
  inDir,
  outDir,
  radiusInches,
  debug = false
) => {
  if (!Number.isFinite(radiusInches) || radiusInches < 0) {
    throw new Error("radiusInches must be a nonnegative number.");
  }
  const names = (await fs.readdir(inDir)).filter(isPng).sort(filenameCompare);
  if (names.length === 0) throw new Error("No PNG slices found.");

  // Global bbox over all slices
  const bbox = await computeGlobalBBox(inDir, names);
  if (!bbox) {
    // No material anywhere → copy all as-is
    await ensureDir(outDir);
    await Promise.all(
      names.map(async (n) =>
        fs.copyFile(path.join(inDir, n), path.join(outDir, n))
      )
    );
    return;
  }

  const xyInches = precomputeXYInches(bbox, bbox.width, bbox.height);
  const zInches = precomputeZInches(bbox, names.length);

  await ensureDir(outDir);

  let prevChamferMask = null;
  for (let zi = 0; zi < names.length; zi++) {
    const src = path.join(inDir, names[zi]);
    const dst = path.join(outDir, names[zi]);
    const mask = await chamferSlice(
      src,
      dst,
      zi,
      bbox,
      xyInches,
      zInches,
      radiusInches,
      debug,
      prevChamferMask
    );
    prevChamferMask = mask;
  }
};

export const main = async () => {
  const args = process.argv.slice(2);
  if (args.length < 3) {
    console.error(
      "Usage: node chamfer.js <inputDir> <outputDir> <radiusInches> [--debug]"
    );
    process.exit(1);
  }
  const inDir = path.resolve(args[0]);
  const outDir = path.resolve(args[1]);
  const radiusInches = Number(args[2]);
  const debug = args.includes("--debug");

  if (!fssync.existsSync(inDir) || !fssync.statSync(inDir).isDirectory()) {
    console.error("Input directory does not exist or is not a directory.");
    process.exit(1);
  }

  await processFolder(inDir, outDir, radiusInches, debug);

  console.log(
    `Chamfered ALL slices from "${inDir}" → "${outDir}" with radius=${radiusInches} inches @ ${DPI_XY} dpi, layer=${LAYER_HEIGHT_NM} nm${
      debug ? " [DEBUG ON]" : ""
    }.`
  );
};

const isDirect =
  fileURLToPath(import.meta.url) ===
  fileURLToPath(`file://${process.argv[1] || ""}`);
if (isDirect) {
  main().catch((err) => {
    console.error(err?.stack || err?.message || String(err));
    process.exit(1);
  });
}
