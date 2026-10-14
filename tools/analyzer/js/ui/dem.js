/*
 * Terrain heights along the track for the 3D map: the Terrarium tiles the terrain layer draws
 * (the browser cache serves them once), decoded here and interpolated bilinearly between the pixel
 * centres. Zoom 13 like the terrain layer's finest level; a long track takes a coarser level so that
 * at most MAX_TILES tiles load.
 */
const TERRAIN = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";
const MAX_ZOOM = 13;
const MIN_ZOOM = 9;
const MAX_TILES = 64;
const SIZE = 256;

function worldPixel(lon, lat, z) {
  const n = SIZE * 2 ** z;
  const s = Math.sin((lat * Math.PI) / 180);
  return [((lon + 180) / 360) * n, (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n];
}

async function loadTile(z, x, y) {
  const url = TERRAIN.replace("{z}", z).replace("{x}", x).replace("{y}", y);
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  const bmp = await createImageBitmap(await res.blob());
  const canvas = new OffscreenCanvas(SIZE, SIZE);
  const g = canvas.getContext("2d");
  g.drawImage(bmp, 0, 0);
  const px = g.getImageData(0, 0, SIZE, SIZE).data;
  const h = new Float32Array(SIZE * SIZE);
  for (let i = 0; i < h.length; i++) h[i] = px[4 * i] * 256 + px[4 * i + 1] + px[4 * i + 2] / 256 - 32768;
  return h;
}

export class Dem {
  constructor() {
    this.z = MAX_ZOOM;
    this.tiles = new Map();
    this.complete = false;
  }

  /* the tiles a bilinear lookup at these points needs, at zoom z */
  static tilesFor(points, z) {
    const keys = new Set();
    for (const [lon, lat] of points) {
      const [px, py] = worldPixel(lon, lat, z);
      for (const ox of [-0.5, 0.5]) {
        for (const oy of [-0.5, 0.5]) keys.add(`${Math.floor((px + ox) / SIZE)}/${Math.floor((py + oy) / SIZE)}`);
      }
    }
    return keys;
  }

  /* loads the tiles under the points ([lon, lat]); true when every one arrived */
  async load(points) {
    let z = MAX_ZOOM;
    let keys = Dem.tilesFor(points, z);
    while (keys.size > MAX_TILES && z > MIN_ZOOM) keys = Dem.tilesFor(points, --z);
    this.z = z;
    const list = [...keys];
    let failed = 0;
    for (let i = 0; i < list.length; i += 8) { /* a few at a time */
      await Promise.all(list.slice(i, i + 8).map(async (key) => {
        const [x, y] = key.split("/").map(Number);
        try {
          this.tiles.set(key, await loadTile(z, x, y));
        } catch (e) {
          failed++;
        }
      }));
    }
    this.complete = failed === 0;
    return this.complete;
  }

  pixel(gx, gy) {
    const tile = this.tiles.get(`${Math.floor(gx / SIZE)}/${Math.floor(gy / SIZE)}`);
    if (!tile) return NaN;
    const x = ((gx % SIZE) + SIZE) % SIZE, y = ((gy % SIZE) + SIZE) % SIZE;
    return tile[y * SIZE + x];
  }

  /* terrain height at a point in metres, NaN where no tile is loaded */
  height(lon, lat) {
    const [px, py] = worldPixel(lon, lat, this.z);
    const fx = px - 0.5, fy = py - 0.5; /* pixel centres */
    const x0 = Math.floor(fx), y0 = Math.floor(fy);
    const tx = fx - x0, ty = fy - y0;
    const h00 = this.pixel(x0, y0), h10 = this.pixel(x0 + 1, y0), h01 = this.pixel(x0, y0 + 1), h11 = this.pixel(x0 + 1, y0 + 1);
    return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
  }
}
