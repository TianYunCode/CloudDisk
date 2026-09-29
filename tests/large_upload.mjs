// 大文件上传链路验证 (需活的服务: bin/server + UserService + MySQL)
//
// 守住的是一组"大文件不能把内存吃穿"的不变量。历史缺陷:
//   1) config.json 的 max_file_size 曾为 100 MiB, 覆盖掉 Config.h 的 10 GiB 默认值,
//      导致用户上传 121GB 文件夹时凡超过 100 MiB 的文件全部被 413 拒绝;
//   2) /api/upload/complete 把**所有分片拼进一个 std::string** 再整体哈希,
//      峰值内存 = 文件大小 —— 上限提到 10 GiB 后一次合并就要 10 GiB 内存,
//      足以 OOM 拖垮整个服务进程 (影响所有用户);
//   3) init / instant 不校验 size, 客户端会把注定失败的文件的所有分片传完才被拒。
//
// 本测试默认用 128 MiB 真实数据 (约 4 秒)。旧实现会为此分配 128 MiB 内存,
// 新实现峰值增量应稳定在数十 MiB 以内, 因此能有效区分两者。
// 可用 BIG_SIZE_MB 调整; 设为 1024 可复现 1 GiB 级别的验证。
import { createHash, randomBytes } from 'crypto';
import { openSync, readSync, writeSync, closeSync, statSync, unlinkSync } from 'fs';
import { tmpdir } from 'os';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { readFileSync } from 'fs';
import { execSync } from 'child_process';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const SIZE_MB = Number(process.env.BIG_SIZE_MB || 128);
const SIZE = SIZE_MB * 1024 * 1024;
const CHUNK = 4 * 1024 * 1024;                 // 与前端 app.js 的 CHUNK_SIZE 一致
const TMP = join(tmpdir(), 'clouddisk-big-' + Date.now() + '.bin');

let pass = 0, fail = 0, skip = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };
const skipped = (m, e) => { skip++; console.log('  \u2013 ' + m + ' (跳过' + (e ? ': ' + e : '') + ')'); };
const check = (c, m, e) => { if (c) ok(m); else bad(m, e); };

// ---- 工具 -------------------------------------------------------------------
const j = (r) => r.json();
const post = (p, body, tok) => fetch(BASE + p, {
  method: 'POST',
  headers: tok ? { 'Authorization': 'Bearer ' + tok, 'Content-Type': 'application/json' }
               : { 'Content-Type': 'application/json' },
  body: JSON.stringify(body),
}).then(j);

// 服务器 RSS (kB); 读不到时返回 null (例如 CI 中进程不可见) —— 相应断言降级为跳过
let PID = null;
try { PID = execSync('pgrep -x server', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim().split('\n')[0]; } catch { }
function rss() {
  if (!PID) return null;
  try {
    const s = readFileSync('/proc/' + PID + '/status', 'utf8');
    const m = /VmRSS:\s+(\d+)/.exec(s);
    return m ? Number(m[1]) : null;
  } catch { return null; }
}

// ---- 准备: 造大文件 + 权威哈希 ----------------------------------------------
console.log('大文件上传验证: %d MiB, %d 个 %d MiB 分片', SIZE_MB, Math.ceil(SIZE / CHUNK), CHUNK / 1048576);
const block = randomBytes(CHUNK);              // 随机内容: 不可压缩, 也不会秒传命中
{
  const fd = openSync(TMP, 'w');
  for (let off = 0; off < SIZE; off += CHUNK) writeSync(fd, block, 0, Math.min(CHUNK, SIZE - off));
  closeSync(fd);
}
const HASH = (() => {
  const h = createHash('sha256');
  const fd = openSync(TMP, 'r'); const b = Buffer.alloc(CHUNK); let n;
  while ((n = readSync(fd, b, 0, CHUNK, null)) > 0) h.update(b.subarray(0, n));
  closeSync(fd);
  return h.digest('hex');
})();

try {
  // ---- 1. 上限必须来自服务端且可读 ------------------------------------------
  const cfg = JSON.parse(readFileSync(join(ROOT, 'config.json'), 'utf8'));
  check(typeof cfg.max_file_size === 'number' && cfg.max_file_size >= 10 * 1024 * 1024 * 1024,
    'config.json max_file_size >= 10 GiB (不再被 100 MiB 的旧值卡死)',
    '实际 ' + cfg.max_file_size);
  check(typeof cfg.user_quota === 'number' && cfg.user_quota >= 1024 * 1024 * 1024 * 1024,
    'config.json user_quota >= 1 TiB', '实际 ' + cfg.user_quota);

  // Config.h 的默认值必须与 config.json 同量级: 二者任一处被改小都会让大文件失败,
  // 而 config.json 会覆盖 Config.h, 所以两边都要守。
  const hdr = readFileSync(join(ROOT, 'server/config/Config.h'), 'utf8');
  check(/max_file_size\(\)[^\n]*10LL\s*\*\s*1024\s*\*\s*1024\s*\*\s*1024/.test(hdr),
    'Config.h max_file_size 默认值仍为 10 GiB');
  check(/user_quota\(\)[^\n]*1LL\s*\*\s*1024\s*\*\s*1024\s*\*\s*1024\s*\*\s*1024/.test(hdr),
    'Config.h user_quota 默认值仍为 1 TiB');

  // ---- 2. 注册并取令牌 ------------------------------------------------------
  const user = 'bigup_' + Date.now();
  await post('/api/auth/register', { username: user, password: 'secret123' });
  const lg = await post('/api/auth/login', { username: user, password: 'secret123' });
  const TOK = lg && lg.data && lg.data.token;
  check(!!TOK, '测试用户注册并登录成功', JSON.stringify(lg).slice(0, 120));
  if (!TOK) throw new Error('无法取得令牌, 后续断言无法进行');

  // ---- 3. 上限必须下发给前端 (前端据此在哈希之前预检) ------------------------
  const info = await (await fetch(BASE + '/api/user/info', { headers: { 'Authorization': 'Bearer ' + TOK } })).json();
  const advertised = info && info.data && info.data.maxFileSize;
  check(advertised === cfg.max_file_size,
    '/api/user/info 下发的 maxFileSize 与 config.json 完全一致 (前端预检的依据)',
    'API=' + advertised + ' config=' + cfg.max_file_size);

  // ---- 4. 超限必须在 init / instant 就被拒, 且文案可读 -----------------------
  const over = cfg.max_file_size + 1024 * 1024 * 1024;      // 上限 + 1 GiB
  const fakeHash = 'a'.repeat(64);
  const rInit = await post('/api/upload/init',
    { filename: 'over.bin', hash: fakeHash, size: over, parentId: 0, chunkSize: CHUNK, totalChunks: Math.ceil(over / CHUNK) }, TOK);
  check(rInit.code === 413, '/api/upload/init 对超限尺寸返回 413 (早拒, 不必传完分片)', 'code=' + rInit.code);
  check(/上限.*\d+(\.\d+)?\s*(B|KB|MB|GB|TB)/.test(rInit.message || ''),
    '413 文案为人类可读字节 (含单位), 不是裸数字', rInit.message);

  const rInst = await post('/api/file/instant',
    { filename: 'over.bin', hash: 'b'.repeat(64), size: over, parentId: 0 }, TOK);
  check(rInst.code === 413, '/api/file/instant 对超限尺寸返回 413', 'code=' + rInst.code);

  // ---- 5. 真实大文件走完整分片链路 ------------------------------------------
  const total = Math.ceil(SIZE / CHUNK);
  const base = rss();
  const init = await post('/api/upload/init',
    { filename: 'big-' + Date.now() + '.bin', hash: HASH, size: SIZE, parentId: 0, chunkSize: CHUNK, totalChunks: total }, TOK);
  check(init.code === 0 && !!(init.data && init.data.uploadId),
    'init 接受 %d MiB 的合法文件'.replace('%d', String(SIZE_MB)), JSON.stringify(init).slice(0, 140));
  const upId = init.data && init.data.uploadId;
  if (!upId) throw new Error('init 未返回 uploadId');

  const fd = openSync(TMP, 'r');
  const buf = Buffer.alloc(CHUNK);
  let peak = base === null ? null : base;
  for (let i = 0; i < total; i++) {
    const n = readSync(fd, buf, 0, Math.min(CHUNK, SIZE - i * CHUNK), i * CHUNK);
    const r = await fetch(BASE + '/api/upload/chunk?uploadId=' + upId + '&index=' + i, {
      method: 'POST',
      headers: { 'Authorization': 'Bearer ' + TOK, 'Content-Type': 'application/octet-stream' },
      body: buf.subarray(0, n),
    });
    if (!r.ok) { closeSync(fd); throw new Error('分片 ' + i + ' 上传失败 HTTP ' + r.status); }
    if (i % 8 === 0 && peak !== null) { const c = rss(); if (c !== null && c > peak) peak = c; }
  }
  closeSync(fd);
  ok(total + ' 个分片全部上传成功');

  // ---- 6. 合并阶段的内存必须是流式的 ----------------------------------------
  // 基线取在 complete 之前一刻, 只测合并本身: 若把分片上传阶段的分配也算进来,
  // 阈值就得放宽到失去判别力 (实测那样会让 128 MiB 样本的增量逼近阈值)。
  const mBase = rss();
  let mPeak = mBase;
  const samples = [];
  const sampler = (mBase !== null) ? setInterval(() => { const c = rss(); if (c !== null) { samples.push(c); if (c > mPeak) mPeak = c; } }, 40) : null;
  const cmp = await post('/api/upload/complete', { uploadId: upId }, TOK);
  if (sampler) clearInterval(sampler);
  { const c = rss(); if (c !== null && mPeak !== null && c > mPeak) mPeak = c; }
  check(cmp.code === 0, '合并 (complete) 成功', JSON.stringify(cmp).slice(0, 160));
  check(cmp.code === 0 && Number(cmp.data && cmp.data.size) === SIZE,
    '合并后字节数与上传一致', '返回 ' + (cmp.data && cmp.data.size));
  check(cmp.code === 0 && cmp.data && cmp.data.hash === HASH,
    '服务端增量哈希与 Node 权威 sha256 一致 (流式实现未算错)', String(cmp.data && cmp.data.hash).slice(0, 16));

  if (mBase === null || mPeak === null) {
    skipped('合并期内存峰值检查', '无法读取 server 进程 RSS');
  } else {
    const delta = mPeak - mBase;
    // 旧实现把所有分片拼进一个 std::string => 峰值增量 >= 文件大小 (reserve + 追加还会更糟)。
    // 新实现只持有单个 4 MiB 读写块, 故阈值取文件大小的 1/4, 二者相差一个数量级以上。
    const bound = Math.max(24 * 1024, Math.floor(SIZE / 1024 / 4));
    check(delta < bound,
      '合并期 RSS 峰值增量 ' + (delta / 1024).toFixed(1) + ' MiB << 文件大小 ' + SIZE_MB
        + ' MiB (证明未把整文件载入内存; 采样 ' + samples.length + ' 次)',
      '增量 ' + (delta / 1024).toFixed(1) + ' MiB, 阈值 ' + (bound / 1024).toFixed(0) + ' MiB');
  }
  // 整轮 (含分片上传) 的增量也不得接近文件大小
  if (base !== null && peak !== null) {
    const whole = peak - base;
    check(whole < SIZE / 1024 / 2,
      '整轮上传 RSS 增量 ' + (whole / 1024).toFixed(1) + ' MiB < 文件大小的 1/2',
      '增量 ' + (whole / 1024).toFixed(1) + ' MiB');
  }

  // ---- 7. 端到端完整性: 下载回来重算哈希 ------------------------------------
  const list = await (await fetch(BASE + '/api/file/list?parentId=0', { headers: { 'Authorization': 'Bearer ' + TOK } })).json();
  const item = ((list.data && list.data.items) || []).find(x => x.hash === HASH);
  check(!!item, '上传的文件出现在列表中');
  if (item) {
    const dl = await fetch(BASE + '/api/file/download?token=' + TOK + '&id=' + item.id);
    check(dl.ok, '下载返回 200', 'HTTP ' + dl.status);
    const dh = createHash('sha256');
    const rd = dl.body.getReader();
    for (;;) { const { done: d, value } = await rd.read(); if (d) break; dh.update(value); }
    check(dh.digest('hex') === HASH, '下载内容重算哈希与上传完全一致 (端到端完整性)');

    // 清理: /api/fs/delete 是软删 (进回收站), 还需清空回收站才真正释放 blob 磁盘空间
    const del = await post('/api/fs/delete', { fileIds: [item.id], folderIds: [] }, TOK);
    check(del.code === 0, '清理: 已删除测试文件 (软删)', JSON.stringify(del).slice(0, 100));
    const emptied = await post('/api/trash/empty', {}, TOK);
    check(emptied.code === 0, '清理: 已清空回收站 (真正释放 blob 空间)', JSON.stringify(emptied).slice(0, 100));
  }

  // ---- 8. 断点续传: 换目标目录不得丢弃已传分片 -------------------------------
  // 历史缺陷: 会话复用条件里带了 parent_id。文件的内容身份是 (uid, hashcode),
  // "字节是否已在服务器上" 与 "最终放进哪个目录" 无关。带上 parent_id 后, 用户只要
  // 换个目录 —— 甚至只是把旧目录删掉再重建同名目录 —— 已传分片就全部作废。
  // 实测某用户 143.8 MiB 视频两次上传分别停在第 13/19 片, 第二次完全没复用第一次进度。
  {
    const mk = async (name) => {
      const r = await post('/api/folder/create', { name, parentId: 0 }, TOK);
      return r && r.data && r.data.id;
    };
    const dirA = await mk('resA-' + Date.now());
    const dirB = await mk('resB-' + Date.now());
    check(!!dirA && !!dirB && dirA !== dirB, '准备: 建出两个不同目录 A/B', 'A=' + dirA + ' B=' + dirB);

    // 用 8 MiB (2 片) 的小样本, 快速验证语义
    const RS = 8 * 1024 * 1024;
    const rdata = randomBytes(RS);
    const rhash = createHash('sha256').update(rdata).digest('hex');
    const rname = 'resume-' + Date.now() + '.bin';

    const i1 = await post('/api/upload/init',
      { filename: rname, hash: rhash, size: RS, parentId: dirA, chunkSize: CHUNK, totalChunks: 2 }, TOK);
    const id1 = i1 && i1.data && i1.data.uploadId;
    check(!!id1, 'init(A) 建立会话', JSON.stringify(i1).slice(0, 120));

    // 只传第 0 片, 模拟上传中断
    const c0 = await fetch(BASE + '/api/upload/chunk?uploadId=' + id1 + '&index=0', {
      method: 'POST',
      headers: { 'Authorization': 'Bearer ' + TOK, 'Content-Type': 'application/octet-stream' },
      body: rdata.subarray(0, CHUNK),
    });
    check(c0.ok, '已上传第 0 片后中断', 'HTTP ' + c0.status);

    // 同一文件改为传到目录 B: 必须复用同一会话并报告已有分片
    const i2 = await post('/api/upload/init',
      { filename: rname, hash: rhash, size: RS, parentId: dirB, chunkSize: CHUNK, totalChunks: 2 }, TOK);
    const id2 = i2 && i2.data && i2.data.uploadId;
    const uploaded = (i2 && i2.data && i2.data.uploaded) || [];
    check(id2 === id1, '换目录后 init 复用同一会话 (不从零重传)', '第一次=' + id1 + ' 第二次=' + id2);
    check(uploaded.length === 1 && uploaded[0] === 0,
      'init 返回已传分片 [0], 客户端可跳过', '实际 ' + JSON.stringify(uploaded));

    // 补传剩余分片并完成
    const c1 = await fetch(BASE + '/api/upload/chunk?uploadId=' + id2 + '&index=1', {
      method: 'POST',
      headers: { 'Authorization': 'Bearer ' + TOK, 'Content-Type': 'application/octet-stream' },
      body: rdata.subarray(CHUNK, RS),
    });
    check(c1.ok, '补传第 1 片成功', 'HTTP ' + c1.status);
    const rc = await post('/api/upload/complete', { uploadId: id2 }, TOK);
    check(rc.code === 0 && rc.data && rc.data.hash === rhash,
      '续传后合并成功且哈希正确', JSON.stringify(rc).slice(0, 140));

    // 文件必须落在**新**目录 B (会话的 parent_id 已随 init 改指)
    const lb = await (await fetch(BASE + '/api/file/list?parentId=' + dirB, { headers: { 'Authorization': 'Bearer ' + TOK } })).json();
    const inB = ((lb.data && lb.data.items) || []).some(x => x.hash === rhash);
    check(inB, '文件落在用户最终选择的目录 B (而非中断时的目录 A)');
    const la = await (await fetch(BASE + '/api/file/list?parentId=' + dirA, { headers: { 'Authorization': 'Bearer ' + TOK } })).json();
    const inA = ((la.data && la.data.items) || []).some(x => x.hash === rhash);
    check(!inA, '目录 A 中没有残留副本 (未产生重复占用)');

    // 清理
    const fids = [...((lb.data && lb.data.items) || []), ...((la.data && la.data.items) || [])].map(x => x.id);
    if (fids.length) await post('/api/fs/delete', { fileIds: fids, folderIds: [] }, TOK);
    await post('/api/fs/delete', { fileIds: [], folderIds: [dirA, dirB].filter(Boolean) }, TOK);
    await post('/api/trash/empty', {}, TOK);
  }
} catch (e) {
  bad('执行异常', e && e.message);
} finally {
  try { unlinkSync(TMP); } catch { }
}

console.log('\n结果: %d 通过, %d 失败, %d 跳过', pass, fail, skip);
process.exit(fail === 0 ? 0 : 1);
