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
