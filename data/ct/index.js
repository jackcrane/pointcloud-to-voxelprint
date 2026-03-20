import { readNRRD } from "./nrrd-utils.js";
import { nrrdToPLY } from "./nrrdToPLY.js";

const nrrd = readNRRD("drone-data/two.nrrd");

nrrdToPLY({
  gzipped: nrrd.data,
  shape: nrrd.shape,
  threshold: 15,
  stride: 2,
  outPath: "ct_cloud.ply",
});
