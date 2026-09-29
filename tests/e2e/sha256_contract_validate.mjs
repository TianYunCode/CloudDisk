/* =============================================================================
   sha256_contract_validate.mjs —— SHA-256 三方一致性契约

   内容寻址 (秒传 / 去重 / 引用计数删除) 依赖"同一份字节在任何一端算出的哈希
   完全相同"。涉及三个独立实现:
     · web/js/core/sha256.js 的纯 JS sha256hex —— **经 http://<局域网IP>:8888
       访问时页面不是安全上下文, WebCrypto 不可用, 这条纯 JS 路径就是实际生效路径**
     · Node crypto (权威参照, 等价于 openssl)
     · server/util/CryptoUtil.cpp 的 generate_hashcode() —— 后端入库/比对所用

   任一端偏差都会导致秒传命中错误内容、去重失效或引用计数错乱, 且症状隐蔽。
   本测试对同一批字节做三方逐值对拍, 并覆盖 SHA-256 的经典易错点:
   55/56/63/64/65/119/120/127/128 字节等填充边界。

   C++ 侧需要链接 jwt/ssl/crypto; 若环境不具备则跳过三方对拍,
   仍执行 JS <-> Node 权威对拍 (这是局域网访问时的真实路径)。
   ============================================================================= */
import { writeFileSync, mkdtempSync, rmSync, existsSync } from 'fs';
import { execFileSync } from 'child_process';
import { createHash } from 'crypto';
import { join, dirname } from 'path';
import { tmpdir } from 'os';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const require_ = createRequire(import.meta.url);

let fail = 0, total = 0;
function check(cond, msg, extra) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (extra ? '\n      ' + extra : '')); }
}

const Sha256 = require_(join(ROOT, 'web/js/core/sha256.js'));
const jsSha = Sha256.sha256hex;
check(typeof jsSha === 'function', '前端 sha256hex 可加载 (Node 双导出)');

// 确定性字节生成式: 两端必须一致 (C++ 探针内用同一公式)
function pattern(n) {
  const b = Buffer.alloc(n);
  for (let i = 0; i < n; i++) b[i] = (i * 7 + 13) & 0xff;
  return b;
}
const nodeSha = buf => createHash('sha256').update(buf).digest('hex');

// ---- 1. NIST / 公开已知答案向量 ----
const KAT = [
  ['', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
  ['abc', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'],
  ['abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
    '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1'],
];
const katBad = [];
for (const [input, expected] of KAT) {
  const got = jsSha(new Uint8Array(Buffer.from(input, 'utf8')));
  if (got !== expected) katBad.push(JSON.stringify(input.slice(0, 20)) + ': ' + got);
}
check(katBad.length === 0, '前端实现通过 ' + KAT.length + ' 组 NIST 已知答案向量', katBad.join('\n      '));

// ---- 2. 填充边界与多块长度: JS <-> Node 权威 ----
const LENGTHS = [0, 1, 2, 3, 55, 56, 57, 63, 64, 65, 119, 120, 121,
  127, 128, 129, 255, 256, 257, 1000, 4096, 65536];
const jsBad = [];
for (const n of LENGTHS) {
  const buf = pattern(n);
  const got = jsSha(new Uint8Array(buf));
  const exp = nodeSha(buf);
  if (got !== exp) jsBad.push('len=' + n + ': JS=' + got.slice(0, 16) + '… exp=' + exp.slice(0, 16) + '…');
}
check(jsBad.length === 0,
  '前端与 Node 权威实现对全部 ' + LENGTHS.length + ' 种长度一致 (含 55/56/63/64/65/119/120/127/128 填充边界)',
  jsBad.slice(0, 5).join('\n      '));

// ---- 2b. 增量式实现: 任意分片切法都必须与一次性 / Node 权威完全一致 ----
// 大文件走的是 createSha256() 流式路径 (hashFile 按 4MiB 分片喂入), 因此
// "分片边界处理" 与 "填充边界" 同等关键: 残留字节缓存 tailLen 在 55/56/63/64
// 处的行为一旦出错, 大文件哈希就会与后端不一致 -> 秒传/去重命中错误内容。
const createSha256 = Sha256.createSha256;
check(typeof createSha256 === 'function', '前端 createSha256 (增量式) 可加载');

const incBad = [];
// 每种长度都用多种切法喂入: 逐字节 / 3 字节 / 整块 64 / 65 (跨块) / 大分片
const SPLITS = [1, 3, 7, 64, 65, 128, 4096];
for (const n of LENGTHS) {
  const buf = pattern(n);
  const exp = nodeSha(buf);
  const oneShot = jsSha(new Uint8Array(buf));
  if (oneShot !== exp) { incBad.push('len=' + n + ' 一次性实现不符'); continue; }
  for (const step of SPLITS) {
    const h = createSha256();
    for (let off = 0; off < n; off += step) {
      h.update(new Uint8Array(buf.subarray(off, Math.min(n, off + step))));
    }
    const got = h.hex();
    if (got !== exp) {
      incBad.push('len=' + n + ' step=' + step + ': 增量=' + got.slice(0, 12) + '… exp=' + exp.slice(0, 12) + '…');
    }
  }
}
check(incBad.length === 0,
  '增量式 createSha256 对 ' + LENGTHS.length + ' 种长度 × ' + SPLITS.length + ' 种分片切法全部与 Node 权威一致 ('
    + (LENGTHS.length * SPLITS.length) + ' 组)',
  incBad.slice(0, 6).join('\n      '));

// 空 update 不得影响结果 (hashFile 在 size 恰为分片整数倍时会多走一次空循环)
{
  const h = createSha256();
  h.update(new Uint8Array(0));
  h.update(new Uint8Array(pattern(100)));
  h.update(new Uint8Array(0));
  check(h.hex() === nodeSha(pattern(100)), '空 update 不影响增量结果', h.hex().slice(0, 16));
}
// 单块残留恰好落在填充边界 (tailLen = 55/56/63) 的切法
{
  const bb = [];
  for (const n of [55, 56, 63, 64, 119, 120]) {
    const buf = pattern(n);
    // 先喂 n-1 字节, 再喂最后 1 字节 -> 强制 tailLen 停在 54/55/62/63/118/119
    const h = createSha256();
    h.update(new Uint8Array(buf.subarray(0, n - 1)));
    h.update(new Uint8Array(buf.subarray(n - 1)));
    if (h.hex() !== nodeSha(buf)) bb.push('n=' + n);
  }
  check(bb.length === 0, '增量式在 tailLen 处于 54/55/62/63 等填充临界点时仍正确', bb.join(','));
}
// 带非零 byteOffset 的子视图 (DataView 若漏加 byteOffset 会静默算错)
{
  const big = new Uint8Array(200);
  big.set(new Uint8Array(pattern(120)), 50);           // 数据放在 offset 50 处
  const view = big.subarray(50, 170);
  const h = createSha256();
  h.update(view);
  check(h.hex() === nodeSha(pattern(120)),
    '增量式正确处理带 byteOffset 的子视图', h.hex().slice(0, 16));
}

// 输出形态自检
const sample = jsSha(new Uint8Array(pattern(10)));
check(/^[0-9a-f]{64}$/.test(sample), '输出为 64 位小写十六进制', sample);

// ---- 3. 三方对拍: 编译 C++ 探针 (CryptoUtil::generate_hashcode) ----
const dir = mkdtempSync(join(tmpdir(), 'sha256-'));
let probe = null;
try {
  const src = join(dir, 'probe.cpp');
  writeFileSync(src, [
    '#include <cstdio>', '#include <cstdlib>', '#include <string>', '#include <vector>',
    '#include "CryptoUtil.h"',
    'int main(int argc, char** argv) {',
    '  for (int i = 1; i < argc; ++i) {',
    '    size_t n = (size_t)std::atoll(argv[i]);',
    '    std::vector<char> buf(n);',
    '    for (size_t j = 0; j < n; ++j) buf[j] = (char)((j * 7 + 13) & 0xff);',
    '    std::printf("%s\\n", CryptoUtil::generate_hashcode(buf.data(), buf.size()).c_str());',
    '  }', '  return 0; }',
  ].join('\n'));
  const bin = join(dir, 'probe');
  const inc = ['server/util', 'server/common', 'server/config', 'server/rpc', 'third_party']
    .flatMap(d => ['-I', join(ROOT, d)]);
  execFileSync('g++', ['-std=c++17', '-O0', ...inc, '-I', '/usr/local/include',
    src, join(ROOT, 'server/util/CryptoUtil.cpp'), '-o', bin,
    '-L/usr/local/lib', '-ljwt', '-lssl', '-lcrypto'], { stdio: 'pipe' });
  if (existsSync(bin)) probe = bin;
} catch (e) {
  console.log('  ! 无法编译 C++ 探针 (缺少 jwt/ssl/crypto?), 跳过三方对拍: ' +
    (e.message || e).toString().split('\n')[0]);
}

if (probe) {
  // 65536 字节经 argv 只传长度, 无二进制问题
  const lens = LENGTHS.map(String);
  const out = execFileSync(probe, lens, {
    encoding: 'utf8',
    env: { ...process.env, LD_LIBRARY_PATH: '/usr/local/lib:' + (process.env.LD_LIBRARY_PATH || '') },
  });
  const cpp = out.split('\n').filter(l => l.length > 0);
  check(cpp.length === LENGTHS.length, 'C++ 探针输出行数一致', cpp.length + ' vs ' + LENGTHS.length);
  const triBad = [];
  for (let i = 0; i < LENGTHS.length; i++) {
    const buf = pattern(LENGTHS[i]);
    const a = cpp[i], b = nodeSha(buf), c = jsSha(new Uint8Array(buf));
    if (!(a === b && b === c)) {
      triBad.push('len=' + LENGTHS[i] + ': C++=' + a.slice(0, 12) + ' Node=' + b.slice(0, 12) + ' JS=' + c.slice(0, 12));
    }
  }
  check(triBad.length === 0,
    '三方 (C++ CryptoUtil / Node 权威 / 前端纯 JS) 对全部 ' + LENGTHS.length + ' 种长度输出完全一致',
    triBad.slice(0, 5).join('\n      '));
}
try { rmSync(dir, { recursive: true, force: true }); } catch (e) { /* 忽略 */ }

console.log('\n覆盖长度: ' + LENGTHS.length + ' 种 (0 至 64KiB, 含全部 SHA-256 填充边界)');
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')' + (probe ? '' : ' [三方对拍已跳过]'));
process.exit(fail === 0 ? 0 : 1);
