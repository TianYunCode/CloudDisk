// P6 开发者令牌 UI 端到端: 创建 / 明文展示 / 列表 / 吊销
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

  const user = 'devt_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  await P.click('#navAccount');
  await P.waitForSelector('#accountView:not([hidden])', { timeout: 4000 });
  ok('账户设置视图打开');

  // WebDAV 地址展示
  const wd = await P.textContent('#webdavUrl');
  if (wd && wd.includes('/webdav/')) ok('WebDAV 地址展示: ' + wd); else bad('WebDAV 地址缺失', wd);

  // 初始无令牌
  await P.waitForSelector('#tokBody', { timeout: 4000 });
  const empty = await P.textContent('#tokBody');
  if (/暂无令牌/.test(empty)) ok('初始令牌列表为空'); else bad('初始列表非空', empty);

  // 创建令牌
  await P.fill('#tokName', 'e2e-cli');
  await P.click('#tokCreateBtn');
  await P.waitForSelector('#tokRevealVal', { timeout: 5000 });
  const plain = await P.textContent('#tokRevealVal');
  if (plain && plain.startsWith('cvt_')) ok('创建后展示明文令牌'); else bad('明文令牌异常', plain);

  // 列表出现该令牌
  await P.waitForFunction(() => /e2e-cli/.test(document.getElementById('tokBody').textContent), { timeout: 5000 });
  ok('令牌出现在列表');

  // 吊销 (自动确认对话框)
  P.on('dialog', d => d.accept());
  await P.click('#tokBody [data-revoke]');
  await P.waitForFunction(() => /暂无令牌/.test(document.getElementById('tokBody').textContent), { timeout: 5000 });
  ok('吊销后列表恢复为空');

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
