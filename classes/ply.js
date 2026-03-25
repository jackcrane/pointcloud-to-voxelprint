import { Point3D } from "./Point3D.js";
import {
  forEachPlyVertex,
  readPlyHeader,
} from "../util/ply-format.js";

/** Minimal PLY loader supporting ASCII and binary_little_endian vertex data (x,y,z + optional r,g,b,a) */
export class PLY {
  /** @param {Point3D[]} points */
  constructor(points = []) {
    if (typeof points === "string") {
      throw new Error("PLY loading is async. Use await PLY.load(filePath).");
    }

    /** @type {Point3D[]} */
    this.points = points;

    // ---- precompute bounds
    this.#computeBounds();

    // ---- build balanced kd-tree for fast nearest queries
    this._kdRoot = buildKdTree(this.points);
  }

  /** @param {string} filePath */
  static load = async (filePath) => {
    const headerInfo = await readPlyHeader(filePath);
    const { header } = headerInfo;

    if (
      !header.format.startsWith("ascii") &&
      !header.format.startsWith("binary_little_endian")
    ) {
      throw new Error(`Unsupported PLY format: ${header.format}`);
    }
    if (!header.vertex || header.vertex.count <= 0) {
      throw new Error("PLY has no vertex element.");
    }

    /** @type {Point3D[]} */
    const points = [];

    await forEachPlyVertex(filePath, headerInfo, ({ x, y, z, color }) => {
      points.push(new Point3D(x, y, z, color));
    });

    return new PLY(points);
  };

  /**
   * Points with z in [zMin, zMax] (inclusive).
   * @param {number} zMin
   * @param {number} zMax
   * @returns {Point3D[]}
   */
  pointsBetweenZ = (zMin, zMax) => {
    const lo = Math.min(zMin, zMax);
    const hi = Math.max(zMin, zMax);
    const out = [];
    for (const p of this.points) if (p.z >= lo && p.z <= hi) out.push(p);
    return out;
  };

  /**
   * Nearest point to (x,y,z) or a Point3D with optional isotropic or anisotropic cutoff.
   *
   * Options:
   * - axes: "x"|"y"|"z"|"xy"|"xz"|"yz"|"xyz"   (default "xyz")
   * - maxDistance: number                      (isotropic radius in model units)
   * - maxDistanceX/Y/Z: number                 (anisotropic radii per-axis in model units)
   * - excludeSelf: boolean                     (default false)
   *
   * Behavior:
   * - If maxDistanceX/Y/Z are provided (any of them), the search uses an
   *   axis-aligned ellipsoidal metric: ((dx/sx)^2 + (dy/sy)^2 + (dz/sz)^2) <= 1.
   * - Else if maxDistance is provided, uses a spherical cutoff of radius maxDistance.
   * - If no cutoff is provided, returns the true nearest point (no rejection).
   *
   * Returns { point, distance } where distance is the Euclidean distance in model units.
   */
  nearestPoint = (target, opts = {}) => {
    const axesMask = maskFromAxes(opts.axes ?? "xyz");

    // --- choose metric scales (denominators) for anisotropic distance ---
    const hasAniso =
      Number.isFinite(opts.maxDistanceX) ||
      Number.isFinite(opts.maxDistanceY) ||
      Number.isFinite(opts.maxDistanceZ);

    const sx = Number.isFinite(opts.maxDistanceX)
      ? opts.maxDistanceX
      : Number.isFinite(opts.maxDistance)
        ? opts.maxDistance
        : 1;

    const sy = Number.isFinite(opts.maxDistanceY)
      ? opts.maxDistanceY
      : Number.isFinite(opts.maxDistance)
        ? opts.maxDistance
        : 1;

    const sz = Number.isFinite(opts.maxDistanceZ)
      ? opts.maxDistanceZ
      : Number.isFinite(opts.maxDistance)
        ? opts.maxDistance
        : 1;

    // If any cutoff (iso or aniso) provided, enable rejection at unit normalized radius.
    const cutoffActive =
      hasAniso || Number.isFinite(opts.maxDistance) ? true : false;

    // normalized best distance^2 (ellipsoidal if aniso or iso provided; Euclidean if no cutoff)
    let bestNd2 = cutoffActive ? 1 : Infinity;
    let bestPoint = null;
    let bestEuclidD2 = Infinity;

    const t = Array.isArray(target)
      ? { x: target[0], y: target[1], z: target[2] }
      : target;

    if (!this._kdRoot) return null;

    const stack = [this._kdRoot];

    const invSx = 1 / sx;
    const invSy = 1 / sy;
    const invSz = 1 / sz;

    while (stack.length) {
      const node = stack.pop();
      const p = node.p;

      // Skip exact self if requested
      if (opts.excludeSelf && p.x === t.x && p.y === t.y && p.z === t.z) {
        // still traverse children
      } else {
        // Compute deltas respecting axesMask
        const dx = axesMask & 1 ? p.x - t.x : 0;
        const dy = axesMask & 2 ? p.y - t.y : 0;
        const dz = axesMask & 4 ? p.z - t.z : 0;

        // Euclidean for reporting:
        const d2e = dx * dx + dy * dy + dz * dz;

        // Normalized metric for comparison (handles iso/aniso cutoffs uniformly)
        const ndx = dx * invSx;
        const ndy = dy * invSy;
        const ndz = dz * invSz;
        const d2n = ndx * ndx + ndy * ndy + ndz * ndz;

        // If cutoff active, reject if outside unit ellipsoid
        if (!cutoffActive || d2n <= 1) {
          if (d2n < bestNd2 || (!cutoffActive && d2e < bestEuclidD2)) {
            bestNd2 = cutoffActive ? d2n : d2n; // same variable, meaningful if !cutoffActive too
            bestEuclidD2 = d2e;
            bestPoint = p;
          }
        }
      }

      // KD traversal / pruning
      const axisBit = 1 << node.axis;
      if (!(axesMask & axisBit)) {
        if (node.left) stack.push(node.left);
        if (node.right) stack.push(node.right);
        continue;
      }

      // Signed difference along split axis
      const diff =
        node.axis === 0 ? t.x - p.x : node.axis === 1 ? t.y - p.y : t.z - p.z;

      // Choose near/far children
      const near = diff < 0 ? node.left : node.right;
      const far = diff < 0 ? node.right : node.left;

      if (near) stack.push(near);

      // Prune far branch using normalized split-axis distance
      if (far) {
        // Scale diff by the corresponding axis scale for consistent pruning
        const diffNorm =
          node.axis === 0
            ? diff * invSx
            : node.axis === 1
              ? diff * invSy
              : diff * invSz;

        // If no cutoff, we compare against current bestEuclidD2 using Euclidean lower bound on split:
        if (!cutoffActive) {
          // Lower bound if we crossed the splitting plane: diff^2 <= bestEuclidD2
          if (diff * diff < bestEuclidD2) stack.push(far);
        } else {
          // With cutoffActive, use normalized bound vs bestNd2
          if (diffNorm * diffNorm < bestNd2) stack.push(far);
        }
      }
    }

    if (!bestPoint) return null;
    if (cutoffActive && bestNd2 > 1) return null; // outside cutoff

    return { point: bestPoint, distance: Math.sqrt(bestEuclidD2) };
  };

  getBounds = () => ({
    min: { ...this._min },
    max: { ...this._max },
  });

  // ---------- internals ----------
  #computeBounds = () => {
    if (this.points.length === 0) {
      this._min = { x: 0, y: 0, z: 0 };
      this._max = { x: 0, y: 0, z: 0 };
      return;
    }
    let minX = Infinity,
      minY = Infinity,
      minZ = Infinity;
    let maxX = -Infinity,
      maxY = -Infinity,
      maxZ = -Infinity;
    for (const p of this.points) {
      if (p.x < minX) minX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.z < minZ) minZ = p.z;
      if (p.x > maxX) maxX = p.x;
      if (p.y > maxY) maxY = p.y;
      if (p.z > maxZ) maxZ = p.z;
    }
    this._min = { x: minX, y: minY, z: minZ };
    this._max = { x: maxX, y: maxY, z: maxZ };
  };
}

/* ---------- kd-tree implementation (balanced median split) ---------- */

const buildKdTree = (points) => {
  if (points.length === 0) return null;

  const idxs = new Array(points.length);
  for (let i = 0; i < points.length; i++) idxs[i] = i;

  const selectK = (lo, hi, k, axis) => {
    while (lo < hi) {
      const pivot = partition(lo, hi, (lo + hi) >> 1, axis);
      if (k === pivot) return;
      if (k < pivot) hi = pivot - 1;
      else lo = pivot + 1;
    }
  };
  const partition = (lo, hi, pivotIdx, axis) => {
    const pivotVal = coord(points[idxs[pivotIdx]], axis);
    swap(idxs, pivotIdx, hi);
    let store = lo;
    for (let i = lo; i < hi; i++) {
      if (coord(points[idxs[i]], axis) < pivotVal) {
        swap(idxs, store++, i);
      }
    }
    swap(idxs, store, hi);
    return store;
  };
  const coord = (p, axis) => (axis === 0 ? p.x : axis === 1 ? p.y : p.z);
  const swap = (arr, a, b) => {
    const t = arr[a];
    arr[a] = arr[b];
    arr[b] = t;
  };

  const build = (lo, hi, depth) => {
    if (lo > hi) return null;
    const axis = depth % 3; // cycle x->y->z
    const mid = (lo + hi) >> 1;
    selectK(lo, hi, mid, axis);
    const iMid = idxs[mid];
    const node = {
      p: points[iMid],
      axis,
      left: build(lo, mid - 1, depth + 1),
      right: build(mid + 1, hi, depth + 1),
    };
    return node;
  };

  return build(0, points.length - 1, 0);
};

// Convert axes string to bitmask: x=1, y=2, z=4
const maskFromAxes = (axes) => {
  switch (axes) {
    case "x":
      return 1;
    case "y":
      return 2;
    case "z":
      return 4;
    case "xy":
      return 1 | 2;
    case "xz":
      return 1 | 4;
    case "yz":
      return 2 | 4;
    case "xyz":
    default:
      return 1 | 2 | 4;
  }
};
