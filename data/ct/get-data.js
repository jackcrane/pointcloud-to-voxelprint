import { readNRRD } from "./nrrd-utils.js";

const { shape } = readNRRD("drone-data/two.nrrd");
console.log(shape);
