// 缩略图请求端到端: 验证前端只为"后端确实能生成缩略图"的文件请求 /api/file/thumb
//
// 背景: "浏览器能预览为图片" (含 webp/svg/ico) 与 "后端能生成缩略图"
// (png/jpg/jpeg/gif/bmp/tga/psd/ppm/pgm) 是两个不同集合。前端若按扩展名自行猜测,
// 就会为 svg/webp/ico 白发请求并收到 404, 同时出现"图片→回退图标"的闪烁。
// 现由后端在文件列表下发权威的 hasThumb 字段, 本测试守住该行为不回退。
import { chromium } from 'playwright';
import { deflateSync, crc32 } from 'zlib';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };
// 条件式断言: 必须显式区分通过/失败 (把条件传给 ok() 会无条件打印 ✓, 掩盖失败)
const check = (cond, m, e) => { if (cond) ok(m); else bad(m, e); };

// 生成一个真实可解码的 4x4 红色 PNG (后端 stb_image 需能解出才会返回 200)
function makePng() {
  const w = 4, h = 4;
  const row = Buffer.concat([Buffer.from([0]), Buffer.from(Array(w).fill(0).map(() => [255, 0, 0]).flat())]);
  const raw = Buffer.concat(Array.from({ length: h }, () => row));
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body) >>> 0);
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0)),
  ]);
}

const SVG = '<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="8" height="8" fill="red"/></svg>';
// webp/ico 内容无需有效: 后端按扩展名判定不可缩略, 前端根本不会请求
const WEBP = Buffer.from('UklGRiQAAABXRUJQVlA4IBgAAAAwAQCdASoBAAEAAwA0JaQAA3AA/vuUAAA=', 'base64');
const ICO = Buffer.from([0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 24, 0, 40, 0]);

const NAMES = ['a.png', 'b.svg', 'c.webp', 'd.ico'];
const browser = await chromium.launch();
const errors = [];
const thumbReqs = [];      // { url, status }
try {
  const ctx = await browser.newContext();
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));
  P.on('response', r => { if (r.url().includes('/api/file/thumb')) thumbReqs.push({ url: r.url(), status: r.status() }); });

  const user = 'thumb_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');
  ok('注册并进入网盘页');

  // 上传 png(可缩略) + svg/webp/ico(后端不可缩略)
  await P.setInputFiles('#dropzone input[type=file], input[type=file]', [
    { name: 'a.png', mimeType: 'image/png', buffer: makePng() },
    { name: 'b.svg', mimeType: 'image/svg+xml', buffer: Buffer.from(SVG) },
    { name: 'c.webp', mimeType: 'image/webp', buffer: WEBP },
    { name: 'd.ico', mimeType: 'image/x-icon', buffer: ICO },
  ]);
  for (const n of NAMES) {
    await P.waitForFunction(nm => document.getElementById('fileBody').textContent.includes(nm), n, { timeout: 10000 });
  }
  ok('4 个文件上传并出现在列表');

  // 列表接口应下发 hasThumb, 且与各扩展名的真实可缩略性一致
  const flags = await P.evaluate(async () => {
    const t = localStorage.getItem('cv_token');
    const r = await fetch('/api/file/list?parentId=0', { headers: { Authorization: 'Bearer ' + t } });
    const d = await r.json();
    return (d.data.items || []).map(i => [i.filename, i.hasThumb]);
  });
  const byName = Object.fromEntries(flags);
  check(NAMES.every(n => n in byName) && NAMES.every(n => typeof byName[n] === 'boolean'),
    '列表为每个文件下发布尔 hasThumb', JSON.stringify(flags));
  check(byName['a.png'] === true, 'a.png  -> hasThumb=true (后端可生成)');
  check(byName['b.svg'] === false, 'b.svg  -> hasThumb=false');
  check(byName['c.webp'] === false, 'c.webp -> hasThumb=false');
  check(byName['d.ico'] === false, 'd.ico  -> hasThumb=false');

  // 整页刷新以观察稳态渲染的请求 (点击当前视图不会触发重新渲染, 会误判为 0 次)
  thumbReqs.length = 0;
  await P.reload({ waitUntil: 'networkidle' });
  await P.waitForFunction(ns => {
    const b = document.getElementById('fileBody');
    return b && ns.every(n => b.textContent.includes(n));
  }, NAMES, { timeout: 10000 });
  await P.waitForTimeout(2500);   // 等懒加载缩略图触发

  const notFound = thumbReqs.filter(r => r.status === 404);
  check(notFound.length === 0,
    '稳态渲染无任何 /api/file/thumb 404 (不再为 svg/webp/ico 白发请求)',
    notFound.map(r => r.status + ' ?' + (r.url.split('?')[1] || '')).join(', '));
  check(thumbReqs.length === 1,
    '只为唯一可缩略的 a.png 发出 1 次缩略图请求',
    '实际 ' + thumbReqs.length + ' 次: ' + thumbReqs.map(r => r.status + ' ?' + (r.url.split('?')[1] || '')).join(', '));
  check(thumbReqs.length === 1 && thumbReqs[0].status === 200,
    '该缩略图请求返回 200 (后端确实能生成)',
    thumbReqs.map(r => String(r.status)).join(','));

  // 页面应为 a.png 渲染 <img class="thumb">, 而 svg/webp/ico 行不应有 thumb 元素
  const domCounts = await P.evaluate(() => {
    const rows = [...document.querySelectorAll('#fileBody tr')];
    return rows.map(r => [(r.querySelector('.txt') || {}).textContent || '', !!r.querySelector('img.thumb')]);
  });
  const pngRow = domCounts.find(r => r[0].includes('a.png'));
  const otherRows = domCounts.filter(r => /b\.svg|c\.webp|d\.ico/.test(r[0]));
  check(!!pngRow && pngRow[1] === true, 'a.png 行渲染了缩略图 <img>', JSON.stringify(pngRow));
  check(otherRows.length === 3 && otherRows.every(r => r[1] === false),
    'svg/webp/ico 三行均未渲染 <img class="thumb"> (改用类型图标, 无闪烁)',
    JSON.stringify(otherRows));

  // svg/webp/ico 仍应能作为图片预览 (这是与缩略图不同的能力, 不应被一并砍掉)
  const cats = await P.evaluate(ns => {
    const Pv = window.CV.Preview;
    return ns.map(n => [n, Pv.previewCat(n)]);
  }, NAMES);
  check(cats.every(c => c[1] === 'image'),
    'svg/webp/ico 仍被识别为可预览图片 (预览能力不受影响)', JSON.stringify(cats));

  check(errors.length === 0, '无控制台 / JS 错误', errors.slice(0, 3).join(' | '));
} catch (e) {
  bad('执行异常', e.message);
} finally {
  await browser.close();
}

console.log('\n结果: ' + pass + ' 通过, ' + fail + ' 失败');
process.exit(fail === 0 ? 0 : 1);
