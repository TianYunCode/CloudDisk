// 统计分析仪表盘端到端: 上传文件 -> 打开统计 -> KPI/类型/最大文件/趋势渲染
import { chromium } from 'playwright';
import fs from 'fs';
import path from 'path';
import os from 'os';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'stats-'));
const img = path.join(tmp, 'pic_' + rnd + '.png'); fs.writeFileSync(img, 'IMGDATA-' + rnd);
const doc = path.join(tmp, 'report_' + rnd + '.pdf'); fs.writeFileSync(doc, 'DOCDATA-longer-content-' + rnd);

const browser = await chromium.launch();
const errors = [];
try {
  const ctx = await browser.newContext();
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));

  const user = 'stats_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  await P.setInputFiles('#fileInput', [img, doc]);
  await P.waitForFunction(() => document.querySelectorAll('#fileBody tr').length >= 2, { timeout: 15000 });
  ok('上传两个不同类型文件');

  await P.click('#navStats');
  await P.waitForSelector('#statsView:not([hidden])', { timeout: 5000 });
  await P.waitForFunction(() => document.querySelectorAll('#statsKpis .kpi-card').length >= 4, { timeout: 5000 });
  ok('KPI 卡片渲染');

  const filesKpi = await P.evaluate(() => {
    const cards = [...document.querySelectorAll('#statsKpis .kpi-card')];
    const c = cards.find(x => x.querySelector('.kpi-label').textContent.includes('文件数'));
    return c ? c.querySelector('.kpi-value').textContent : '';
  });
  if (filesKpi === '2') ok('文件数 KPI = 2'); else bad('文件数 KPI 异常', filesKpi);

  await P.waitForFunction(() => document.querySelectorAll('#statsTypes .bar-row').length >= 2, { timeout: 5000 });
  ok('文件类型分布至少 2 类');

  await P.waitForFunction(() => document.querySelectorAll('#statsLargest .lg-row').length >= 2, { timeout: 5000 });
  ok('最大文件列表渲染');

  const cols = await P.$$eval('#statsTimeline .tl-col', els => els.length);
  if (cols === 14) ok('上传趋势 14 天柱状'); else bad('趋势列数异常', cols);

  // 切回文件视图正常
  await P.click('#navFiles');
  await P.waitForSelector('#filesView:not([hidden])', { timeout: 5000 });
  ok('切回文件视图正常');

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
