// out/statcast2ply.js
// Usage: node out/statcast2ply.js statcast.csv hits.ply
// ES modules, named exports, arrow functions.

import fs from "fs";
import readline from "readline";
import path from "path";

const G = 32.174; // ft/s^2
const POINTS_PER_TRAJ = 500;
const MIN_LAUNCH_ANGLE_DEG = 15; // filter grounders

const strip = (s) => (s ?? "").trim().replace(/^"(.*)"$/s, "$1");

// split line into fields, supporting comma OR tab and quoted cells
const splitRow = (line) => {
  const delim = line.includes("\t") ? "\t" : ",";
  const out = [];
  let cur = "";
  let inQ = false;
  for (let i = 0; i < line.length; i++) {
    const ch = line[i];
    if (ch === '"') {
      if (inQ && line[i + 1] === '"') {
        cur += '"';
        i++;
      } else {
        inQ = !inQ;
      }
    } else if (ch === delim && !inQ) {
      out.push(strip(cur));
      cur = "";
    } else cur += ch;
  }
  out.push(strip(cur));
  return out;
};

const toNum = (v) => {
  const n = parseFloat(strip(v));
  return Number.isFinite(n) ? n : NaN;
};

const getDistance = (row) => {
  for (const k of ["bbdist", "hit_distance_sc", "distance_feet"]) {
    const n = toNum(row[k]);
    if (Number.isFinite(n) && n > 0) return n;
  }
  return NaN;
};

// robust quantile
const quantile = (arr, q) => {
  if (!arr.length) return NaN;
  const a = [...arr].sort((x, y) => x - y);
  const idx = (a.length - 1) * q;
  const lo = Math.floor(idx),
    hi = Math.ceil(idx);
  if (lo === hi) return a[lo];
  return a[lo] + (a[hi] - a[lo]) * (idx - lo);
};

// color map: red→yellow→green with percentile scaling
const speedToRGBFactory = (speeds) => {
  // Use P10..P90 for variance; fall back to wide range if degenerate
  let min = quantile(speeds, 0.1);
  let max = quantile(speeds, 0.9);
  if (!Number.isFinite(min) || !Number.isFinite(max) || min >= max) {
    min = 60;
    max = 115; // reasonable fallback for EV
  }
  const map = (speed) => {
    const t = Math.max(
      0,
      Math.min(1, (speed - min) / Math.max(1e-9, max - min))
    );
    let r,
      g,
      b = 0;
    if (t < 0.5) {
      r = 255;
      g = Math.round(510 * t);
    } else {
      g = 255;
      r = Math.round(510 * (1 - t));
    }
    return [r, g, b];
  };
  return { map, min, max };
};

const flightTime = (vz) => (vz > 0 ? (2 * vz) / G : 0);

const makeTrajectoryPoints = (
  v0_fps,
  launchAngleRad,
  dirRad,
  targetRangeFt,
  color
) => {
  const vx = v0_fps * Math.cos(launchAngleRad) * Math.cos(dirRad);
  const vy = v0_fps * Math.cos(launchAngleRad) * Math.sin(dirRad);
  const vz = v0_fps * Math.sin(launchAngleRad);

  const T = flightTime(vz);
  if (T <= 0) return [];

  const xL = vx * T,
    yL = vy * T;
  const simRange = Math.hypot(xL, yL) || 1e-9;
  const scale = targetRangeFt / simRange;

  const pts = [];
  for (let i = 0; i < POINTS_PER_TRAJ; i++) {
    const t = (i / (POINTS_PER_TRAJ - 1)) * T;
    const x = vx * t * scale;
    const y = vy * t * scale;
    const z = Math.max(0, vz * t - 0.5 * G * t * t);
    pts.push([x, y, z, ...color]);
  }
  return pts;
};

export const run = async () => {
  const [, , inCsv, outPly] = process.argv;
  if (!inCsv || !outPly) {
    console.error("Usage: node out/statcast2ply.js statcast.csv hits.ply");
    process.exit(1);
  }

  const rl = readline.createInterface({
    input: fs.createReadStream(inCsv),
    crlfDelay: Infinity,
  });

  let headers = null;
  const rows = [];
  for await (const line of rl) {
    if (!line.trim()) continue;
    if (!headers) {
      headers = splitRow(line);
      continue;
    }
    const cols = splitRow(line);
    while (cols.length < headers.length) cols.push("");
    const obj = {};
    for (let i = 0; i < headers.length; i++) obj[strip(headers[i])] = cols[i];
    rows.push(obj);
  }

  // Filter & collect candidates (exclude grounders)
  const candidates = [];
  for (const r of rows) {
    const launchSpeed_mph = toNum(r.launch_speed);
    const launchAngle_deg = toNum(r.launch_angle);
    if (
      !Number.isFinite(launchAngle_deg) ||
      launchAngle_deg < MIN_LAUNCH_ANGLE_DEG
    )
      continue;

    const attackDir_deg = Number.isFinite(toNum(r.attack_direction))
      ? toNum(r.attack_direction)
      : 0;

    const dist_ft = getDistance(r);

    if (
      Number.isFinite(launchSpeed_mph) &&
      Number.isFinite(dist_ft) &&
      launchSpeed_mph > 0 &&
      dist_ft > 0
    ) {
      candidates.push({
        launchSpeed_mph,
        launchAngle_deg,
        attackDir_deg,
        dist_ft,
      });
    }
  }

  if (!candidates.length) {
    const empty = [
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
    ].join("\n");
    fs.writeFileSync(outPly, empty);
    console.log(
      `Used 0 hits after filtering (launch_angle >= ${MIN_LAUNCH_ANGLE_DEG}°). Wrote 0 points to ${outPly}`
    );
    process.exit(0);
  }

  // Color scaling by P10..P90 for variance
  const speeds = candidates.map((c) => c.launchSpeed_mph);
  const { map: mapColor, min: minS, max: maxS } = speedToRGBFactory(speeds);

  const points = [];
  for (const c of candidates) {
    const v0_fps = c.launchSpeed_mph * 1.46667;
    const ang = (c.launchAngle_deg * Math.PI) / 180;
    const dir = (c.attackDir_deg * Math.PI) / 180;
    const color = mapColor(c.launchSpeed_mph);
    const traj = makeTrajectoryPoints(v0_fps, ang, dir, c.dist_ft, color);
    points.push(...traj);
  }

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
  fs.writeFileSync(outPly, header + "\n" + body + "\n");
  console.log(
    `Used ${candidates.length} hits (launch_angle >= ${MIN_LAUNCH_ANGLE_DEG}°); wrote ${points.length} points to ${outPly}. ` +
      `Color EV range (P10..P90): ${minS.toFixed(1)}–${maxS.toFixed(1)} mph`
  );
};

if (
  process.argv[1] &&
  path.basename(process.argv[1]) ===
    path.basename(new URL(import.meta.url).pathname)
) {
  run().catch((e) => {
    console.error(e);
    process.exit(1);
  });
}
