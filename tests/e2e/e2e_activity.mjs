// 活动日志端到端: 触发操作 -> 打开活动日志 -> 校验条目与筛选
import { chromium } from 'playwright';
import fs from 'fs';
import path from 'path';
import os from 'os';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'act-'));
const fileName = 'actfile_' + rnd + '.txt';
const fp = path.join(tmp, fileName); fs.writeFileSync(fp, 'activity-' + rnd);

const browser = await chromium.launch();
const errors = [];
try {
  const ctx = await browser.newContext();
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));

  const user = 'actui_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  await P.setInputFiles('#fileInput', fp);
  await P.waitForFunction((n) => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === n), fileName, { timeout: 15000 });
  ok('上传文件(触发上传活动)');

  await P.waitForTimeout(1200); // 等待异步审计写入

  await P.click('#navActivity');
  await P.waitForSelector('#activityView:not([hidden])', { timeout: 5000 });
  await P.waitForFunction(() => document.querySelectorAll('#activityList .act-row').length >= 2, { timeout: 6000 });
  ok('活动日志列出多条记录');

  const labels = await P.$$eval('#activityList .act-label', els => els.map(e => e.textContent));
  if (labels.some(l => l.includes('上传'))) ok('包含“上传”记录'); else bad('缺少上传记录', labels.join(','));
  if (labels.some(l => l.includes('登录'))) ok('包含“登录”记录'); else bad('缺少登录记录', labels.join(','));

  // 筛选: 仅上传
  await P.selectOption('#activityFilter', 'file_upload');
  await P.waitForFunction(() => {
    const rows = [...document.querySelectorAll('#activityList .act-label')];
    return rows.length >= 1 && rows.every(r => r.textContent.includes('上传'));
  }, { timeout: 5000 });
  ok('按“上传”筛选生效');

  // 恢复全部
  await P.selectOption('#activityFilter', '');
  await P.waitForFunction(() => document.querySelectorAll('#activityList .act-row').length >= 2, { timeout: 5000 });
  ok('恢复全部筛选');

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
