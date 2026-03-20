import fs from "fs";
import zlib from "zlib";

/**
 * @param {Uint8Array} gzipped
 * @returns {Uint8Array}
 */
export const gunzipNRRDData = (gzipped) =>
  new Uint8Array(zlib.gunzipSync(gzipped));

/**
 * Reads a .nrrd file and returns { header, data, shape }
 */
export const readNRRD = (path) => {
  const buffer = fs.readFileSync(path);

  // NRRD header ends with a blank line
  const headerEnd = buffer.indexOf("\n\n");
  if (headerEnd === -1) {
    throw new Error("Invalid NRRD: header not found");
  }

  const headerText = buffer.slice(0, headerEnd).toString("ascii");
  const dataBuffer = buffer.slice(headerEnd + 2);

  const header = {};
  for (const line of headerText.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const [key, value] = line.split(":").map((s) => s.trim());
    header[key] = value;
  }

  const sizes = header.sizes.split(" ").map(Number);
  const type = header.type.toLowerCase();

  const TypedArray = {
    uchar: Uint8Array,
    uint8: Uint8Array,
    short: Int16Array,
    int16: Int16Array,
    ushort: Uint16Array,
    uint16: Uint16Array,
    int: Int32Array,
    int32: Int32Array,
    float: Float32Array,
    double: Float64Array,
  }[type];

  if (!TypedArray) {
    throw new Error(`Unsupported NRRD type: ${type}`);
  }

  const data = new TypedArray(
    dataBuffer.buffer,
    dataBuffer.byteOffset,
    dataBuffer.byteLength / TypedArray.BYTES_PER_ELEMENT,
  );

  return {
    header,
    data,
    shape: sizes,
  };
};

/**
 * @param {Uint8Array} voxels
 * @param {[number, number, number]} shape [x, y, z]
 * @param {number} threshold
 */
export const volumeToPointCloud = (voxels, [nx, ny, nz], threshold = 1) => {
  const points = [];

  let i = 0;
  for (let z = 0; z < nz; z++) {
    for (let y = 0; y < ny; y++) {
      for (let x = 0; x < nx; x++, i++) {
        const v = voxels[i];
        if (v < threshold) continue;

        // Simple grayscale → RGB
        const r = v;
        const g = v;
        const b = v;

        points.push({
          x,
          y,
          z,
          r,
          g,
          b,
        });
      }
    }
  }

  return points;
};
