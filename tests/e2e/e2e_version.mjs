// 文件版本历史端到端: 上传新版本 -> 列表出现历史 -> 恢复
import { chromium } from 'playwright';
import fs from 'fs';
import path from 'path';
import os from 'os';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ver-'));
const fileName = 'doc_' + rnd + '.txt';
const v1 = path.join(tmp, fileName); fs.writeFileSync(v1, 'content-v1-' + rnd);
const v2 = path.join(tmp, 'v2.txt');  fs.writeFileSync(v2, 'content-v2-longer-' + rnd);

const browser = await chromium.launch();
const errors = [];
try {
  const ctx = await browser.newContext();
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));

  const user = 'verui_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  // 上传初始文件
  await P.setInputFiles('#fileInput', v1);
  await P.waitForFunction((n) => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === n), fileName, { timeout: 15000 });
  ok('上传初始文件');

  // 打开该文件的历史版本弹窗
  await P.evaluate((n) => {
    const rows = [...document.querySelectorAll('#fileBody tr')];
    const row = rows.find(r => { const t = r.querySelector('.txt'); return t && t.textContent === n; });
    row.querySelector('button[data-act="versions"]').click();
  }, fileName);
  await P.waitForSelector('#verList', { timeout: 5000 });
  await P.waitForFunction(() => /暂无历史版本/.test(document.getElementById('verList').textContent), { timeout: 5000 });
  ok('初始无历史版本');

  // 上传新版本
  await P.setInputFiles('#verFile', v2);
  await P.waitForFunction(() => /版本 #\d+/.test(document.getElementById('verList').textContent), { timeout: 8000 });
  ok('上传新版本后出现历史条目');

  // 恢复到历史版本 (即初始内容)
  await P.click('#verList [data-restore]');
  await P.waitForTimeout(800);
  const stillListed = await P.$eval('#verList', el => /版本 #\d+/.test(el.textContent));
  if (stillListed) ok('恢复后历史仍在(当前版本已归档)'); else bad('恢复后历史丢失');

  // 关闭弹窗
  await P.click('[data-x="ok"]');
  await P.waitForFunction(() => !document.getElementById('modalRoot').classList.contains('show'), { timeout: 3000 });
  ok('关闭历史版本弹窗');

  // 校验恢复后下载内容为 v1
  const dlText = await P.evaluate(async (n) => {
    const rows = [...document.querySelectorAll('#fileBody tr')];
    const row = rows.find(r => { const t = r.querySelector('.txt'); return t && t.textContent === n; });
    const id = row.dataset.id;
    const tok = window.CV.Store.token;
    const r = await fetch('/api/file/download?id=' + id + '&token=' + encodeURIComponent(tok));
    return await r.text();
  }, fileName);
  if (/content-v1-/.test(dlText)) ok('恢复后当前内容为初始版本'); else bad('恢复内容不符', dlText.slice(0, 40));

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
