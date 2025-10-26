// flights2csv.js — ES modules, named exports, arrow functions.
// Usage: node flights2csv.js flights.csv out.ply [sampleIntervalSeconds]
// Notes: Colors encode speed (m/s): red=slow → green=fast. Uses ground+vertical speed.
//        For ground speed only, change dist to Math.hypot(dx, dy).

import fs from "fs";
import csv from "csv-parser";

export const flightsCsvToPly = async (
  inputPath,
  outputPath,
  sampleIntervalArg
) => {
  const sampleInterval = Number.isFinite(Number(sampleIntervalArg))
    ? Number(sampleIntervalArg)
    : 5;

  const R = 6_371_000; // Earth radius (m)
  const LAT0_DEG = 38.7487;
  const LON0_DEG = -90.37;
  const LAT0 = (LAT0_DEG * Math.PI) / 180;
  const LON0 = (LON0_DEG * Math.PI) / 180;

  const headers = ["icao24", "callsign", "time", "lat", "lon", "altitude"];
  const tracks = new Map();

  // --- read CSV ---
  await new Promise((resolve, reject) => {
    fs.createReadStream(inputPath)
      .pipe(csv({ headers }))
      .on("data", (row) => {
        const id = row.icao24?.trim();
        const t = Number(row.time);
        const lat = parseFloat(row.lat);
        const lon = parseFloat(row.lon);
        const alt = parseFloat(row.altitude ?? "0");
        if (!id || Number.isNaN(t) || Number.isNaN(lat) || Number.isNaN(lon))
          return;
        if (!tracks.has(id)) tracks.set(id, []);
        tracks.get(id).push({ t, lat, lon, alt });
      })
      .on("end", resolve)
      .on("error", reject);
  });

  const toLocal = (latDeg, lonDeg, altM) => {
    const phi = (latDeg * Math.PI) / 180;
    const lam = (lonDeg * Math.PI) / 180;
    const x = (lam - LON0) * Math.cos(LAT0) * R;
    const y = (phi - LAT0) * R;
    const z = altM;
    return { x, y, z };
  };

  // --- subsample and compute speeds ---
  const points = [];
  for (const samples of tracks.values()) {
    samples.sort((a, b) => a.t - b.t);
    if (samples.length === 0) continue;

    let lastKept = samples[0];
    let lastXYZ = toLocal(lastKept.lat, lastKept.lon, lastKept.alt);
    points.push({ ...lastXYZ, t: lastKept.t, speed: 0 });

    for (let i = 1; i < samples.length; i++) {
      const s = samples[i];
      if (s.t - lastKept.t >= sampleInterval) {
        const cur = toLocal(s.lat, s.lon, s.alt);
        const dt = s.t - lastKept.t;
        const dx = cur.x - lastXYZ.x;
        const dy = cur.y - lastXYZ.y;
        const dz = cur.z - lastXYZ.z;
        const dist = Math.hypot(dx, dy, dz); // use Math.hypot(dx, dy) for ground speed only
        const v = dist / dt; // m/s
        points.push({ ...cur, t: s.t, speed: v });
        lastKept = s;
        lastXYZ = cur;
      }
    }
  }

  // --- handle empty ---
  if (points.length === 0) {
    fs.writeFileSync(
      outputPath,
      [
        "ply",
        "format ascii 1.0",
        "element vertex 0",
        "property float x",
        "property float y",
        "property float z",
        "property uchar red",
        "property uchar green",
        "property uchar blue",
        "end_header",
        "",
      ].join("\n")
    );
    return;
  }

  const speeds = points
    .map((p) => p.speed)
    .filter(Number.isFinite)
    .sort((a, b) => a - b);
  const q = (arr, q) => arr[Math.floor(q * (arr.length - 1))];
  const vMin = q(speeds, 0.05);
  const vMax = q(speeds, 0.95);

  const norm = (v) => {
    const u = (v - vMin) / (vMax - vMin);
    return u < 0 ? 0 : u > 1 ? 1 : u;
  };

  const toRGB = (v) => {
    const u = norm(v); // 0..1
    let r, g, b;

    if (u < 0.5) {
      // red → yellow
      const k = u / 0.5;
      r = 255;
      g = Math.round(255 * k);
      b = 0;
    } else {
      // yellow → darker green
      const k = (u - 0.5) / 0.5;
      r = Math.round(255 - 205 * k); // 255→50
      g = Math.round(255 - 55 * (1 - k)); // 200→255 roughly mid-tone
      b = 0;
    }

    return { r, g, b };
  };

  // --- write PLY (ASCII) ---
  const header =
    [
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
    ].join("\n") + "\n";

  const body = points
    .map((p) => {
      const { r, g, b } = toRGB(p.speed);
      return `${p.x.toFixed(2)} ${p.y.toFixed(2)} ${p.z.toFixed(
        2
      )} ${r} ${g} ${b}`;
    })
    .join("\n");

  fs.writeFileSync(outputPath, header + body);
};

// --- CLI ---
if (import.meta.url === `file://${process.argv[1]}`) {
  const [, , INPUT, OUTPUT, SAMPLE] = process.argv;
  if (!INPUT || !OUTPUT) {
    console.error(
      "Usage: node flights2csv.js input.csv output.ply [sampleIntervalSeconds]"
    );
    process.exit(1);
  }
  flightsCsvToPly(INPUT, OUTPUT, SAMPLE)
    .then(() => console.log(`✅ Wrote PLY → ${OUTPUT}`))
    .catch((e) => {
      console.error(e?.stack || e?.message || String(e));
      process.exit(1);
    });
}
