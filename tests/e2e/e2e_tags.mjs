// 文件标签端到端: 新建标签+打标 -> 行内标签芯片 -> 标签视图筛选 -> 删除标签
import { chromium } from 'playwright';
import fs from 'fs';
import path from 'path';
import os from 'os';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tag-'));
const fileName = 'tagfile_' + rnd + '.txt';
const fp = path.join(tmp, fileName); fs.writeFileSync(fp, 'tag-content-' + rnd);
const tagName = '项目' + (rnd % 1000);

const browser = await chromium.launch();
const errors = [];
try {
  const ctx = await browser.newContext();
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));

  const user = 'tagui_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  await P.setInputFiles('#fileInput', fp);
  await P.waitForFunction((n) => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === n), fileName, { timeout: 15000 });
  ok('上传文件');

  // 打开标签弹窗
  await P.evaluate((n) => {
    const rows = [...document.querySelectorAll('#fileBody tr')];
    const row = rows.find(r => { const t = r.querySelector('.txt'); return t && t.textContent === n; });
    row.querySelector('button[data-act="tags"]').click();
  }, fileName);
  await P.waitForSelector('#tagPicker', { timeout: 5000 });
  // 新建标签
  await P.fill('#tagNewName', tagName);
  await P.click('#tagNewBtn');
  await P.waitForFunction((tn) => [...document.querySelectorAll('#tagPicker .tag-opt')].some(b => b.textContent === tn && b.classList.contains('on')), tagName, { timeout: 5000 });
  ok('新建标签并自动选中');
  // 保存
  await P.click('[data-x="ok"]');
  await P.waitForFunction(() => !document.getElementById('modalRoot').classList.contains('show'), { timeout: 3000 });

  // 行内芯片
  await P.waitForFunction((args) => {
    const [n, tn] = args;
    const rows = [...document.querySelectorAll('#fileBody tr')];
    const row = rows.find(r => { const t = r.querySelector('.txt'); return t && t.textContent === n; });
    return row && [...row.querySelectorAll('.row-tags .tag-chip')].some(c => c.textContent === tn);
  }, [fileName, tagName], { timeout: 5000 });
  ok('文件行显示标签芯片');

  // 标签视图
  await P.click('#navTags');
  await P.waitForSelector('#tagsView:not([hidden])', { timeout: 5000 });
  await P.waitForFunction((tn) => [...document.querySelectorAll('#tagCloud .tag-card')].some(c => c.querySelector('.tag-card-name').textContent === tn), tagName, { timeout: 5000 });
  const cnt = await P.evaluate((tn) => {
    const c = [...document.querySelectorAll('#tagCloud .tag-card')].find(x => x.querySelector('.tag-card-name').textContent === tn);
    return c.querySelector('.tag-card-count').textContent;
  }, tagName);
  if (cnt === '1') ok('标签视图计数=1'); else bad('标签计数异常', cnt);

  // 点击标签查看文件
  await P.evaluate((tn) => {
    const c = [...document.querySelectorAll('#tagCloud .tag-card')].find(x => x.querySelector('.tag-card-name').textContent === tn);
    c.click();
  }, tagName);
  await P.waitForFunction((n) => {
    const card = document.getElementById('tagFilesCard');
    return card && !card.hidden && [...card.querySelectorAll('.lg-name')].some(e => e.textContent === n);
  }, fileName, { timeout: 5000 });
  ok('标签筛选列出该文件');

  // 删除标签
  await P.evaluate((tn) => {
    const c = [...document.querySelectorAll('#tagCloud .tag-card')].find(x => x.querySelector('.tag-card-name').textContent === tn);
    c.querySelector('[data-del]').click();
  }, tagName);
  await P.waitForSelector('#modalRoot.show', { timeout: 3000 });
  await P.click('[data-x="ok"]');
  await P.waitForFunction((tn) => ![...document.querySelectorAll('#tagCloud .tag-card')].some(c => c.querySelector('.tag-card-name') && c.querySelector('.tag-card-name').textContent === tn), tagName, { timeout: 5000 });
  ok('删除标签后从视图消失');

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
