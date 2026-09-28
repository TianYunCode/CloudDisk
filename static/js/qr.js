/* =============================================================================
   qr.js — 自包含 QR 码生成器 (字节模式, 纠错级别 M, 版本 1-10)
   无任何外部依赖 / CDN。导出 window.QR (浏览器) 与 module.exports (Node 测试)。
   算法遵循 ISO/IEC 18004。
   ============================================================================= */
(function (root) {
  'use strict';

  // ---- GF(256) 伽罗华域 (本原多项式 0x11d) ----
  const EXP = new Array(512), LOG = new Array(256);
  (function () {
    let x = 1;
    for (let i = 0; i < 255; i++) { EXP[i] = x; LOG[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
    for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
  })();
  const gmul = (a, b) => (a === 0 || b === 0) ? 0 : EXP[LOG[a] + LOG[b]];

  // Reed-Solomon 生成多项式
  function rsGenPoly(deg) {
    let poly = [1];
    for (let i = 0; i < deg; i++) {
      const np = new Array(poly.length + 1).fill(0);
      for (let j = 0; j < poly.length; j++) {
        np[j] ^= gmul(poly[j], EXP[i]);
        np[j + 1] ^= poly[j];
      }
      poly = np;
    }
    return poly;
  }
  function rsEncode(data, ecLen) {
    const gen = rsGenPoly(ecLen);   // 长度 ecLen+1, gen[0]=1 为首项
    const res = new Array(ecLen).fill(0);
    for (let i = 0; i < data.length; i++) {
      const factor = data[i] ^ res[0];
      res.shift(); res.push(0);
      for (let j = 0; j < ecLen; j++) res[j] ^= gmul(gen[ecLen - 1 - j], factor);
    }
    return res;
  }

  // ---- 纠错级别 M 的分块参数 (版本 1-10) ----
  // [ecPerBlock, g1blocks, g1data, g2blocks, g2data]
  const ECM = {
    1:[10,1,16,0,0], 2:[16,1,28,0,0], 3:[26,1,44,0,0], 4:[18,2,32,0,0], 5:[24,2,43,0,0],
    6:[16,4,27,0,0], 7:[18,4,31,0,0], 8:[22,2,38,2,39], 9:[22,3,36,2,37], 10:[26,4,43,1,44],
  };
  const dataCodewords = (v) => { const e = ECM[v]; return e[1]*e[2] + e[3]*e[4]; };

  // 对齐图案中心坐标
  const ALIGN = {
    1:[], 2:[6,18], 3:[6,22], 4:[6,26], 5:[6,30], 6:[6,34],
    7:[6,22,38], 8:[6,24,42], 9:[6,26,46], 10:[6,28,50],
  };

  function chooseVersion(len) {
    for (let v = 1; v <= 10; v++) {
      const dc = dataCodewords(v);
      const countBits = v >= 10 ? 16 : 8;
      const need = 4 + countBits + len * 8;              // 位
      const cap = dc * 8;
      if (need + 4 <= cap || need <= cap) return v;      // 终止符可省略
    }
    throw new Error('内容过长, 超出 QR 版本 10 容量');
  }

  // 字节模式编码 -> 数据码字 (含填充)
  function encodeData(bytes, version) {
    const dc = dataCodewords(version);
    const countBits = version >= 10 ? 16 : 8;
    const bits = [];
    const push = (val, n) => { for (let i = n - 1; i >= 0; i--) bits.push((val >> i) & 1); };
    push(0b0100, 4);                 // 字节模式
    push(bytes.length, countBits);   // 字符计数
    for (const b of bytes) push(b, 8);
    // 终止符
    const cap = dc * 8;
    for (let i = 0; i < 4 && bits.length < cap; i++) bits.push(0);
    // 补足到字节边界
    while (bits.length % 8 !== 0) bits.push(0);
    // 填充字节
    const words = [];
    for (let i = 0; i < bits.length; i += 8) { let b = 0; for (let j = 0; j < 8; j++) b = (b << 1) | bits[i + j]; words.push(b); }
    const pad = [0xec, 0x11]; let pi = 0;
    while (words.length < dc) { words.push(pad[pi & 1]); pi++; }
    return words;
  }

  // 交织数据 + 纠错码字
  function interleave(words, version) {
    const [ecLen, g1b, g1d, g2b, g2d] = ECM[version];
    const blocks = [];
    let idx = 0;
    for (let i = 0; i < g1b; i++) { const d = words.slice(idx, idx + g1d); idx += g1d; blocks.push({ d, e: rsEncode(d, ecLen) }); }
    for (let i = 0; i < g2b; i++) { const d = words.slice(idx, idx + g2d); idx += g2d; blocks.push({ d, e: rsEncode(d, ecLen) }); }
    const maxD = Math.max(g1d, g2d || 0);
    const out = [];
    for (let i = 0; i < maxD; i++) for (const b of blocks) if (i < b.d.length) out.push(b.d[i]);
    for (let i = 0; i < ecLen; i++) for (const b of blocks) out.push(b.e[i]);
    return out;
  }

  // ---- 矩阵构建 ----
  function buildMatrix(version) {
    const size = 17 + version * 4;
    const m = Array.from({ length: size }, () => new Array(size).fill(null));  // null=未定, 0/1
    const rsv = Array.from({ length: size }, () => new Array(size).fill(false)); // 功能区
    const set = (r, c, v) => { m[r][c] = v; rsv[r][c] = true; };

    // 定位图案 (finder) + 分隔
    function finder(r, c) {
      for (let i = -1; i <= 7; i++) for (let j = -1; j <= 7; j++) {
        const rr = r + i, cc = c + j;
        if (rr < 0 || cc < 0 || rr >= size || cc >= size) continue;
        const inRing = (i >= 0 && i <= 6 && (j === 0 || j === 6)) || (j >= 0 && j <= 6 && (i === 0 || i === 6));
        const inCore = i >= 2 && i <= 4 && j >= 2 && j <= 4;
        set(rr, cc, inRing || inCore ? 1 : 0);
      }
    }
    finder(0, 0); finder(0, size - 7); finder(size - 7, 0);

    // 定时图案
    for (let i = 8; i < size - 8; i++) { const v = i % 2 === 0 ? 1 : 0; set(6, i, v); set(i, 6, v); }

    // 对齐图案 (排除与三个定位图案重叠的角: (6,6)(6,last)(last,6))
    const centers = ALIGN[version];
    const last = centers.length ? centers[centers.length - 1] : 0;
    for (const r of centers) for (const c of centers) {
      if ((r === 6 && c === 6) || (r === 6 && c === last) || (r === last && c === 6)) continue;
      for (let i = -2; i <= 2; i++) for (let j = -2; j <= 2; j++) {
        const on = Math.max(Math.abs(i), Math.abs(j)) !== 1;
        set(r + i, c + j, on ? 1 : 0);
      }
    }

    // 暗模块
    set(size - 8, 8, 1);

    // 预留格式信息区 (置 0, 稍后填)
    for (let i = 0; i < 9; i++) { if (!rsv[8][i]) set(8, i, 0); if (!rsv[i][8]) set(i, 8, 0); }
    for (let i = 0; i < 8; i++) { if (!rsv[8][size - 1 - i]) set(8, size - 1 - i, 0); if (!rsv[size - 1 - i][8]) set(size - 1 - i, 8, 0); }

    // 版本信息区 (v>=7)
    if (version >= 7) {
      for (let i = 0; i < 6; i++) for (let j = 0; j < 3; j++) { set(i, size - 11 + j, 0); set(size - 11 + j, i, 0); }
    }
    return { m, rsv, size };
  }

  // 数据放置 (锯齿)
  function placeData(state, codewords) {
    const { m, rsv, size } = state;
    const bits = [];
    for (const w of codewords) for (let i = 7; i >= 0; i--) bits.push((w >> i) & 1);
    let bi = 0, up = true;
    for (let col = size - 1; col > 0; col -= 2) {
      if (col === 6) col--; // 跳过定时列
      for (let k = 0; k < size; k++) {
        const row = up ? size - 1 - k : k;
        for (let c = 0; c < 2; c++) {
          const cc = col - c;
          if (rsv[row][cc]) continue;
          m[row][cc] = bi < bits.length ? bits[bi] : 0;
          bi++;
        }
      }
      up = !up;
    }
  }

  const MASKS = [
    (r, c) => (r + c) % 2 === 0,
    (r, c) => r % 2 === 0,
    (r, c) => c % 3 === 0,
    (r, c) => (r + c) % 3 === 0,
    (r, c) => (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0,
    (r, c) => ((r * c) % 2 + (r * c) % 3) === 0,
    (r, c) => (((r * c) % 2 + (r * c) % 3) % 2) === 0,
    (r, c) => (((r + c) % 2 + (r * c) % 3) % 2) === 0,
  ];

  function applyMask(state, maskIdx) {
    const { m, rsv, size } = state;
    const out = m.map(row => row.slice());
    const fn = MASKS[maskIdx];
    for (let r = 0; r < size; r++) for (let c = 0; c < size; c++)
      if (!rsv[r][c] && fn(r, c)) out[r][c] ^= 1;
    return out;
  }

  // BCH 格式信息 (级别 M=0b00)
  function formatBits(maskIdx) {
    const data = (0b00 << 3) | maskIdx;      // 5 位
    let d = data << 10;
    let g = 0b10100110111;
    for (let i = 14; i >= 10; i--) if ((d >> i) & 1) d ^= g << (i - 10);
    let bits = ((data << 10) | d) ^ 0b101010000010010;
    return bits & 0x7fff;
  }
  function placeFormat(mtx, size, maskIdx) {
    const f = formatBits(maskIdx);
    const bit = (i) => (f >> (14 - i)) & 1;   // 位置 0 = 最高位 (符合 ISO 18004)
    // 竖直 (左上) + 水平 (左上)
    for (let i = 0; i <= 5; i++) mtx[8][i] = bit(i);
    mtx[8][7] = bit(6); mtx[8][8] = bit(7); mtx[7][8] = bit(8);
    for (let i = 9; i <= 14; i++) mtx[14 - i][8] = bit(i);
    // 右上 (水平) + 左下 (竖直)
    for (let i = 0; i <= 7; i++) mtx[8][size - 1 - i] = bit(i);
    for (let i = 8; i <= 14; i++) mtx[size - 15 + i][8] = bit(i);
    mtx[size - 8][8] = 1; // 暗模块保持
  }

  // BCH 版本信息 (v>=7)
  function versionBits(version) {
    let d = version << 12;
    const g = 0b1111100100101;
    for (let i = 17; i >= 12; i--) if ((d >> i) & 1) d ^= g << (i - 12);
    return (version << 12) | d;
  }
  function placeVersion(mtx, size, version) {
    if (version < 7) return;
    const vb = versionBits(version);
    for (let i = 0; i < 18; i++) {
      const bit = (vb >> i) & 1;
      const r = Math.floor(i / 3), c = i % 3;
      mtx[r][size - 11 + c] = bit;
      mtx[size - 11 + c][r] = bit;
    }
  }

  // 掩码惩罚评分
  function penalty(mtx, size) {
    let p = 0;
    // 规则1: 连续同色
    for (let r = 0; r < size; r++) {
      let run = 1;
      for (let c = 1; c < size; c++) { if (mtx[r][c] === mtx[r][c - 1]) { run++; if (run === 5) p += 3; else if (run > 5) p++; } else run = 1; }
    }
    for (let c = 0; c < size; c++) {
      let run = 1;
      for (let r = 1; r < size; r++) { if (mtx[r][c] === mtx[r - 1][c]) { run++; if (run === 5) p += 3; else if (run > 5) p++; } else run = 1; }
    }
    // 规则2: 2x2 同色块
    for (let r = 0; r < size - 1; r++) for (let c = 0; c < size - 1; c++) {
      const v = mtx[r][c];
      if (v === mtx[r][c + 1] && v === mtx[r + 1][c] && v === mtx[r + 1][c + 1]) p += 3;
    }
    // 规则3: finder-like 图案
    const pat1 = [1,0,1,1,1,0,1,0,0,0,0], pat2 = [0,0,0,0,1,0,1,1,1,0,1];
    const check = (arr) => { for (let i = 0; i + 11 <= arr.length; i++) { let m1 = true, m2 = true; for (let k = 0; k < 11; k++) { if (arr[i + k] !== pat1[k]) m1 = false; if (arr[i + k] !== pat2[k]) m2 = false; } if (m1 || m2) p += 40; } };
    for (let r = 0; r < size; r++) check(mtx[r]);
    for (let c = 0; c < size; c++) { const col = []; for (let r = 0; r < size; r++) col.push(mtx[r][c]); check(col); }
    // 规则4: 深色比例
    let dark = 0; for (let r = 0; r < size; r++) for (let c = 0; c < size; c++) dark += mtx[r][c];
    const ratio = dark / (size * size) * 100;
    p += Math.floor(Math.abs(ratio - 50) / 5) * 10;
    return p;
  }

  function generate(text, forceMask) {
    const bytes = typeof text === 'string'
      ? Array.from(new TextEncoder().encode(text))
      : text;
    const version = chooseVersion(bytes.length);
    const words = encodeData(bytes, version);
    const codewords = interleave(words, version);
    const state = buildMatrix(version);
    placeData(state, codewords);
    // 选择最佳掩码
    let best = null, bestScore = Infinity, bestMask = 0;
    for (let mk = 0; mk < 8; mk++) {
      if (forceMask != null && mk !== forceMask) continue;
      const masked = applyMask(state, mk);
      placeFormat(masked, state.size, mk);
      placeVersion(masked, state.size, version);
      const sc = penalty(masked, state.size);
      if (sc < bestScore) { bestScore = sc; best = masked; bestMask = mk; }
    }
    return { size: state.size, modules: best, version, mask: bestMask };
  }

  // 渲染为 SVG 字符串
  function toSVG(text, opts) {
    opts = opts || {};
    const q = generate(text);
    const margin = opts.margin != null ? opts.margin : 4;
    const scale = opts.scale || 4;
    const dim = (q.size + margin * 2) * scale;
    const dark = opts.dark || '#111827';
    const light = opts.light || '#ffffff';
    let path = '';
    for (let r = 0; r < q.size; r++) for (let c = 0; c < q.size; c++)
      if (q.modules[r][c]) path += 'M' + ((c + margin) * scale) + ' ' + ((r + margin) * scale) + 'h' + scale + 'v' + scale + 'h-' + scale + 'z';
    return '<svg xmlns="http://www.w3.org/2000/svg" width="' + dim + '" height="' + dim + '" viewBox="0 0 ' + dim + ' ' + dim + '" shape-rendering="crispEdges">' +
      '<rect width="' + dim + '" height="' + dim + '" fill="' + light + '"/>' +
      '<path d="' + path + '" fill="' + dark + '"/></svg>';
  }

  const QR = { generate, toSVG };
  if (typeof module !== 'undefined' && module.exports) module.exports = QR;
  if (root) root.QR = QR;
})(typeof window !== 'undefined' ? window : null);
