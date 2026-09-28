// 全局搜索端到端: 跨目录按名称搜索 -> 结果可见 -> 打开定位
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
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));

  const user = 'srch_' + rnd;
  const folderName = 'ProjX_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  // 建目录
  await P.click('#newFolderBtn');
  await P.waitForSelector('#folderNameInput', { timeout: 4000 });
  await P.fill('#folderNameInput', folderName);
  await P.click('[data-x="ok"]');
  await P.waitForFunction((n) => document.getElementById('fileBody').textContent.includes(n), folderName, { timeout: 5000 });
  ok('创建目标文件夹');

  // 切到全局搜索
  await P.selectOption('#searchScope', 'global');
  await P.fill('#searchInput', folderName);
  await P.waitForFunction((n) => document.getElementById('fileBody').textContent.includes(n), folderName, { timeout: 5000 });
  ok('全局搜索命中新建文件夹');

  // 部分关键字也能命中
  await P.fill('#searchInput', 'ProjX');
  await P.waitForFunction(() => /ProjX_/.test(document.getElementById('fileBody').textContent), { timeout: 5000 });
  ok('部分关键字命中');

  // 点“打开”定位到该目录
  await P.click('#fileBody [data-go]');
  await P.waitForTimeout(500);
  const scopeReset = await P.$eval('#searchScope', el => el.value);
  if (scopeReset === 'cur') ok('打开后搜索范围复位为当前目录'); else bad('范围未复位', scopeReset);

  // 空结果
  await P.selectOption('#searchScope', 'global');
  await P.fill('#searchInput', 'zzz_nomatch_' + rnd);
  await P.waitForFunction(() => /没有匹配/.test(document.getElementById('fileBody').textContent), { timeout: 5000 });
  ok('无匹配时显示空态');

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
