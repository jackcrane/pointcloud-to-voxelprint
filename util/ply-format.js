import fs from "fs";
import { promises as fsPromises } from "fs";

export const DEFAULT_PLY_STREAM_CHUNK_BYTES = 1024 * 1024;

export const parseHeaderFromBuffer = (buf) => {
  const txt = buf.toString("utf8");
  const endIdx = txt.indexOf("end_header");
  if (endIdx < 0) throw new Error("Invalid PLY: missing end_header.");

  const after = txt.indexOf("\n", endIdx);
  const headerEndOffset = Buffer.from(
    txt.slice(0, after < 0 ? endIdx + "end_header".length : after + 1),
    "utf8",
  ).length;

  const headerText = txt.slice(0, after < 0 ? endIdx : after);
  const headerLines = headerText.split(/\r?\n/);

  return {
    header: parseHeaderLines(headerLines),
    headerEndOffset,
    headerText,
  };
};

export const parseHeaderText = (headerText) => {
  const headerLines = headerText.trimEnd().split(/\r?\n/);
  return parseHeaderLines(headerLines);
};

export const readPlyHeader = async (filePath) => {
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

      const fullHeaderText = headerText.slice(0, match.index + match[0].length);
      return {
        header: parseHeaderText(fullHeaderText),
        headerEndOffset: Buffer.byteLength(fullHeaderText, "utf8"),
      };
    }
  } finally {
    await handle.close();
  }
};

export const getVertexFieldIndices = (props) => {
  const colorIndex = (primary, fallback) =>
    props.findIndex((prop) => prop.name === primary || prop.name === fallback);
  const colorType = (index) => (index >= 0 ? props[index].type : null);

  const r = colorIndex("red", "r");
  const g = colorIndex("green", "g");
  const b = colorIndex("blue", "b");
  const a = colorIndex("alpha", "a");

  return {
    x: props.findIndex((prop) => prop.name === "x"),
    y: props.findIndex((prop) => prop.name === "y"),
    z: props.findIndex((prop) => prop.name === "z"),
    r,
    g,
    b,
    a,
    rType: colorType(r),
    gType: colorType(g),
    bType: colorType(b),
    aType: colorType(a),
  };
};

export const pickReader = (type) => {
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
      return { size: 4, read: (dv, off) => dv.getFloat32(off, true) };
  }
};

export const buildBinaryLayout = (properties) => {
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
    const entry = { offset, reader, type: property.type };
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

export const readColorFromParts = (parts, cidx) => {
  const haveRGB =
    cidx.r >= 0 &&
    cidx.g >= 0 &&
    cidx.b >= 0 &&
    parts[cidx.r] != null &&
    parts[cidx.g] != null &&
    parts[cidx.b] != null;

  if (!haveRGB) return null;

  const r = normalizeColorValue(parts[cidx.r], cidx.rType);
  const g = normalizeColorValue(parts[cidx.g], cidx.gType);
  const b = normalizeColorValue(parts[cidx.b], cidx.bType);
  const a = cidx.a >= 0 && parts[cidx.a] != null
    ? normalizeColorValue(parts[cidx.a], cidx.aType)
    : null;

  return { r, g, b, a };
};

export const normalizeColorValue = (value, type = null) => {
  const n = Number(value);
  if (!Number.isFinite(n)) return null;

  if (type != null) {
    const kind = classifyPlyType(type);

    if (kind === "normalized_float") {
      if (n <= 1 && n >= 0) return Math.round(n * 255);
      return Math.max(0, Math.min(255, Math.round(n)));
    }

    if (kind?.unsignedMax != null) {
      const clamped = Math.max(0, Math.min(kind.unsignedMax, n));
      if (kind.unsignedMax <= 255) return Math.round(clamped);
      return Math.round((clamped * 255) / kind.unsignedMax);
    }
  }

  if (n <= 1 && n >= 0) return Math.round(n * 255);
  return Math.max(0, Math.min(255, Math.round(n)));
};

export const forEachPlyVertex = async (
  filePath,
  headerInfo,
  onVertex,
  { chunkBytes = DEFAULT_PLY_STREAM_CHUNK_BYTES } = {},
) => {
  if (headerInfo.header.format.startsWith("ascii")) {
    return streamAsciiVertices(filePath, headerInfo, onVertex, chunkBytes);
  }

  if (headerInfo.header.format.startsWith("binary_little_endian")) {
    return streamBinaryVertices(filePath, headerInfo, onVertex, chunkBytes);
  }

  throw new Error(`Unsupported PLY format: ${headerInfo.header.format}`);
};

const classifyPlyType = (type) => {
  switch (String(type).toLowerCase()) {
    case "uchar":
    case "uint8":
      return { unsignedMax: 0xff };
    case "ushort":
    case "uint16":
      return { unsignedMax: 0xffff };
    case "uint":
    case "uint32":
      return { unsignedMax: 0xffffffff };
    case "float":
    case "float32":
    case "double":
    case "float64":
      return "normalized_float";
    default:
      return null;
  }
};

const streamAsciiVertices = async (
  filePath,
  headerInfo,
  onVertex,
  chunkBytes,
) => {
  const props = headerInfo.header.vertex?.properties ?? [];
  const indices = getVertexFieldIndices(props);

  if (indices.x < 0 || indices.y < 0 || indices.z < 0) {
    throw new Error("PLY vertex must include x, y, and z.");
  }

  const stream = fs.createReadStream(filePath, {
    start: headerInfo.headerEndOffset,
    highWaterMark: chunkBytes,
    encoding: "utf8",
  });

  let leftover = "";
  let processed = 0;
  const total = headerInfo.header.vertex?.count ?? 0;

  for await (const chunk of stream) {
    leftover += chunk;
    const lines = leftover.split(/\r?\n/);
    leftover = lines.pop() ?? "";

    for (const rawLine of lines) {
      if (processed >= total) return processed;
      const line = rawLine.trim();
      if (!line || line.startsWith("comment")) continue;

      const parts = line.split(/\s+/);
      const maybePromise = onVertex({
        x: Number(parts[indices.x]),
        y: Number(parts[indices.y]),
        z: Number(parts[indices.z]),
        color: readColorFromParts(parts, indices),
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
      color: readColorFromParts(parts, indices),
    });
    if (isPromiseLike(maybePromise)) await maybePromise;
    processed++;
  }

  if (processed !== total) {
    throw new Error(
      `PLY ASCII parse error: expected ${total} vertices, got ${processed}`,
    );
  }

  return processed;
};

const streamBinaryVertices = async (
  filePath,
  headerInfo,
  onVertex,
  chunkBytes,
) => {
  const props = headerInfo.header.vertex?.properties ?? [];
  const layout = buildBinaryLayout(props);

  if (!layout.x || !layout.y || !layout.z) {
    throw new Error("PLY vertex must include x, y, and z.");
  }

  const stream = fs.createReadStream(filePath, {
    start: headerInfo.headerEndOffset,
    highWaterMark: chunkBytes,
  });

  let leftover = Buffer.alloc(0);
  let processed = 0;
  const total = headerInfo.header.vertex?.count ?? 0;

  for await (const chunk of stream) {
    const buffer = leftover.length ? Buffer.concat([leftover, chunk]) : chunk;
    const completeBytes = buffer.length - (buffer.length % layout.stride);
    const dv = new DataView(buffer.buffer, buffer.byteOffset, buffer.length);

    for (let offset = 0; offset < completeBytes && processed < total; ) {
      const maybePromise = onVertex({
        x: layout.x.reader.read(dv, offset + layout.x.offset),
        y: layout.y.reader.read(dv, offset + layout.y.offset),
        z: layout.z.reader.read(dv, offset + layout.z.offset),
        color: readColorFromBinaryLayout(layout, dv, offset),
      });
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

  return processed;
};

const readColorFromBinaryLayout = (layout, dv, offset) => {
  if (!layout.r || !layout.g || !layout.b) return null;

  return {
    r: normalizeColorValue(
      layout.r.reader.read(dv, offset + layout.r.offset),
      layout.r.type,
    ),
    g: normalizeColorValue(
      layout.g.reader.read(dv, offset + layout.g.offset),
      layout.g.type,
    ),
    b: normalizeColorValue(
      layout.b.reader.read(dv, offset + layout.b.offset),
      layout.b.type,
    ),
    a: layout.a
      ? normalizeColorValue(
          layout.a.reader.read(dv, offset + layout.a.offset),
          layout.a.type,
        )
      : null,
  };
};

const isPromiseLike = (value) =>
  value != null && typeof value.then === "function";

const parseHeaderLines = (headerLines) => {
  if (!/^ply\s*$/i.test(headerLines[0])) throw new Error("Not a PLY file.");

  const formatLine = headerLines.find((line) => line.startsWith("format "));
  if (!formatLine) throw new Error("Missing format line.");
  const format = formatLine.split(/\s+/).slice(1, 3).join(" ");

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
          const parts = headerLines[i].trim().split(/\s+/).slice(1);
          if (parts[0] === "list") {
            i++;
            continue;
          }
          const [type, propName] = parts;
          properties.push({ name: propName, type });
          i++;
        }
        header.vertex = { count, properties };
        continue;
      }
    }
    i++;
  }

  return header;
};
