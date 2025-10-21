// png.js — uncompressed, standards-correct PNG writer (ESM, named export)
// Writes 8-bit RGBA, filter=0, no interlace, zlib wrapper + Adler32.
// Usage:
//   import { PNG } from "./png.js";
//   const img = new PNG(200, 200);
//   img.setPixel(0, 0, 255, 0, 0); // alpha defaults to 255
//   await img.flush("out.png");

import { promises as fs } from "fs";

/** ---- CRC32 ---- */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

const crc32 = (bytes) => {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++)
    c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};

const writeChunk = (typeStr, data) => {
  const type = Buffer.from(typeStr, "ascii");
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const crcBuf = Buffer.alloc(4);
  const crc = crc32(Buffer.concat([type, data]));
  crcBuf.writeUInt32BE(crc >>> 0, 0);
  return Buffer.concat([len, type, data, crcBuf]);
};

const u32be = (n) => {
  const b = Buffer.alloc(4);
  b.writeUInt32BE(n >>> 0, 0);
  return b;
};

/** ---- Adler-32 (for zlib trailer) ---- */
const adler32 = (bytes) => {
  let a = 1,
    d = 0;
  const MOD = 65521;
  for (let i = 0; i < bytes.length; i++) {
    a += bytes[i];
    if (a >= MOD) a -= MOD;
    d += a;
    if (d >= MOD) d -= MOD;
  }
  return ((d << 16) | a) >>> 0;
};

/** ---- PNG (8-bit RGBA) ---- */
export class PNG {
  constructor(width, height) {
    if (
      !Number.isInteger(width) ||
      !Number.isInteger(height) ||
      width <= 0 ||
      height <= 0
    ) {
      throw new Error("PNG(width, height) requires positive integers.");
    }
    this.width = width;
    this.height = height;
    this.stride = width * 4; // RGBA
    this.pixels = new Uint8Array(this.stride * height); // zero init = transparent
  }

  /** Clear all pixels to transparent black (0,0,0,0) */
  clear = () => {
    this.pixels.fill(0);
  };

  getPixel = (x, y) => {
    const i = y * this.stride + (x << 2);
    return [
      this.pixels[i],
      this.pixels[i + 1],
      this.pixels[i + 2],
      this.pixels[i + 3],
    ];
  };

  setPixel = (x, y, r, g, b, a = 255) => {
    const i = y * this.stride + (x << 2);
    this.pixels[i] = r | 0;
    this.pixels[i + 1] = g | 0;
    this.pixels[i + 2] = b | 0;
    this.pixels[i + 3] = a | 0;
  };

  /**
   * Flood-fill from (x,y), replacing the contiguous region matching the seed pixel's RGBA
   * with the given color. 4-neighborhood (N,S,E,W). Returns number of pixels changed.
   */
  floodFillFrom = (x, y, r, g, b, a = 255) => {
    if (x < 0 || y < 0 || x >= this.width || y >= this.height) return 0;

    // Views for fast 32-bit comparisons/writes (little-endian layout: r | g<<8 | b<<16 | a<<24)
    const u8 = this.pixels;
    const u32 = new Uint32Array(u8.buffer, u8.byteOffset, u8.byteLength >>> 2);

    const idx = (yy, xx) => yy * this.width + xx;

    const i0 = idx(y, x);
    // Compose colors into 32-bit little-endian words
    const seedR = u8[i0 << 2];
    const seedG = u8[(i0 << 2) + 1];
    const seedB = u8[(i0 << 2) + 2];
    const seedA = u8[(i0 << 2) + 3];
    const target = (seedR | (seedG << 8) | (seedB << 16) | (seedA << 24)) >>> 0;
    const fill =
      (r | 0 | ((g | 0) << 8) | ((b | 0) << 16) | ((a | 0) << 24)) >>> 0;

    if (target === fill) return 0;

    let filled = 0;
    const stack = [i0];

    while (stack.length) {
      const i = stack.pop();
      if (u32[i] !== target) continue;

      // Expand west
      let xL = i % this.width;
      let yL = (i / this.width) | 0;
      let left = i;
      while (xL >= 0 && u32[left] === target) {
        u32[left] = fill;
        filled++;
        xL--;
        left--;
      }

      // Expand east
      let xR = (i % this.width) + 1;
      let right = i + 1;
      while (xR < this.width && u32[right] === target) {
        u32[right] = fill;
        filled++;
        xR++;
        right++;
      }

      // Now push spans above and below between (left+1) and (right-1)
      const start = left + 1;
      const end = right - 1;

      // Above
      if (yL > 0) {
        const up = (yL - 1) * this.width;
        for (let xx = start; xx <= end; xx++) {
          const j = up + (xx % this.width);
          if (u32[j] === target) stack.push(j);
        }
      }
      // Below
      if (yL < this.height - 1) {
        const dn = (yL + 1) * this.width;
        for (let xx = start; xx <= end; xx++) {
          const j = dn + (xx % this.width);
          if (u32[j] === target) stack.push(j);
        }
      }
    }

    return filled;
  };

  /** Count pixels with non-zero alpha (A > 0). */
  countFilledPixels = () => {
    let count = 0;
    for (let y = 0; y < this.height; y++) {
      let i = y * this.stride + 3; // alpha
      for (let x = 0; x < this.width; x++, i += 4)
        if (this.pixels[i] !== 0) count++;
    }
    return count;
  };

  /** Build zlib stream with uncompressed DEFLATE blocks (≤ 65535 each) */
  #zlibNoCompression = (raw) => {
    const zlibHeader = Buffer.from([0x78, 0x01]); // CMF/FLG for no compression

    const blocks = [];
    let off = 0;
    while (off < raw.length) {
      const remaining = raw.length - off;
      const blockSize = Math.min(remaining, 65535);
      const bfinal = off + blockSize >= raw.length ? 1 : 0;
      const header = Buffer.from([bfinal]); // BFINAL + BTYPE=00
      const lenLE = Buffer.alloc(2);
      const nlenLE = Buffer.alloc(2);
      lenLE.writeUInt16LE(blockSize, 0);
      nlenLE.writeUInt16LE(~blockSize & 0xffff, 0);
      const data = raw.subarray(off, off + blockSize);
      blocks.push(Buffer.concat([header, lenLE, nlenLE, data]));
      off += blockSize;
    }

    const adler = Buffer.alloc(4);
    adler.writeUInt32BE(adler32(raw) >>> 0, 0);

    return Buffer.concat([zlibHeader, ...blocks, adler]);
  };

  flush = async (fileName) => {
    if (!fileName || typeof fileName !== "string")
      throw new Error("flush(fileName) requires a path.");

    const SIGNATURE = Buffer.from([
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    ]);

    const ihdr = Buffer.concat([
      u32be(this.width),
      u32be(this.height),
      Buffer.from([8, 6, 0, 0, 0]), // bit depth=8, RGBA, deflate, filter=0, no interlace
    ]);
    const IHDR = writeChunk("IHDR", ihdr);

    const scanlines = Buffer.alloc(this.height * (1 + this.stride));
    let src = 0,
      dst = 0;
    for (let y = 0; y < this.height; y++) {
      scanlines[dst++] = 0; // filter type None
      scanlines.set(this.pixels.subarray(src, src + this.stride), dst);
      src += this.stride;
      dst += this.stride;
    }

    const zlibStream = this.#zlibNoCompression(scanlines);
    const IDAT = writeChunk("IDAT", zlibStream);
    const IEND = writeChunk("IEND", Buffer.alloc(0));

    const out = Buffer.concat([SIGNATURE, IHDR, IDAT, IEND]);
    await fs.writeFile(fileName, out);
    return out;
  };
}
