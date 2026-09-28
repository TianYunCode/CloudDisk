// P1 端到端验证: 文件夹 / 面包屑 / 多选 / 移动 / 回收站 (Playwright)
import { chromium } from 'playwright';
import fs from 'fs';
import os from 'os';
import path from 'path';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const U = 'e2e_p1_' + Date.now();
const PW = 'secret123';
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'e2e'));
const f1 = path.join(tmp, 'hello.txt'); fs.writeFileSync(f1, 'hello world ' + U);
const f2 = path.join(tmp, 'notes.md');  fs.writeFileSync(f2, '# notes ' + U);

const browser = await chromium.launch();
const ctx = await browser.newContext();
const page = await ctx.newPage();
const errors = [];
page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
page.on('pageerror', e => errors.push('pageerror: ' + e.message));

try {
  // ---- 注册 ----
  await page.goto(BASE + '/', { waitUntil: 'networkidle' });
  await page.click('.tab[data-tab="register"]');
  await page.fill('#username', U);
  await page.fill('#password', PW);
  await page.fill('#confirm', PW);
  await page.click('#submitBtn');
  await page.waitForURL('**/app', { timeout: 8000 });
  ok('注册并进入云盘');

  await page.waitForSelector('#breadcrumb', { timeout: 5000 });

  // ---- 新建文件夹 ----
  await page.click('#newFolderBtn');
  await page.fill('#folderNameInput', 'Project');
  await page.click('#modalRoot [data-x="ok"]');
  await page.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'Project'), { timeout: 5000 });
  ok('新建文件夹 Project 出现在列表');

  // ---- 进入文件夹, 检查面包屑 ----
  await page.click('#fileBody .folder-name .txt.link');
  await page.waitForFunction(() => [...document.querySelectorAll('#breadcrumb .crumb')].some(e => e.textContent === 'Project'), { timeout: 5000 });
  ok('进入文件夹后面包屑显示 Project');

  // ---- 上传文件到该文件夹 ----
  await page.setInputFiles('#fileInput', [f1, f2]);
  await page.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'hello.txt'), { timeout: 15000 });
  await page.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'notes.md'), { timeout: 15000 });
  ok('上传的 2 个文件出现在当前文件夹');

  // ---- 返回根目录 ----
  await page.click('#breadcrumb .crumb'); // 根目录
  await page.waitForFunction(() => {
    const names = [...document.querySelectorAll('#fileBody .txt')].map(e => e.textContent);
    return names.includes('Project') && !names.includes('hello.txt');
  }, { timeout: 5000 });
  ok('返回根目录 (文件在子目录, 根目录不可见)');

  // ---- 全选并删除 (移入回收站) ----
  await page.check('#selAll');
  await page.waitForSelector('#batchBar:not([hidden])', { timeout: 3000 });
  await page.click('#batchDelete');
  await page.click('#modalRoot [data-x="ok"]');
  await page.waitForFunction(() => document.querySelectorAll('#fileBody tr[data-kind]').length === 0, { timeout: 5000 });
  ok('批量删除后根目录清空');

  // ---- 回收站: 恢复 ----
  await page.click('#navTrash');
  await page.waitForFunction(() => [...document.querySelectorAll('#trashBody .txt')].some(e => e.textContent === 'Project'), { timeout: 5000 });
  ok('回收站显示已删除的 Project');
  await page.click('#trashBody [data-act="restore"]');
  await page.waitForFunction(() => document.querySelectorAll('#trashBody tr').length === 0
     || ![...document.querySelectorAll('#trashBody .txt')].some(e => e.textContent === 'Project'), { timeout: 5000 });
  ok('从回收站恢复 Project');

  // ---- 回到文件, 确认恢复 ----
  await page.click('#navFiles');
  await page.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'Project'), { timeout: 5000 });
  ok('恢复后 Project 回到根目录');

  // ---- 再删并清空回收站 ----
  await page.check('#selAll');
  await page.click('#batchDelete');
  await page.click('#modalRoot [data-x="ok"]');
  await page.waitForTimeout(500);
  await page.click('#navTrash');
  await page.waitForSelector('#emptyTrashBtn', { timeout: 3000 });
  await page.click('#emptyTrashBtn');
  await page.click('#modalRoot [data-x="ok"]');
  await page.waitForFunction(() => document.querySelectorAll('#trashBody tr[data-kind], #trashBody .txt').length === 0, { timeout: 5000 });
  ok('清空回收站成功');

  if (errors.length === 0) ok('无控制台 / JS 错误');
  else bad('存在控制台错误', errors.slice(0, 5).join(' | '));
} catch (e) {
  bad('E2E 异常', e.message);
} finally {
  await browser.close();
  console.log('\n结果: ' + pass + ' 通过, ' + fail + ' 失败');
  process.exit(fail === 0 ? 0 : 1);
}
