/* =============================================================================
   sha256.js — 纯 JS SHA-256 (作为 crypto.subtle 不可用时的回退)
   用法: window.sha256hex(Uint8Array) -> 64 位十六进制字符串
         window.hashFile(File) -> Promise<hex>  (安全上下文优先用 WebCrypto)
   实现参考公共领域算法 (FIPS 180-4)。

   注意: 经 http://<局域网 IP>:8888 访问时页面**不是**安全上下文, WebCrypto 不可用,
   因此本文件的纯 JS 实现就是实际生效路径 —— 其正确性由
   tests/e2e/sha256_contract_validate.mjs 对拍 Node 权威实现 (NIST 向量 + 填充边界)
   永久守护; 秒传/去重依赖哈希完全一致, 任何偏差都会导致内容寻址错乱。
   · 浏览器: window.sha256hex / window.hashFile / window.CV.Sha256
   · Node  : module.exports = { sha256hex, hashFile }
   ============================================================================= */
(function (global) {
  'use strict';
  const K = new Uint32Array([
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
  ]);

  function toHex(u32arr) {
    let s = '';
    for (let i = 0; i < u32arr.length; i++) {
      s += ('00000000' + (u32arr[i] >>> 0).toString(16)).slice(-8);
    }
    return s;
  }

  function sha256hex(bytes) {
    if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
    const H = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
    const l = bytes.length;
    const bitLen = l * 8;
    // padding
    const withOne = l + 1;
    const total = withOne + ((56 - (withOne % 64) + 64) % 64) + 8;
    const msg = new Uint8Array(total);
    msg.set(bytes);
    msg[l] = 0x80;
    // 64-bit length (big-endian); 高 32 位存位长的高位 (JS 位运算 32 位, 用浮点拆分)
    const hi = Math.floor(bitLen / 0x100000000);
    const lo = bitLen >>> 0;
    const dv = new DataView(msg.buffer);
    dv.setUint32(total - 8, hi);
    dv.setUint32(total - 4, lo);

    const w = new Uint32Array(64);
    for (let off = 0; off < total; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const a = w[i-15], b = w[i-2];
        const s0 = ((a>>>7)|(a<<25)) ^ ((a>>>18)|(a<<14)) ^ (a>>>3);
        const s1 = ((b>>>17)|(b<<15)) ^ ((b>>>19)|(b<<13)) ^ (b>>>10);
        w[i] = (w[i-16] + s0 + w[i-7] + s1) >>> 0;
      }
      let a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
      for (let i = 0; i < 64; i++) {
        const S1 = ((e>>>6)|(e<<26)) ^ ((e>>>11)|(e<<21)) ^ ((e>>>25)|(e<<7));
        const ch = (e & f) ^ (~e & g);
        const t1 = (h + S1 + ch + K[i] + w[i]) >>> 0;
        const S0 = ((a>>>2)|(a<<30)) ^ ((a>>>13)|(a<<19)) ^ ((a>>>22)|(a<<10));
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) >>> 0;
        h=g; g=f; f=e; e=(d+t1)>>>0; d=c; c=b; b=a; a=(t1+t2)>>>0;
      }
      H[0]=(H[0]+a)>>>0; H[1]=(H[1]+b)>>>0; H[2]=(H[2]+c)>>>0; H[3]=(H[3]+d)>>>0;
      H[4]=(H[4]+e)>>>0; H[5]=(H[5]+f)>>>0; H[6]=(H[6]+g)>>>0; H[7]=(H[7]+h)>>>0;
    }
    return toHex(H);
  }

  // 统一入口: 优先用原生 WebCrypto (localhost 是安全上下文), 否则回退纯 JS。
  async function hashFile(file) {
    const buf = await file.arrayBuffer();
    if (global.crypto && global.crypto.subtle && global.isSecureContext) {
      try {
        const digest = await global.crypto.subtle.digest('SHA-256', buf);
        const b = new Uint8Array(digest);
        let hex = '';
        for (let i = 0; i < b.length; i++) hex += ('0' + b[i].toString(16)).slice(-2);
        return hex;
      } catch (e) { /* 回退 */ }
    }
    return sha256hex(new Uint8Array(buf));
  }

  // 浏览器: 保持既有的裸全局 (app.js 使用 window.hashFile), 同时挂到 CV 下便于统一访问
  // Node   : module.exports, 供契约测试对拍 (tests/e2e/sha256_contract_validate.mjs)
  const Sha256 = { sha256hex, hashFile };
  if (global) {
    global.sha256hex = sha256hex;
    global.hashFile = hashFile;
    global.CV = global.CV || {};
    global.CV.Sha256 = Sha256;
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = Sha256;
})(typeof window !== 'undefined' ? window : null);
