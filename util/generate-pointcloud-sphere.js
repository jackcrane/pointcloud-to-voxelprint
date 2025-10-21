// sphere-points.js
// Generate a colored sphere point cloud and save as a standard PLY file (ASCII).

import fs from "fs";

const TWO_PI = Math.PI * 2;

/**
 * Generate n random colored points on or in a sphere.
 * Each point: [x, y, z, r, g, b]
 */
export const generateSpherePoints = ({
  n = 10000,
  radius = 1,
  center = [0, 0, 0],
  mode = "surface",
  colorMode = "random", // "random" | "gradient"
} = {}) => {
  const [cx, cy, cz] = center;
  const out = new Array(n);

  for (let i = 0; i < n; i++) {
    const z = Math.random() * 2 - 1;
    const theta = Math.random() * TWO_PI;
    const r_xy = Math.sqrt(1 - z * z);
    const R = mode === "volume" ? radius * Math.cbrt(Math.random()) : radius;

    const x = cx + R * r_xy * Math.cos(theta);
    const y = cy + R * r_xy * Math.sin(theta);
    const zc = cz + R * z;

    let r, g, b;
    if (colorMode === "gradient") {
      // map z position to color gradient
      const t = (zc - (cz - radius)) / (2 * radius);
      r = Math.floor(255 * t);
      g = Math.floor(255 * (1 - t));
      b = Math.floor(128 + 127 * Math.sin(theta));
    } else {
      // random colors
      r = Math.floor(Math.random() * 256);
      g = Math.floor(Math.random() * 256);
      b = Math.floor(Math.random() * 256);
    }

    out[i] = [x, y, zc, r, g, b];
  }

  return out;
};

/**
 * Save colored point cloud to PLY.
 */
export const saveAsPLY = (points, outPath = "sphere.ply") => {
  const header = [
    "ply",
    "format ascii 1.0",
    `element vertex ${points.length}`,
    "property float x",
    "property float y",
    "property float z",
    "property uchar red",
    "property uchar green",
    "property uchar blue",
    "end_header",
  ].join("\n");

  const body = points.map((p) => p.join(" ")).join("\n");
  fs.writeFileSync(outPath, `${header}\n${body}\n`);
  console.log(`Saved ${points.length} colored points → ${outPath}`);
};

/** Example usage */
if (import.meta.url === `file://${process.argv[1]}`) {
  const pts = generateSpherePoints({
    n: 10000,
    radius: 1,
    mode: "surface",
    colorMode: "gradient",
  });
  saveAsPLY(pts, "sphere.ply");
}
