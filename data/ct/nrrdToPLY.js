import fs from "fs";
import zlib from "zlib";

const COLOR_STOPS = [
  // { t: 0.0, c: [127, 48, 156] },
  { t: 0.0, c: [33, 102, 172] },
  { t: 0.3, c: [103, 169, 207] },
  { t: 0.6, c: [253, 219, 199] },
  { t: 0.8, c: [239, 138, 98] },
  { t: 1.0, c: [178, 24, 43] },
];

const MIN_DENSITY = 15;
const MAX_DENSITY = 255;

export const densityToRGB = (v, min, max) => {
  const t = Math.min(1, Math.max(0, (v - min) / (max - min)));

  for (let i = 0; i < COLOR_STOPS.length - 1; i++) {
    const a = COLOR_STOPS[i];
    const b = COLOR_STOPS[i + 1];

    if (t >= a.t && t <= b.t) {
      const u = (t - a.t) / (b.t - a.t);
      return [
        Math.round(a.c[0] + u * (b.c[0] - a.c[0])),
        Math.round(a.c[1] + u * (b.c[1] - a.c[1])),
        Math.round(a.c[2] + u * (b.c[2] - a.c[2])),
      ];
    }
  }

  return COLOR_STOPS[COLOR_STOPS.length - 1].c;
};

export const densityToAlpha = (v, min, max) => {
  const t = Math.min(1, Math.max(0, (v - min) / (max - min)));
  return Math.round(255 * (1 - t)); // denser → less opaque
};

/**
 * Writes an ASCII PLY point cloud from gzipped uint8 NRRD data
 */
export const nrrdToPLY = ({
  gzipped,
  shape: [nx, ny, nz],
  outPath = "out_ascii.ply",
  threshold = 5,
  stride = 2,
}) => {
  const voxels = zlib.gunzipSync(gzipped);

  let count = 0;
  let i = 0;

  for (let z = 0; z < nz; z++) {
    for (let y = 0; y < ny; y++) {
      for (let x = 0; x < nx; x++, i++) {
        if (x % stride || y % stride || z % stride) continue;
        const v = voxels[i];
        if (v === undefined || v < threshold) continue;
        count++;
      }
    }
  }

  const stream = fs.createWriteStream(outPath, { encoding: "ascii" });

  stream.write(
    `ply
format ascii 1.0
element vertex ${count}
property float x
property float y
property float z
property uchar red
property uchar green
property uchar blue
property uchar alpha
end_header
`,
  );

  i = 0;
  for (let z = 0; z < nz; z++) {
    for (let y = 0; y < ny; y++) {
      for (let x = 0; x < nx; x++, i++) {
        if (x % stride || y % stride || z % stride) continue;

        const v = voxels[i];
        if (v === undefined || v < threshold) continue;

        const [r, g, b] = densityToRGB(v, MIN_DENSITY, MAX_DENSITY);
        const a = densityToAlpha(v, MIN_DENSITY, MAX_DENSITY);

        stream.write(`${x} ${y} ${z} ${r} ${g} ${b} ${a}\n`);
      }
    }
  }

  stream.end();
  console.log(`Wrote ${count.toLocaleString()} ASCII points → ${outPath}`);
};
