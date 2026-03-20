import zlib from "zlib";

/**
 * @param {Uint8Array} gzipped
 * @returns {Uint8Array}
 */
export const gunzipUint8 = (gzipped) =>
  new Uint8Array(zlib.gunzipSync(gzipped));

/**
 * @param {Uint8Array} vox
 */
export const summarizeUint8 = (vox) => {
  const hist = new Uint32Array(256);

  let min = 255,
    max = 0;
  for (let i = 0; i < vox.length; i++) {
    const v = vox[i];
    hist[v]++;
    if (v < min) min = v;
    if (v > max) max = v;
  }

  const total = vox.length;
  const pct = (p) => {
    const target = Math.floor(p * (total - 1));
    let cum = 0;
    for (let v = 0; v < 256; v++) {
      cum += hist[v];
      if (cum > target) return v;
    }
    return 255;
  };

  const nonzero = total - hist[0];
  const nonzeroPct = (100 * nonzero) / total;

  console.log({
    total,
    min,
    max,
    nonzeroPct: Number(nonzeroPct.toFixed(3)),
    p50: pct(0.5),
    p75: pct(0.75),
    p90: pct(0.9),
    p95: pct(0.95),
    p98: pct(0.98),
    p99: pct(0.99),
    p995: pct(0.995),
  });

  // Optional: show where the first big jump in cumulative mass starts
  let cum = 0;
  for (let v = 0; v < 256; v++) {
    cum += hist[v];
    if (cum / total >= 0.9) {
      console.log("90% of voxels are <= ", v);
      break;
    }
  }
};
