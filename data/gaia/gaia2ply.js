// gaia2ply_galacto_text.js
// Usage: node gaia2ply_galacto_text.js input.csv output.ply
// Input columns (ESA/DR3): ra, dec, parallax, (optional) bp_rp, phot_bp_mean_mag, phot_rp_mean_mag, phot_g_mean_mag
// Output: ASCII PLY with Galactocentric X,Y,Z (pc) and RGB.

import fs from "fs";
import { parse } from "csv-parse";

// ---------- constants ----------
export const deg2rad = (d) => (d * Math.PI) / 180;
const RA_NGP = deg2rad(192.85948); // α_NGP
const DEC_NGP = deg2rad(27.12825); // δ_NGP
const L_OMEGA = deg2rad(32.93192); // ℓ of ascending node
const R0_PC = 8200; // Sun–GC distance in pc (8.2 kpc)
const Z_SUN_PC = 20; // Sun above plane (≈20 pc)
const Z_MAX = 150; // thin-disk half-thickness filter (pc)

// ---------- helpers ----------
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// ICRS (ra,dec) → Galactic (l,b), radians
export const icrsToGal = (raDeg, decDeg) => {
  const ra = deg2rad(raDeg),
    dec = deg2rad(decDeg);
  const sinb =
    Math.sin(dec) * Math.sin(DEC_NGP) +
    Math.cos(dec) * Math.cos(DEC_NGP) * Math.cos(ra - RA_NGP);
  const y = Math.cos(dec) * Math.sin(ra - RA_NGP);
  const x =
    Math.sin(dec) * Math.cos(DEC_NGP) -
    Math.cos(dec) * Math.sin(DEC_NGP) * Math.cos(ra - RA_NGP);
  let l = Math.atan2(y, x) + L_OMEGA;
  if (l < 0) l += 2 * Math.PI;
  const b = Math.asin(clamp(sinb, -1, 1));
  return [l, b];
};

// Distance from parallax (mas) → pc (only safe for high SNR; we already queried for that)
export const parallaxMasToParsec = (p_mas) => 1000 / p_mas;

// Galactic heliocentric (l,b,d_pc) → heliocentric Cartesian (pc), x toward GC
export const galHelioToCartesian = (l, b, d) => {
  const cb = Math.cos(b),
    sb = Math.sin(b);
  const cl = Math.cos(l),
    sl = Math.sin(l);
  const xh = d * cb * cl; // toward GC
  const yh = d * cb * sl; // direction of rotation
  const zh = d * sb; // toward NGP
  return [xh, yh, zh];
};

// Convert to Galactocentric (pc). Convention here: GC at (0,0,0); Sun at (X=R0, Y=0, Z=Z_SUN).
export const helioToGalactocentric = (xh, yh, zh) => {
  const X = R0_PC - xh;
  const Y = yh;
  const Z = zh + Z_SUN_PC;
  return [X, Y, Z];
};

// BP−RP → pleasant RGB gradient (display only)
export const rgbFromBpRp = (bp_rp) => {
  const c = clamp(bp_rp, -0.5, 3.5);
  const stops = [
    { t: -0.5, rgb: [155, 176, 255] },
    { t: 0.8, rgb: [255, 255, 255] },
    { t: 1.3, rgb: [255, 245, 200] },
    { t: 2.0, rgb: [255, 210, 160] },
    { t: 3.5, rgb: [255, 160, 122] },
  ];
  let i = 0;
  while (i + 1 < stops.length && !(c >= stops[i].t && c <= stops[i + 1].t)) i++;
  if (i + 1 === stops.length) return stops.at(-1).rgb;
  const a = stops[i],
    b = stops[i + 1];
  const u = (c - a.t) / (b.t - a.t);
  return [
    Math.round(a.rgb[0] + u * (b.rgb[0] - a.rgb[0])),
    Math.round(a.rgb[1] + u * (b.rgb[1] - a.rgb[1])),
    Math.round(a.rgb[2] + u * (b.rgb[2] - a.rgb[2])),
  ];
};

export const brightnessFromGmag = (gmag) => {
  if (!isFinite(gmag)) return 1.0;
  const flux = Math.pow(10, -0.4 * (gmag - 12));
  return clamp(Math.pow(flux, 0.25), 0.15, 1.0);
};

// ---------- main ----------
export const run = async (inCsv, outPly) => {
  if (!inCsv || !outPly) {
    console.error("Usage: node gaia2ply_galacto_text.js input.csv output.ply");
    process.exit(1);
  }

  const parser = fs
    .createReadStream(inCsv)
    .pipe(parse({ columns: true, relax_quotes: true }));
  const rows = [];

  for await (const row of parser) {
    const ra = Number(row.ra);
    const dec = Number(row.dec);
    const p = Number(row.parallax);
    if (!isFinite(ra) || !isFinite(dec) || !isFinite(p) || p <= 0) continue;

    const d_pc = parallaxMasToParsec(p);
    const [l, b] = icrsToGal(ra, dec);
    const [xh, yh, zh] = galHelioToCartesian(l, b, d_pc);
    const [X, Y, Z] = helioToGalactocentric(xh, yh, zh);

    // Keep thin disk
    if (Math.abs(Z) > Z_MAX) continue;

    // Color
    let bp_rp = Number(row.bp_rp ?? NaN);
    if (!isFinite(bp_rp) && row.phot_bp_mean_mag && row.phot_rp_mean_mag) {
      bp_rp = Number(row.phot_bp_mean_mag) - Number(row.phot_rp_mean_mag);
    }
    let [r, g, bRGB] = rgbFromBpRp(isFinite(bp_rp) ? bp_rp : 0.8);
    if (row.phot_g_mean_mag) {
      const s = brightnessFromGmag(Number(row.phot_g_mean_mag));
      r = Math.round(r * s);
      g = Math.round(g * s);
      bRGB = Math.round(bRGB * s);
    }

    rows.push(`${X} ${Y} ${Z} ${r} ${g} ${bRGB}`);
  }

  if (rows.length === 0) {
    console.error("No rows after filtering (check columns and Z_MAX).");
    process.exit(1);
  }

  const header = [
    "ply",
    "format ascii 1.0",
    `element vertex ${rows.length}`,
    "property float x",
    "property float y",
    "property float z",
    "property uchar red",
    "property uchar green",
    "property uchar blue",
    "end_header",
  ].join("\n");

  fs.writeFileSync(outPly, `${header}\n${rows.join("\n")}\n`);
  console.log(`Wrote ${rows.length} vertices → ${outPly}`);
};

if (import.meta.url === `file://${process.argv[1]}`) {
  const [, , inCsv, outPly] = process.argv;
  run(inCsv, outPly);
}
