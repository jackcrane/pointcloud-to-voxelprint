// worker.js
import { parentPort, workerData } from "worker_threads";
import { PLY } from "./classes/ply.js";
import { PNG } from "./classes/png.js";

const {
  startZ,
  endZ,
  width,
  height,
  min,
  max,
  zSize,
  xSize,
  ySize,
  depth,
  plyPath,
} = workerData;
const ply = new PLY(plyPath);
const image = new PNG(width, height);

for (let z = startZ; z < endZ; z++) {
  const zWorld = min.z + ((z + 0.5) / depth) * zSize;
  for (let x = 0; x < width; x++) {
    const worldX = min.x + ((x + 0.5) / width) * xSize;
    for (let y = 0; y < height; y++) {
      const worldY = min.y + ((y + 0.5) / height) * ySize;
      const { point: p, distance: d } = ply.nearestPoint([
        worldX,
        worldY,
        zWorld,
      ]);
      if (d > 0.07) continue;
      if (d > 0.02) image.setPixel(x, y, 255, 255, 255, 128);
      else image.setPixel(x, y, p.r, p.g, p.b);
    }
  }
  await image.flush(`out/out_${z}.png`);
  image.clear();
}

parentPort.postMessage({ done: true });
