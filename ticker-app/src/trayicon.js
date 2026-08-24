'use strict';

const zlib = require('zlib');

// Electron's nativeImage understands PNG and JPEG - NOT SVG. Passing an SVG
// data URL yields an empty image and a blank tray slot, so build a real PNG.

function crc32(buf) {
  if (typeof zlib.crc32 === 'function') return zlib.crc32(buf) >>> 0;
  let c, crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    c = (crc ^ buf[i]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([len, body, crc]);
}

function encodePng(width, height, rgba) {
  const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;    // bit depth
  ihdr[9] = 6;    // colour type: RGBA
  ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;

  // Each scanline is prefixed with filter type 0 (none).
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;
    rgba.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }

  return Buffer.concat([
    sig,
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

function hexToRgb(hex) {
  const h = hex.replace('#', '');
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}

/**
 * A 16x16 tray glyph: a lit LED panel. Two rows of "pixels" inside a frame,
 * which reads as a ticker rather than as a generic dot.
 */
function panelIcon(hex) {
  const W = 16, H = 16;
  const [r, g, b] = hexToRgb(hex);
  const px = Buffer.alloc(W * H * 4);        // transparent by default

  const set = (x, y, alpha) => {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const o = (y * W + x) * 4;
    px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = alpha;
  };

  // Frame around the panel body (x 1..14, y 3..12), corners left out.
  for (let x = 2; x <= 13; x++) { set(x, 3, 210); set(x, 12, 210); }
  for (let y = 4; y <= 11; y++) { set(1, y, 210); set(14, y, 210); }

  // Two rows of LED dots, brighter than the frame.
  for (let x = 3; x <= 12; x += 2) {
    set(x, 6, 255);
    set(x, 9, 255);
  }
  return encodePng(W, H, px);
}

module.exports = { panelIcon, encodePng };
