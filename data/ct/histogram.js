import fs from "fs";
import zlib from "zlib";

/**
 * Reads a (single-file) .nrrd and returns:
 *  - header: parsed header object
 *  - histogram: Map-like object keyed by bin value/count (0..255 for uint8, etc)
 *  - stats: min/max/mean and percentiles
 *
 * Notes:
 * - Supports encoding: raw | gzip
 * - Supports types: uint8/uchar, int16/short, uint16/ushort, int32/int, float, double
 */
export const histogramFromNRRDFile = (filePath) => {
  const buf = fs.readFileSync(filePath);

  const headerEnd = buf.indexOf("\n\n");
  if (headerEnd === -1)
    throw new Error("Invalid NRRD: missing header terminator");

  const headerText = buf.slice(0, headerEnd).toString("ascii");
  const payload = buf.slice(headerEnd + 2);

  const header = {};
  for (const line of headerText.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const colon = line.indexOf(":");
    if (colon === -1) {
      header[line.trim()] = undefined; // e.g. NRRD0005
      continue;
    }
    const key = line.slice(0, colon).trim();
    const value = line.slice(colon + 1).trim();
    header[key] = value;
  }

  const encoding = (header.encoding || "raw").toLowerCase();
  const type = (header.type || "").toLowerCase();
  const endian = (header.endian || "little").toLowerCase();

  if (!type) throw new Error("NRRD header missing: type");
  if (encoding !== "raw" && encoding !== "gzip") {
    throw new Error(`Unsupported NRRD encoding: ${encoding}`);
  }
  if (endian !== "little") {
    throw new Error(
      `Unsupported endian: ${endian} (this function expects little-endian)`,
    );
  }

  const rawBytes =
    encoding === "gzip" ? Buffer.from(zlib.gunzipSync(payload)) : payload;

  const typeInfo = (() => {
    // minimal but practical mapping
    if (type === "uchar" || type === "uint8")
      return { ctor: Uint8Array, isInt: true, bits: 8 };
    if (type === "short" || type === "int16")
      return { ctor: Int16Array, isInt: true, bits: 16 };
    if (type === "ushort" || type === "uint16")
      return { ctor: Uint16Array, isInt: true, bits: 16 };
    if (type === "int" || type === "int32")
      return { ctor: Int32Array, isInt: true, bits: 32 };
    if (type === "float") return { ctor: Float32Array, isInt: false, bits: 32 };
    if (type === "double")
      return { ctor: Float64Array, isInt: false, bits: 64 };
    return null;
  })();

  if (!typeInfo) throw new Error(`Unsupported NRRD type: ${type}`);

  const { ctor: TypedArray } = typeInfo;

  const view = new TypedArray(
    rawBytes.buffer,
    rawBytes.byteOffset,
    Math.floor(rawBytes.byteLength / TypedArray.BYTES_PER_ELEMENT),
  );

  // ---- histogram ----
  // Strategy:
  // - uint8: exact bins 0..255
  // - other ints: auto-bin into 4096 bins over [min..max]
  // - floats: auto-bin into 4096 bins over [min..max]
  const n = view.length;
  if (n === 0) throw new Error("NRRD has no data");

  let min = view[0];
  let max = view[0];
  let sum = 0;

  for (let i = 0; i < n; i++) {
    const v = view[i];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
  }

  const mean = sum / n;

  const makePercentiles = (pList) => {
    // For huge arrays, full sort is expensive; do a histogram-based percentile estimate.
    // If uint8, we can do exact.
    if (TypedArray === Uint8Array) {
      const h = new Uint32Array(256);
      for (let i = 0; i < n; i++) h[view[i]]++;
      const out = {};
      for (const p of pList) {
        const target = Math.floor(p * (n - 1));
        let cum = 0;
        for (let b = 0; b < 256; b++) {
          cum += h[b];
          if (cum > target) {
            out[`p${Math.round(p * 1000) / 10}`] = b;
            break;
          }
        }
      }
      return out;
    }

    // approximate percentiles via coarse bins
    const bins = 4096;
    const h = new Uint32Array(bins);
    const range = max - min || 1;
    for (let i = 0; i < n; i++) {
      const t = (view[i] - min) / range;
      const b = Math.min(bins - 1, Math.max(0, Math.floor(t * (bins - 1))));
      h[b]++;
    }

    const out = {};
    for (const p of pList) {
      const target = Math.floor(p * (n - 1));
      let cum = 0;
      for (let b = 0; b < bins; b++) {
        cum += h[b];
        if (cum > target) {
          const val = min + (b / (bins - 1)) * range;
          out[`p${Math.round(p * 1000) / 10}`] = val;
          break;
        }
      }
    }
    return out;
  };

  let histogram;
  let histogramMeta;

  if (TypedArray === Uint8Array) {
    const h = new Uint32Array(256);
    for (let i = 0; i < n; i++) h[view[i]]++;
    histogram = h;
    histogramMeta = { kind: "uint8", bins: 256, binMin: 0, binMax: 255 };
  } else {
    const bins = 4096;
    const h = new Uint32Array(bins);
    const range = max - min || 1;
    for (let i = 0; i < n; i++) {
      const t = (view[i] - min) / range;
      const b = Math.min(bins - 1, Math.max(0, Math.floor(t * (bins - 1))));
      h[b]++;
    }
    histogram = h;
    histogramMeta = { kind: "auto", bins, binMin: min, binMax: max };
  }

  const percentiles = makePercentiles([
    0.5, 0.75, 0.9, 0.95, 0.98, 0.99, 0.995,
  ]);

  return {
    header,
    stats: {
      count: n,
      min,
      max,
      mean,
      ...percentiles,
    },
    histogramMeta,
    histogram, // Uint32Array
  };
};

/**
 * Reads an NRRD, computes a density histogram, and logs a concise summary
 * plus the non-empty bins.
 */
export const logNRRDHistogram = (filePath) => {
  const { header, stats, histogram, histogramMeta } =
    histogramFromNRRDFile(filePath);

  console.log("=== NRRD SUMMARY ===");
  console.log({
    type: header.type,
    encoding: header.encoding,
    dimension: header.dimension,
    sizes: header.sizes,
  });

  console.log("\n=== STATS ===");
  console.log(stats);

  console.log("\n=== HISTOGRAM ===");

  if (histogramMeta.kind === "uint8") {
    for (let v = 0; v < histogram.length; v++) {
      const c = histogram[v];
      if (c > 0) {
        console.log(`${v}: ${c}`);
      }
    }
  } else {
    const { bins, binMin, binMax } = histogramMeta;
    const step = (binMax - binMin) / (bins - 1);
    for (let b = 0; b < bins; b++) {
      const c = histogram[b];
      if (c > 0) {
        const lo = binMin + b * step;
        const hi = lo + step;
        console.log(`[${lo.toFixed(3)}, ${hi.toFixed(3)}): ${c}`);
      }
    }
  }
};

logNRRDHistogram("one.nrrd");
