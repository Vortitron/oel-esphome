/*
 * gif.js - a small GIF decoder for the builder's 1-bit preview: every frame
 * of an animated GIF, whole, as 0xRRGGBB per pixel with its delay. A canvas
 * can't be trusted to give the frame showing (the spec says drawImage takes
 * the first), so the preview plays the GIF's frames itself.
 *
 *   decodeGif(arrayBuffer) -> { width, height, frames: [{ rgb: Int32Array, delay: ms }] }
 */
(function (root) {
  function decodeGif(buf) {
    var b = new Uint8Array(buf), at = 0;
    function u16() { at += 2; return b[at - 2] | b[at - 1] << 8; }
    function table(n) {
      var t = new Int32Array(n);
      for (var i = 0; i < n; i++, at += 3) t[i] = b[at] << 16 | b[at + 1] << 8 | b[at + 2];
      return t;
    }
    function blocks() {
      var parts = [], len = 0;
      for (var n = b[at++]; n; n = b[at++]) { parts.push(b.subarray(at, at + n)); len += n; at += n; }
      var out = new Uint8Array(len), o = 0;
      parts.forEach(function (p) { out.set(p, o); o += p.length; });
      return out;
    }
    if (String.fromCharCode(b[0], b[1], b[2]) !== 'GIF') throw new Error('not a GIF');
    at = 6;
    var width = u16(), height = u16(), packed = b[at++];
    at += 2;  // background, aspect
    var global = packed & 0x80 ? table(2 << (packed & 7)) : null;
    var screen = new Int32Array(width * height), saved = null, frames = [];
    var delay = 100, transparent = -1, disposal = 0;
    while (at < b.length) {
      var kind = b[at++];
      if (kind === 0x3B) break;
      if (kind === 0x21) {
        var label = b[at++];
        if (label === 0xF9) {
          var g = blocks();
          disposal = g[0] >> 2 & 7;
          transparent = g[0] & 1 ? g[3] : -1;
          delay = (g[1] | g[2] << 8) * 10 || 100;
        } else {
          blocks();
        }
        continue;
      }
      if (kind !== 0x2C) throw new Error('bad GIF block ' + kind);
      var x0 = u16(), y0 = u16(), w = u16(), h = u16(), p = b[at++];
      var colours = p & 0x80 ? table(2 << (p & 7)) : global;
      var interlaced = p & 0x40;
      var pixels = lzw(b[at++], blocks(), w * h);
      if (disposal === 3) saved = screen.slice();
      var rows = [];
      if (interlaced) {
        [[0, 8], [4, 8], [2, 4], [1, 2]].forEach(function (pass) {
          for (var y = pass[0]; y < h; y += pass[1]) rows.push(y);
        });
      } else {
        for (var y = 0; y < h; y++) rows.push(y);
      }
      for (var r = 0; r < h; r++) {
        var sy = y0 + rows[r];
        if (sy >= height) continue;
        for (var x = 0; x < w; x++) {
          var k = pixels[r * w + x];
          if (k === transparent || x0 + x >= width) continue;
          screen[sy * width + x0 + x] = colours[k];
        }
      }
      frames.push({ rgb: screen.slice(), delay: delay });
      if (disposal === 2) {
        for (var yy = y0; yy < Math.min(height, y0 + h); yy++) screen.fill(0, yy * width + x0, yy * width + Math.min(width, x0 + w));
      } else if (disposal === 3 && saved) {
        screen = saved;
      }
      delay = 100;
      transparent = -1;
      disposal = 0;
    }
    return { width: width, height: height, frames: frames };
  }

  function lzw(minSize, data, count) {
    var out = new Uint8Array(count), clear = 1 << minSize, end = clear + 1;
    var prefix = new Int16Array(4096), suffix = new Uint8Array(4096), first = new Uint8Array(4096);
    var stack = new Uint8Array(4097);
    var size = minSize + 1, next = end + 1, prev = -1, o = 0, bits = 0, acc = 0;
    for (var i = 0; i < clear; i++) { suffix[i] = first[i] = i; prefix[i] = -1; }
    for (var at = 0; at < data.length && o < count;) {
      while (bits < size && at < data.length) { acc |= data[at++] << bits; bits += 8; }
      if (bits < size) break;
      var code = acc & (1 << size) - 1;
      acc >>>= size;
      bits -= size;
      if (code === clear) { size = minSize + 1; next = end + 1; prev = -1; continue; }
      if (code === end) break;
      var c = code, sp = 0;
      if (prev < 0) {
        out[o++] = suffix[code];
        prev = code;
        continue;
      }
      if (code >= next) { stack[sp++] = first[prev]; c = prev; }
      while (c >= clear) { stack[sp++] = suffix[c]; c = prefix[c]; }
      stack[sp++] = c;
      while (sp && o < count) out[o++] = stack[--sp];
      if (next < 4096) {
        prefix[next] = prev;
        suffix[next] = c;
        first[next] = first[prev];
        next++;
        if (next === 1 << size && size < 12) size++;
      }
      prev = code;
    }
    return out;
  }

  root.decodeGif = decodeGif;
  if (typeof module !== 'undefined') module.exports = decodeGif;
})(typeof window !== 'undefined' ? window : this);
