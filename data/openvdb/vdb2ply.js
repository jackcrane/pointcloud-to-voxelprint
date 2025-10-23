// vdb2ply.js
import fs from "fs/promises";
import path from "path";
import { VDBFile, FloatGrid } from "vdb-js";

/**
 * Sample the active voxels of a VDB grid into a point cloud array.
 * @param {FloatGrid} grid – a FloatGrid from vdb-js
 * @param {number} isoThreshold – include voxels whose value ≥ isoThreshold
 * @returns {Array<{x:number,y:number,z:number, value:number}>}
 */
const sampleGridToPoints = (grid, isoThreshold = 0.0) => {
  const points = [];
  const accessor = grid.getAccessor();
  const transform = grid.transform();

  // iterate through all active voxels
  for (const iter of accessor.activeVoxels()) {
    const { x, y, z } = iter.getCoord();
    const value = iter.getValue();
    if (value >= isoThreshold) {
      // world position = index * voxelSize + origin
      const worldP = transform.indexToWorld([x, y, z]);
      points.push({ x: worldP[0], y: worldP[1], z: worldP[2], value });
    }
  }

  return points;
};

/**
 * Write a PLY ASCII file from a point cloud.
 * @param {string} outPath
 * @param {Array<{x:number,y:number,z:number,value:number}>} points
 * @returns {Promise<void>}
 */
const writePly = async (outPath, points) => {
  const header =
    [
      "ply",
      "format ascii 1.0",
      `element vertex ${points.length}`,
      "property float x",
      "property float y",
      "property float z",
      "property float value",
      "end_header",
    ].join("\n") + "\n";

  const body =
    points.map((p) => `${p.x} ${p.y} ${p.z} ${p.value}`).join("\n") + "\n";

  await fs.writeFile(outPath, header + body, "utf8");
};

/**
 * Convert a .vdb file to a .ply point cloud
 * @param {string} inputVdbPath
 * @param {string} outputPlyPath
 * @param {number} isoThreshold – only voxels ≥ this value will become points
 */
export const convertVdbToPly = async (
  inputVdbPath,
  outputPlyPath,
  isoThreshold = 0.0
) => {
  // read file buffer
  const buf = await fs.readFile(inputVdbPath);
  // parse .vdb
  const vdb = await VDBFile.fromBuffer(buf);
  // pick first float grid (you might want to pick a specific one by name)
  const gridNames = vdb.gridNames();
  if (gridNames.length === 0) {
    throw new Error("No grids found in VDB file");
  }
  const gridName = gridNames[0];
  const grid = await vdb.readGrid(gridName);
  if (!(grid instanceof FloatGrid)) {
    throw new Error(`Grid ${gridName} is not a FloatGrid`);
  }

  // sample points
  const points = sampleGridToPoints(grid, isoThreshold);

  // write PLY
  await writePly(outputPlyPath, points);

  console.log(
    `Converted ${inputVdbPath} → ${outputPlyPath}, points: ${points.length}`
  );
};

// Example usage when run as script:
if (process.argv.length >= 4) {
  const input = process.argv[2];
  const output = process.argv[3];
  const threshold =
    process.argv[4] !== undefined ? parseFloat(process.argv[4]) : 0.0;

  convertVdbToPly(input, output, threshold).catch((err) => {
    console.error("Error:", err);
    process.exit(1);
  });
}
