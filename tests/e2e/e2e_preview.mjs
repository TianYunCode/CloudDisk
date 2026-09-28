// P3 预览端到端: 缩略图 / 图片画廊 / Markdown / 代码 / Range 流
import { chromium } from 'playwright';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const browser = await chromium.launch();
const errors = [];
try {
  const ctx = await browser.newContext();
  const A = await ctx.newPage();
  A.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  A.on('pageerror', e => errors.push('pageerror: ' + e.message));

  await A.goto(BASE + '/', { waitUntil: 'networkidle' });
  await A.click('.tab[data-tab="register"]');
  await A.fill('#username', 'pv_' + rnd); await A.fill('#password', 'secret123'); await A.fill('#confirm', 'secret123');
  await A.click('#submitBtn');
  await A.waitForURL('**/app', { timeout: 8000 });
  await A.waitForSelector('#breadcrumb', { timeout: 5000 });

  // 上传两张图 + 一个 md + 一个代码
  await A.setInputFiles('#fileInput', ['/tmp/pv_a.png', '/tmp/pv_b.png', '/tmp/pv_doc.md', '/tmp/pv_code.py']);
  await A.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].filter(e => /pv_/.test(e.textContent)).length >= 4, { timeout: 20000 });
  ok('上传 2 图 + md + 代码');

  // 缩略图加载成功 (naturalWidth>0)
  await A.waitForFunction(() => {
    const imgs = [...document.querySelectorAll('#fileBody img.thumb')];
    return imgs.length >= 2 && imgs.every(i => i.complete && i.naturalWidth > 0);
  }, { timeout: 15000 });
  ok('列表图片缩略图渲染成功 (服务端生成)');

  // 打开图片预览 -> 画廊 (2 张可切换)
  const imgRow = await A.$('#fileBody tr[data-kind="file"] img.thumb');
  await imgRow.click(); // 点击缩略图? 缩略图在 .ftype, 不是链接; 改用预览按钮
  // 找第一张图片行的预览按钮
  await A.evaluate(() => {
    const rows = [...document.querySelectorAll('#fileBody tr[data-kind="file"]')];
    const r = rows.find(x => /pv_a\.png|pv_b\.png/.test(x.querySelector('.txt').textContent));
    r.querySelector('[data-act="preview"]').click();
  });
  await A.waitForSelector('.pv-img', { timeout: 5000 });
  await A.waitForFunction(() => { const i = document.querySelector('.pv-img'); return i && i.complete && i.naturalWidth > 0; }, { timeout: 8000 });
  ok('图片预览浮层显示大图');
  const hasNav = await A.$('#pvNext');
  if (hasNav) {
    const t1 = await A.textContent('#pvTitle');
    await A.click('#pvNext');
    await A.waitForFunction((prev) => { const el = document.querySelector('#pvTitle'); return el && el.textContent !== prev; }, t1, { timeout: 5000 });
    ok('画廊左右切换生效');
  } else bad('画廊导航缺失');
  await A.keyboard.press('Escape');
  await A.waitForFunction(() => document.querySelector('#previewRoot').children.length === 0, { timeout: 3000 });
  ok('Esc 关闭预览');

  // Markdown 预览
  await A.evaluate(() => {
    const rows = [...document.querySelectorAll('#fileBody tr[data-kind="file"]')];
    const r = rows.find(x => /pv_doc\.md/.test(x.querySelector('.txt').textContent));
    r.querySelector('[data-act="preview"]').click();
  });
  await A.waitForSelector('.pv-doc.md-body', { timeout: 5000 });
  await A.waitForFunction(() => { const d = document.querySelector('.pv-doc.md-body'); return d && d.querySelector('h1') && /加粗/.test(d.innerHTML) && d.querySelector('ul li'); }, { timeout: 6000 });
  ok('Markdown 渲染 (标题/加粗/列表)');
  await A.keyboard.press('Escape');

  // 代码/文本预览
  await A.evaluate(() => {
    const rows = [...document.querySelectorAll('#fileBody tr[data-kind="file"]')];
    const r = rows.find(x => /pv_code\.py/.test(x.querySelector('.txt').textContent));
    r.querySelector('[data-act="preview"]').click();
  });
  await A.waitForFunction(() => { const p = document.querySelector('.pv-pre'); return p && /def hello/.test(p.textContent); }, { timeout: 6000 });
  ok('代码文本预览显示源码');
  await A.keyboard.press('Escape');

  // Range: 页面内 fetch 校验 206
  const rangeOk = await A.evaluate(async () => {
    const rows = [...document.querySelectorAll('#fileBody tr[data-kind="file"]')];
    const r = rows.find(x => /pv_code\.py/.test(x.querySelector('.txt').textContent));
    const id = r.dataset.id;
    const tok = localStorage.getItem('cv_token');
    const res = await fetch('/api/file/content?id=' + id + '&token=' + tok, { headers: { Range: 'bytes=0-3' } });
    return res.status === 206 && res.headers.get('Content-Range') && (await res.text()).length === 4;
  });
  rangeOk ? ok('HTTP Range 请求返回 206 分块') : bad('Range 分块失败');

  if (errors.length === 0) ok('无控制台 / JS 错误');
  else bad('存在控制台错误', errors.slice(0, 6).join(' | '));
} catch (e) {
  bad('E2E 异常', e.message + '\n' + (e.stack || '').split('\n').slice(0, 3).join('\n'));
} finally {
  await browser.close();
  console.log('\n结果: ' + pass + ' 通过, ' + fail + ' 失败');
  process.exit(fail === 0 ? 0 : 1);
}
