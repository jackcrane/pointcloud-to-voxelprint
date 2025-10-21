/** Simple 3D point with optional RGBA color (0–255) */

export class Point3D {
  /** @param {number} x @param {number} y @param {number} z @param {{r?:number,g?:number,b?:number,a?:number}|null} color */
  constructor(x, y, z, color = null) {
    this.x = x;
    this.y = y;
    this.z = z;
    if (color) {
      this.r = color.r ?? null;
      this.g = color.g ?? null;
      this.b = color.b ?? null;
      this.a = color.a ?? null;
    }
  }
  /** Euclidean distance to another Point3D (xyz) */
  distanceFrom = (other) => {
    const dx = this.x - other.x;
    const dy = this.y - other.y;
    const dz = this.z - other.z;
    return Math.hypot(dx, dy, dz);
  };
}
