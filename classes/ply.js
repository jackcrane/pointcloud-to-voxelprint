// ply.js — ES module (drop-in faster version)
// Usage: const ply = new PLY("./model.ply");

import { readFileSync } from "fs";
import { Point3D } from "./Point3D.js";

/** Minimal PLY loader supporting ASCII and binary_little_endian vertex data (x,y,z + optional r,g,b,a) */
export class PLY {
  /** @param {string} filePath */
  constructor(filePath) {
    const buf = readFileSync(filePath);
    const { header, headerEndOffset } = parseHeader(buf);
    if (
      !header.format.startsWith("ascii") &&
      !header.format.startsWith("binary_little_endian")
    ) {
      throw new Error(`Unsupported PLY format: ${header.format}`);
    }
    if (!header.vertex || header.vertex.count <= 0) {
      throw new Error("PLY has no vertex element.");
    }

    const props = header.vertex.properties; // array of {name,type}
    const idx = {
      x: props.findIndex((p) => p.name === "x"),
      y: props.findIndex((p) => p.name === "y"),
      z: props.findIndex((p) => p.name === "z"),
    };
    if (idx.x < 0 || idx.y < 0 || idx.z < 0)
      throw new Error("PLY vertex must include x,y,z.");

    // color properties may be r/g/b/a or red/green/blue/alpha
    const cname = (a, b) =>
      props.findIndex((p) => p.name === a || p.name === b);
    const cidx = {
      r: cname("red", "r"),
      g: cname("green", "g"),
      b: cname("blue", "b"),
      a: cname("alpha", "a"),
    };

    const count = header.vertex.count;
    /** @type {Point3D[]} */
    this.points = [];

    if (header.format.startsWith("ascii")) {
      const text = buf.slice(headerEndOffset).toString("utf8");
      const lines = text.trim().split(/\r?\n/);
      for (let i = 0; i < count; i++) {
        const parts = lines[i].trim().split(/\s+/);
        const x = parseFloat(parts[idx.x]);
        const y = parseFloat(parts[idx.y]);
        const z = parseFloat(parts[idx.z]);
        const color = readColorFromParts(parts, cidx);
        const p = new Point3D(x, y, z, color);
        this.points.push(p);
      }
    } else {
      // binary_little_endian
      const dv = new DataView(buf.buffer, buf.byteOffset + headerEndOffset);
      let off = 0;
      const readers = props.map((p) => pickReader(p.type));
      const stride = readers.reduce((s, r) => s + r.size, 0);

      for (let i = 0; i < count; i++) {
        let pos = off;
        const values = new Array(readers.length);
        for (let pi = 0; pi < readers.length; pi++) {
          const r = readers[pi];
          values[pi] = r.read(dv, pos);
          pos += r.size;
        }
        off += stride;

        const x = Number(values[idx.x]);
        const y = Number(values[idx.y]);
        const z = Number(values[idx.z]);
        const color = readColorFromParts(values, cidx);
        const p = new Point3D(x, y, z, color);
        this.points.push(p);
      }
    }

    // ---- precompute bounds
    this.#computeBounds();

    // ---- build balanced kd-tree for fast nearest queries
    // store as a flat object tree to avoid function allocations during query
    this._kdRoot = buildKdTree(this.points);
  }

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
   * Nearest point to (x,y,z) or a Point3D.
   * @param {Point3D|[number,number,number]} target
   * @param {{axes?: "x"|"y"|"z"|"xy"|"xz"|"yz"|"xyz", maxDistance?: number, excludeSelf?: boolean}} [opts]
   * @returns {{point: Point3D, distance: number} | null}
   */
  nearestPoint = (target, opts = {}) => {
    const axesMask = maskFromAxes(opts.axes ?? "xyz");
    const maxD = Number.isFinite(opts.maxDistance)
      ? opts.maxDistance
      : Infinity;
    const t = Array.isArray(target)
      ? { x: target[0], y: target[1], z: target[2] }
      : target;

    if (!this._kdRoot) return null;

    let best = null,
      bestD2 = maxD * maxD;
    const stack = [this._kdRoot];

    while (stack.length) {
      const node = stack.pop();
      const p = node.p;

      // squared distance
      const dx = axesMask & 1 ? p.x - t.x : 0;
      const dy = axesMask & 2 ? p.y - t.y : 0;
      const dz = axesMask & 4 ? p.z - t.z : 0;
      const d2 = dx * dx + dy * dy + dz * dz;

      if (d2 < bestD2) {
        bestD2 = d2;
        best = p;
      }

      const axisBit = 1 << node.axis;
      if (!(axesMask & axisBit)) {
        if (node.left) stack.push(node.left);
        if (node.right) stack.push(node.right);
        continue;
      }

      const diff =
        node.axis === 0 ? t.x - p.x : node.axis === 1 ? t.y - p.y : t.z - p.z;
      const near = diff < 0 ? node.left : node.right;
      const far = diff < 0 ? node.right : node.left;
      if (near) stack.push(near);
      if (far && diff * diff < bestD2) stack.push(far);
    }

    return best ? { point: best, distance: Math.sqrt(bestD2) } : null;
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

/**
 * Node shape: { p: Point3D, axis: 0|1|2, left: Node|null, right: Node|null }
 * We build using an index array to avoid moving Point3D objects.
 */
const buildKdTree = (points) => {
  if (points.length === 0) return null;

  // Precompute an index array to select medians without copying point objects
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

/* ---------- helpers ---------- */

const parseHeader = (buf) => {
  const txt = buf.toString("utf8");
  const endIdx = txt.indexOf("end_header");
  if (endIdx < 0) throw new Error("Invalid PLY: missing end_header.");

  // Determine exact header byte length (consume the line that has end_header)
  const after = txt.indexOf("\n", endIdx);
  const headerEndOffset = Buffer.from(
    txt.slice(0, after < 0 ? endIdx + "end_header".length : after + 1),
    "utf8"
  ).length;

  const headerLines = txt.slice(0, after < 0 ? endIdx : after).split(/\r?\n/);

  if (!/^ply\s*$/i.test(headerLines[0])) throw new Error("Not a PLY file.");
  const formatLine = headerLines.find((l) => l.startsWith("format "));
  if (!formatLine) throw new Error("Missing format line.");
  const format = formatLine.split(/\s+/).slice(1, 3).join(" ");

  // Parse elements/properties (only care about "vertex")
  const header = { format, vertex: null };
  let i = 0;
  while (i < headerLines.length) {
    const line = headerLines[i].trim();
    if (line.startsWith("element ")) {
      const [, name, countStr] = line.split(/\s+/);
      const count = parseInt(countStr, 10);
      if (name === "vertex") {
        i++;
        const properties = [];
        while (
          i < headerLines.length &&
          headerLines[i].trim().startsWith("property ")
        ) {
          const parts = headerLines[i].trim().split(/\s+/).slice(1); // after "property"
          if (parts[0] === "list") {
            i++;
            continue; // skip lists
          }
          const [type, name2] = parts;
          properties.push({ name: name2, type });
          i++;
        }
        header.vertex = { count, properties };
        continue;
      }
    }
    i++;
  }
  return { header, headerEndOffset };
};

const pickReader = (type) => {
  // PLY numeric types → size & DataView reader
  switch (type) {
    case "float":
    case "float32":
      return { size: 4, read: (dv, off) => dv.getFloat32(off, true) };
    case "double":
    case "float64":
      return { size: 8, read: (dv, off) => dv.getFloat64(off, true) };
    case "uchar":
    case "uint8":
      return { size: 1, read: (dv, off) => dv.getUint8(off) };
    case "char":
    case "int8":
      return { size: 1, read: (dv, off) => dv.getInt8(off) };
    case "ushort":
    case "uint16":
      return { size: 2, read: (dv, off) => dv.getUint16(off, true) };
    case "short":
    case "int16":
      return { size: 2, read: (dv, off) => dv.getInt16(off, true) };
    case "uint":
    case "uint32":
      return { size: 4, read: (dv, off) => dv.getUint32(off, true) };
    case "int":
    case "int32":
      return { size: 4, read: (dv, off) => dv.getInt32(off, true) };
    default:
      // Fallback as 32-bit float
      return { size: 4, read: (dv, off) => dv.getFloat32(off, true) };
  }
};

const readColorFromParts = (parts, cidx) => {
  const haveRGB =
    cidx.r >= 0 &&
    cidx.g >= 0 &&
    cidx.b >= 0 &&
    parts[cidx.r] != null &&
    parts[cidx.g] != null &&
    parts[cidx.b] != null;

  if (!haveRGB) return null;

  const to255 = (v) => {
    const n = Number(v);
    if (!Number.isFinite(n)) return null;
    // Heuristic: if <=1, assume normalized float; else 0–255 byte
    if (n <= 1 && n >= 0) return Math.round(n * 255);
    return Math.max(0, Math.min(255, Math.round(n)));
  };

  const r = to255(parts[cidx.r]);
  const g = to255(parts[cidx.g]);
  const b = to255(parts[cidx.b]);
  const a = cidx.a >= 0 && parts[cidx.a] != null ? to255(parts[cidx.a]) : null;

  return { r, g, b, a };
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

// Small heap-allocated temp to avoid constructing Point3D for array targets
const tempPoint3D = (x, y, z) => ({ x, y, z });
