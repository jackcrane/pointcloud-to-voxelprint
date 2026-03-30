import path from "path";
import { PNG } from "../classes/png.js";

const SEPARATOR_WIDTH = 10;
const SEPARATOR_COLOR = [255, 0, 0, 255];

const printUsageAndExit = () => {
  console.error(
    "Usage: node util/generate-stepped-gradient.js <start-rgba> <end-rgba> <stops> <width> <height> <output.png>",
  );
  console.error(
    "Example: node util/generate-stepped-gradient.js 0,0,0,255 255,255,255,255 5 1200 200 out.png",
  );
  process.exit(1);
};

const parseChannel = (value, label) => {
  const parsed = Number(value);
  if (!Number.isInteger(parsed) || parsed < 0 || parsed > 255) {
    throw new Error(`${label} channel must be an integer in [0, 255].`);
  }
  return parsed;
};

const parseColor = (raw, label) => {
  const parts = raw.split(",");
  if (parts.length !== 4) {
    throw new Error(`${label} must be four comma-separated integers: r,g,b,a`);
  }

  return parts.map((part, index) =>
    parseChannel(part.trim(), `${label}[${index}]`),
  );
};

const parsePositiveInteger = (raw, label) => {
  const parsed = Number(raw);
  if (!Number.isInteger(parsed) || parsed <= 0) {
    throw new Error(`${label} must be a positive integer.`);
  }
  return parsed;
};

const interpolateColor = (start, end, t) =>
  start.map((channel, index) =>
    Math.round(channel + (end[index] - channel) * t),
  );

const fillRect = (image, xStart, rectWidth, color) => {
  for (let y = 0; y < image.height; y++) {
    for (let x = xStart; x < xStart + rectWidth; x++) {
      image.setPixel(x, y, color[0], color[1], color[2], color[3]);
    }
  }
};

const main = async () => {
  const [startRaw, endRaw, stopsRaw, widthRaw, heightRaw, outputPath] =
    process.argv.slice(2);

  if (!startRaw || !endRaw || !stopsRaw || !widthRaw || !heightRaw || !outputPath) {
    printUsageAndExit();
  }

  const start = parseColor(startRaw, "start-rgba");
  const end = parseColor(endRaw, "end-rgba");
  const stops = parsePositiveInteger(stopsRaw, "stops");
  const width = parsePositiveInteger(widthRaw, "width");
  const height = parsePositiveInteger(heightRaw, "height");

  const separatorCount = Math.max(0, stops - 1);
  const separatorPixels = separatorCount * SEPARATOR_WIDTH;
  const fillPixels = width - separatorPixels;
  if (fillPixels < stops) {
    throw new Error(
      `width=${width} is too small for ${stops} stops with ${SEPARATOR_WIDTH}px separators.`,
    );
  }

  const image = new PNG(width, height);
  const baseSegmentWidth = Math.floor(fillPixels / stops);
  let remainder = fillPixels % stops;
  let cursor = 0;

  for (let stopIndex = 0; stopIndex < stops; stopIndex++) {
    const t = stops === 1 ? 0 : stopIndex / (stops - 1);
    const segmentColor = interpolateColor(start, end, t);
    const segmentWidth = baseSegmentWidth + (remainder > 0 ? 1 : 0);

    fillRect(image, cursor, segmentWidth, segmentColor);
    cursor += segmentWidth;
    if (remainder > 0) remainder--;

    if (stopIndex < stops - 1) {
      fillRect(image, cursor, SEPARATOR_WIDTH, SEPARATOR_COLOR);
      cursor += SEPARATOR_WIDTH;
    }
  }

  await image.flush(outputPath);
  console.log(
    `Wrote ${path.resolve(outputPath)} (${width}x${height}, ${stops} stops, ${SEPARATOR_WIDTH}px separators)`,
  );
};

await main();
