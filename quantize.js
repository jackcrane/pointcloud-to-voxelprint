import cliProgress from "cli-progress";
import fs from "fs";
import { promises as fsPromises } from "fs";
import os from "os";
import path from "path";
import {
  getVertexFieldIndices,
  normalizeColorValue,
  parseHeaderText,
  pickReader,
  readColorFromParts,
} from "./util/ply-format.js";

const { SingleBar, Presets } = cliProgress;

const SIZE_X_IN = 0.5;
const SIZE_Y_IN = 0.5;
const SIZE_Z_IN = 0.25;

const DPI_X = 300;
const DPI_Y = 300;
const DPI_Z = 300;

const SHARD_COUNT = 128;
const SHARD_RECORD_BYTES = 12;
const OUTPUT_RECORD_BYTES = 16;
const SHARD_BUFFER_BYTES = 1024 * 1024;
const STREAM_CHUNK_BYTES = 1024 * 1024;
const PROGRESS_BATCH = 50_000;

const DEFAULT_COLOR = Object.freeze({ r: 255, g: 255, b: 255, a: 255 });

const run = async () => {
  const [inputPathArg, outputPathArg] = process.argv.slice(2);
  if (!inputPathArg || !outputPathArg) {
    console.error("Usage: node quantize.js <input.ply> <output.ply>");
    process.exit(1);
  }

  const inputPath = path.resolve(inputPathArg);
  const outputPath = path.resolve(outputPathArg);

  if (inputPath === outputPath) {
    throw new Error("Input and output paths must be different.");
  }

  await fsPromises.access(inputPath, fs.constants.R_OK);
  await fsPromises.mkdir(path.dirname(outputPath), { recursive: true });

  const headerInfo = await readPlyHeader(inputPath);
  const { header } = headerInfo;
  const vertexCount = header.vertex?.count ?? 0;

  if (
    !header.format.startsWith("ascii") &&
    !header.format.startsWith("binary_little_endian")
  ) {
    throw new Error(`Unsupported PLY format: ${header.format}`);
  }

  if (!header.vertex) {
    throw new Error("PLY has no vertex element.");
  }

  if (vertexCount === 0) {
    await writeEmptyPly(outputPath);
    console.log("Input has no vertices. Wrote an empty quantized PLY.");
    return;
  }

  const bounds = {
    minX: Infinity,
    minY: Infinity,
    minZ: Infinity,
    maxX: -Infinity,
    maxY: -Infinity,
    maxZ: -Infinity,
  };

  await forEachVertex(
    inputPath,
    headerInfo,
    ({ x, y, z }) => {
      if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) {
        return;
      }
      if (x < bounds.minX) bounds.minX = x;
      if (y < bounds.minY) bounds.minY = y;
      if (z < bounds.minZ) bounds.minZ = z;
      if (x > bounds.maxX) bounds.maxX = x;
      if (y > bounds.maxY) bounds.maxY = y;
      if (z > bounds.maxZ) bounds.maxZ = z;
    },
    { label: "Scan Bounds" },
  );

  if (!Number.isFinite(bounds.minX)) {
    await writeEmptyPly(outputPath);
    console.log(
      "Input had no valid numeric vertices. Wrote an empty quantized PLY.",
    );
    return;
  }

  const grid = {
    x: Math.max(1, Math.round(SIZE_X_IN * DPI_X)),
    y: Math.max(1, Math.round(SIZE_Y_IN * DPI_Y)),
    z: Math.max(1, Math.round(SIZE_Z_IN * DPI_Z)),
  };

  const scaler = buildScaler(bounds, grid);
  const tempDir = await fsPromises.mkdtemp(
    path.join(os.tmpdir(), "pointcloud-quantize-"),
  );

  try {
    const shardPaths = Array.from({ length: SHARD_COUNT }, (_, index) =>
      path.join(tempDir, `shard-${String(index).padStart(3, "0")}.bin`),
    );
    const shardWriters = await Promise.all(
      shardPaths.map((shardPath) =>
        BufferedShardWriter.create(shardPath, SHARD_BUFFER_BYTES),
      ),
    );

    let shardedPointCount = 0;

    await forEachVertex(
      inputPath,
      headerInfo,
      async ({ x, y, z, color }) => {
        if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) {
          return;
        }

        const ix = scaler.quantizeX(x);
        const iy = scaler.quantizeY(y);
        const iz = scaler.quantizeZ(z);

        const cellId =
          BigInt(ix) +
          scaler.gridXBig * (BigInt(iy) + scaler.gridYBig * BigInt(iz));
        const packedColor = packColor(color ?? DEFAULT_COLOR);
        const shardIndex = Number(cellId % BigInt(SHARD_COUNT));

        const maybeFlush = shardWriters[shardIndex].append(cellId, packedColor);
        if (maybeFlush) await maybeFlush;
        shardedPointCount++;
      },
      { label: "Shard Points" },
    );

    await Promise.all(shardWriters.map((writer) => writer.close()));

    if (shardedPointCount === 0) {
      await writeEmptyPly(outputPath);
      console.log(
        "Input had no valid numeric vertices to quantize. Wrote an empty PLY.",
      );
      return;
    }

    const outputDataPath = path.join(tempDir, "quantized-vertices.bin");
    const outputWriter = await BufferedOutputWriter.create(
      outputDataPath,
      SHARD_BUFFER_BYTES,
    );

    let outputPointCount = 0;
    const reduceBar = createBar("Reduce Shards", shardedPointCount, "points");
    reduceBar.start(shardedPointCount, 0);
    let reducedPointsSeen = 0;

    for (const shardPath of shardPaths) {
      const cells = new Map();

      await readShardRecords(shardPath, (cellId, packedColor, count) => {
        let cell = cells.get(cellId);
        if (!cell) {
          cell = new CellAccumulator();
          cells.set(cellId, cell);
        }
        cell.add(packedColor);

        reducedPointsSeen += count;
        if (
          reducedPointsSeen === shardedPointCount ||
          reducedPointsSeen % PROGRESS_BATCH === 0
        ) {
          reduceBar.update(Math.min(reducedPointsSeen, shardedPointCount));
        }
      });

      for (const [cellId, cell] of cells) {
        const { x, y, z } = decodeCellCenter(cellId, scaler);
        const rgba = unpackColor(cell.bestColor);
        const maybeFlush = outputWriter.append(x, y, z, rgba);
        if (maybeFlush) await maybeFlush;
        outputPointCount++;
      }
    }

    reduceBar.update(shardedPointCount);
    reduceBar.stop();
    await outputWriter.close();

    const stagedDataSize = (await fsPromises.stat(outputDataPath)).size;
    const expectedDataSize = outputPointCount * OUTPUT_RECORD_BYTES;
    if (stagedDataSize !== expectedDataSize) {
      throw new Error(
        `Staged output size mismatch: expected ${expectedDataSize} bytes, got ${stagedDataSize}`,
      );
    }

    await writeFinalPly(outputDataPath, outputPath, outputPointCount);

    console.log(
      [
        `Quantized ${vertexCount} input points into ${outputPointCount} output points.`,
        `Target size: ${SIZE_X_IN}" x ${SIZE_Y_IN}" x ${SIZE_Z_IN}"`,
        `Target DPI: ${DPI_X} x ${DPI_Y} x ${DPI_Z}`,
      ].join("\n"),
    );
  } finally {
    await fsPromises.rm(tempDir, { recursive: true, force: true });
  }
};

const readPlyHeader = async (filePath) => {
  const handle = await fsPromises.open(filePath, "r");
  let headerBuffer = Buffer.alloc(0);
  try {
    for (;;) {
      const chunk = Buffer.allocUnsafe(64 * 1024);
      const { bytesRead } = await handle.read(chunk, 0, chunk.length, null);
      if (bytesRead === 0) {
        throw new Error("Invalid PLY: missing end_header.");
      }

      headerBuffer = Buffer.concat([
        headerBuffer,
        chunk.subarray(0, bytesRead),
      ]);
      const headerText = headerBuffer.toString("utf8");
      const match = headerText.match(/end_header(?:\r?\n|$)/);
      if (!match || match.index == null) continue;

      const byteLength = Buffer.byteLength(
        headerText.slice(0, match.index + match[0].length),
        "utf8",
      );
      return {
        header: parseHeaderText(
          headerText.slice(0, match.index + match[0].length),
        ),
        headerEndOffset: byteLength,
      };
    }
  } finally {
    await handle.close();
  }
};

const forEachVertex = async (filePath, headerInfo, onVertex, { label }) => {
  const total = headerInfo.header.vertex.count;
  const bar = createBar(label, total, "points");
  bar.start(total, 0);

  let processed = 0;
  let nextUpdate = PROGRESS_BATCH;

  const onParsedVertex = (vertex) => {
    const maybePromise = onVertex(vertex);
    processed++;
    if (processed >= nextUpdate || processed === total) {
      bar.update(processed);
      nextUpdate += PROGRESS_BATCH;
    }
    return maybePromise;
  };

  if (headerInfo.header.format.startsWith("ascii")) {
    await streamAsciiVertices(filePath, headerInfo, onParsedVertex);
  } else {
    await streamBinaryVertices(filePath, headerInfo, onParsedVertex);
  }

  bar.update(total);
  bar.stop();
  return processed;
};

const streamAsciiVertices = async (filePath, headerInfo, onVertex) => {
  const props = headerInfo.header.vertex.properties;
  const indices = getVertexFieldIndices(props);

  if (indices.x < 0 || indices.y < 0 || indices.z < 0) {
    throw new Error("PLY vertex must include x, y, and z.");
  }

  const stream = fs.createReadStream(filePath, {
    start: headerInfo.headerEndOffset,
    highWaterMark: STREAM_CHUNK_BYTES,
    encoding: "utf8",
  });

  let leftover = "";
  let processed = 0;
  const total = headerInfo.header.vertex.count;

  for await (const chunk of stream) {
    leftover += chunk;
    const lines = leftover.split(/\r?\n/);
    leftover = lines.pop() ?? "";

    for (const rawLine of lines) {
      if (processed >= total) return;
      const line = rawLine.trim();
      if (!line || line.startsWith("comment")) continue;

      const parts = line.split(/\s+/);
      const maybePromise = onVertex({
        x: Number(parts[indices.x]),
        y: Number(parts[indices.y]),
        z: Number(parts[indices.z]),
        color: normalizeColor(readColorFromParts(parts, indices)),
      });
      if (isPromiseLike(maybePromise)) await maybePromise;
      processed++;
    }
  }

  if (processed < total && leftover.trim()) {
    const parts = leftover.trim().split(/\s+/);
    const maybePromise = onVertex({
      x: Number(parts[indices.x]),
      y: Number(parts[indices.y]),
      z: Number(parts[indices.z]),
      color: normalizeColor(readColorFromParts(parts, indices)),
    });
    if (isPromiseLike(maybePromise)) await maybePromise;
    processed++;
  }

  if (processed !== total) {
    throw new Error(
      `PLY ASCII parse error: expected ${total} vertices, got ${processed}`,
    );
  }
};

const streamBinaryVertices = async (filePath, headerInfo, onVertex) => {
  const props = headerInfo.header.vertex.properties;
  const layout = buildBinaryLayout(props);

  if (!layout.x || !layout.y || !layout.z) {
    throw new Error("PLY vertex must include x, y, and z.");
  }

  const stream = fs.createReadStream(filePath, {
    start: headerInfo.headerEndOffset,
    highWaterMark: STREAM_CHUNK_BYTES,
  });

  let leftover = Buffer.alloc(0);
  let processed = 0;
  const total = headerInfo.header.vertex.count;

  for await (const chunk of stream) {
    const buffer = leftover.length ? Buffer.concat([leftover, chunk]) : chunk;
    const completeBytes = buffer.length - (buffer.length % layout.stride);
    const dv = new DataView(buffer.buffer, buffer.byteOffset, buffer.length);

    for (let offset = 0; offset < completeBytes && processed < total; ) {
      const x = layout.x.reader.read(dv, offset + layout.x.offset);
      const y = layout.y.reader.read(dv, offset + layout.y.offset);
      const z = layout.z.reader.read(dv, offset + layout.z.offset);

      const color = normalizeColor({
        r: layout.r
          ? normalizeColorValue(
              layout.r.reader.read(dv, offset + layout.r.offset),
            )
          : DEFAULT_COLOR.r,
        g: layout.g
          ? normalizeColorValue(
              layout.g.reader.read(dv, offset + layout.g.offset),
            )
          : DEFAULT_COLOR.g,
        b: layout.b
          ? normalizeColorValue(
              layout.b.reader.read(dv, offset + layout.b.offset),
            )
          : DEFAULT_COLOR.b,
        a: layout.a
          ? normalizeColorValue(
              layout.a.reader.read(dv, offset + layout.a.offset),
            )
          : DEFAULT_COLOR.a,
      });

      const maybePromise = onVertex({ x, y, z, color });
      if (isPromiseLike(maybePromise)) await maybePromise;
      processed++;
      offset += layout.stride;
    }

    leftover = buffer.subarray(completeBytes);
    if (processed >= total) break;
  }

  if (processed !== total) {
    throw new Error(
      `PLY binary parse error: expected ${total} vertices, got ${processed}`,
    );
  }
};

const buildBinaryLayout = (properties) => {
  const layout = {
    stride: 0,
    x: null,
    y: null,
    z: null,
    r: null,
    g: null,
    b: null,
    a: null,
  };

  let offset = 0;
  for (const property of properties) {
    const reader = pickReader(property.type);
    const entry = { offset, reader };
    switch (property.name) {
      case "x":
        layout.x = entry;
        break;
      case "y":
        layout.y = entry;
        break;
      case "z":
        layout.z = entry;
        break;
      case "red":
      case "r":
        layout.r = entry;
        break;
      case "green":
      case "g":
        layout.g = entry;
        break;
      case "blue":
      case "b":
        layout.b = entry;
        break;
      case "alpha":
      case "a":
        layout.a = entry;
        break;
    }
    offset += reader.size;
  }

  layout.stride = offset;
  return layout;
};

const buildScaler = (bounds, grid) => {
  const rangeX = bounds.maxX - bounds.minX;
  const rangeY = bounds.maxY - bounds.minY;
  const rangeZ = bounds.maxZ - bounds.minZ;

  const quantize = (value, min, range, steps) => {
    const ratio = range > 0 ? (value - min) / range : 0.5;
    const index = Math.floor(ratio * steps);
    if (index < 0) return 0;
    if (index >= steps) return steps - 1;
    return index;
  };

  return {
    gridX: grid.x,
    gridY: grid.y,
    gridZ: grid.z,
    gridXBig: BigInt(grid.x),
    gridYBig: BigInt(grid.y),
    quantizeX: (value) => quantize(value, bounds.minX, rangeX, grid.x),
    quantizeY: (value) => quantize(value, bounds.minY, rangeY, grid.y),
    quantizeZ: (value) => quantize(value, bounds.minZ, rangeZ, grid.z),
  };
};

const decodeCellCenter = (cellId, scaler) => {
  const xIndex = Number(cellId % scaler.gridXBig);
  const yz = cellId / scaler.gridXBig;
  const yIndex = Number(yz % scaler.gridYBig);
  const zIndex = Number(yz / scaler.gridYBig);

  return {
    x: ((xIndex + 0.5) / scaler.gridX) * SIZE_X_IN,
    y: ((yIndex + 0.5) / scaler.gridY) * SIZE_Y_IN,
    z: ((zIndex + 0.5) / scaler.gridZ) * SIZE_Z_IN,
  };
};

const packColor = (color) =>
  ((((color.a ?? 255) << 24) >>> 0) |
    ((color.b ?? 255) << 16) |
    ((color.g ?? 255) << 8) |
    (color.r ?? 255)) >>>
  0;

const unpackColor = (packedColor) => ({
  r: packedColor & 0xff,
  g: (packedColor >>> 8) & 0xff,
  b: (packedColor >>> 16) & 0xff,
  a: (packedColor >>> 24) & 0xff,
});

const normalizeColor = (color) => ({
  r: color?.r ?? DEFAULT_COLOR.r,
  g: color?.g ?? DEFAULT_COLOR.g,
  b: color?.b ?? DEFAULT_COLOR.b,
  a: color?.a ?? DEFAULT_COLOR.a,
});

const createBar = (label, total, unit) =>
  new SingleBar(
    {
      format: `${label} |{bar}| {percentage}% | {value}/{total} ${unit} | ETA {eta_formatted}`,
      hideCursor: true,
    },
    Presets.shades_classic,
  );

const readShardRecords = async (shardPath, onRecord) => {
  const stat = await fsPromises.stat(shardPath);
  if (stat.size === 0) return;

  const stream = fs.createReadStream(shardPath, {
    highWaterMark: STREAM_CHUNK_BYTES,
  });

  let leftover = Buffer.alloc(0);

  for await (const chunk of stream) {
    const buffer = leftover.length ? Buffer.concat([leftover, chunk]) : chunk;
    const completeBytes = buffer.length - (buffer.length % SHARD_RECORD_BYTES);

    for (let offset = 0; offset < completeBytes; offset += SHARD_RECORD_BYTES) {
      const cellId = buffer.readBigUInt64LE(offset);
      const packedColor = buffer.readUInt32LE(offset + 8);
      onRecord(cellId, packedColor, 1);
    }

    leftover = buffer.subarray(completeBytes);
  }

  if (leftover.length !== 0) {
    throw new Error(`Corrupt shard file: ${shardPath}`);
  }
};

const writeEmptyPly = async (outputPath) => {
  const header =
    "ply\n" +
    "format binary_little_endian 1.0\n" +
    "element vertex 0\n" +
    "property float x\n" +
    "property float y\n" +
    "property float z\n" +
    "property uchar red\n" +
    "property uchar green\n" +
    "property uchar blue\n" +
    "property uchar alpha\n" +
    "end_header\n";
  await fsPromises.writeFile(outputPath, header, "ascii");
};

const writeFinalPly = async (vertexDataPath, outputPath, pointCount) => {
  const header =
    "ply\n" +
    "format binary_little_endian 1.0\n" +
    `element vertex ${pointCount}\n` +
    "property float x\n" +
    "property float y\n" +
    "property float z\n" +
    "property uchar red\n" +
    "property uchar green\n" +
    "property uchar blue\n" +
    "property uchar alpha\n" +
    "end_header\n";

  if (pointCount === 0) {
    await fsPromises.writeFile(outputPath, header, "ascii");
    return;
  }

  await fsPromises.writeFile(outputPath, header, "ascii");

  const writeBar = createBar("Write Output", pointCount, "points");
  writeBar.start(pointCount, 0);

  let written = 0;

  await new Promise((resolve, reject) => {
    const input = fs.createReadStream(vertexDataPath, {
      highWaterMark: STREAM_CHUNK_BYTES,
    });
    const output = fs.createWriteStream(outputPath, { flags: "a" });

    const fail = (error) => reject(error);
    input.on("error", fail);
    output.on("error", fail);
    output.on("finish", resolve);

    input.on("data", (chunk) => {
      written += Math.floor(chunk.length / OUTPUT_RECORD_BYTES);
      if (written === pointCount || written % PROGRESS_BATCH === 0) {
        writeBar.update(Math.min(written, pointCount));
      }
    });

    input.pipe(output);
  });

  writeBar.update(pointCount);
  writeBar.stop();
};

const isPromiseLike = (value) =>
  value != null && typeof value.then === "function";

class BufferedShardWriter {
  static create = async (filePath, bufferBytes) => {
    const handle = await fsPromises.open(filePath, "w");
    return new BufferedShardWriter(handle, bufferBytes);
  };

  constructor(handle, bufferBytes) {
    this.handle = handle;
    this.buffer = Buffer.allocUnsafe(
      Math.max(
        SHARD_RECORD_BYTES,
        Math.floor(bufferBytes / SHARD_RECORD_BYTES) * SHARD_RECORD_BYTES,
      ),
    );
    this.offset = 0;
  }

  append = (cellId, packedColor) => {
    if (this.offset + SHARD_RECORD_BYTES > this.buffer.length) {
      return this.#flushAndAppend(cellId, packedColor);
    }

    this.buffer.writeBigUInt64LE(cellId, this.offset);
    this.buffer.writeUInt32LE(packedColor >>> 0, this.offset + 8);
    this.offset += SHARD_RECORD_BYTES;
    return null;
  };

  close = async () => {
    await this.#flush();
    await this.handle.close();
  };

  #flushAndAppend = async (cellId, packedColor) => {
    await this.#flush();
    this.buffer.writeBigUInt64LE(cellId, this.offset);
    this.buffer.writeUInt32LE(packedColor >>> 0, this.offset + 8);
    this.offset += SHARD_RECORD_BYTES;
  };

  #flush = async () => {
    if (this.offset === 0) return;
    await this.handle.write(this.buffer.subarray(0, this.offset));
    this.offset = 0;
  };
}

class BufferedOutputWriter {
  static create = async (filePath, bufferBytes) => {
    const handle = await fsPromises.open(filePath, "w");
    return new BufferedOutputWriter(handle, bufferBytes);
  };

  constructor(handle, bufferBytes) {
    this.handle = handle;
    this.buffer = Buffer.allocUnsafe(
      Math.max(
        OUTPUT_RECORD_BYTES,
        Math.floor(bufferBytes / OUTPUT_RECORD_BYTES) * OUTPUT_RECORD_BYTES,
      ),
    );
    this.offset = 0;
  }

  append = (x, y, z, color) => {
    if (this.offset + OUTPUT_RECORD_BYTES > this.buffer.length) {
      return this.#flushAndAppend(x, y, z, color);
    }

    this.#writeRecord(x, y, z, color);
    return null;
  };

  close = async () => {
    await this.#flush();
    await this.handle.close();
  };

  #flushAndAppend = async (x, y, z, color) => {
    await this.#flush();
    this.#writeRecord(x, y, z, color);
  };

  #writeRecord = (x, y, z, color) => {
    this.buffer.writeFloatLE(x, this.offset);
    this.buffer.writeFloatLE(y, this.offset + 4);
    this.buffer.writeFloatLE(z, this.offset + 8);
    this.buffer.writeUInt8(color.r ?? 255, this.offset + 12);
    this.buffer.writeUInt8(color.g ?? 255, this.offset + 13);
    this.buffer.writeUInt8(color.b ?? 255, this.offset + 14);
    this.buffer.writeUInt8(color.a ?? 255, this.offset + 15);
    this.offset += OUTPUT_RECORD_BYTES;
  };

  #flush = async () => {
    if (this.offset === 0) return;
    await this.handle.write(this.buffer.subarray(0, this.offset));
    this.offset = 0;
  };
}

class CellAccumulator {
  constructor() {
    this.counts = new Map();
    this.bestColor = 0;
    this.bestCount = 0;
  }

  add = (packedColor) => {
    const nextCount = (this.counts.get(packedColor) ?? 0) + 1;
    this.counts.set(packedColor, nextCount);

    if (
      nextCount > this.bestCount ||
      (nextCount === this.bestCount && packedColor < this.bestColor)
    ) {
      this.bestCount = nextCount;
      this.bestColor = packedColor;
    }
  };
}

await run();
