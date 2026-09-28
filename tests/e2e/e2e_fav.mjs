// 收藏夹端到端: 上传 -> 多选收藏 -> 收藏视图可见 -> 取消收藏
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

  const user = 'fav_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  // 上传一个文件
  const buf = Buffer.from('favorite-e2e-' + rnd);
  await P.setInputFiles('#dropzone input[type=file], input[type=file]', { name: 'fav.txt', mimeType: 'text/plain', buffer: buf });
  await P.waitForFunction(() => /fav\.txt/.test(document.getElementById('fileBody').textContent), { timeout: 8000 });
  ok('文件上传并出现在列表');

  // 全选 -> 收藏
  await P.check('#selAll');
  await P.waitForSelector('#batchBar:not([hidden])', { timeout: 3000 });
  await P.click('#batchFav');
  await P.waitForTimeout(600);
  ok('多选后点击收藏');

  // 进入收藏视图
  await P.click('#navFav');
  await P.waitForSelector('#favView:not([hidden])', { timeout: 4000 });
  await P.waitForFunction(() => /fav\.txt/.test(document.getElementById('favBody').textContent), { timeout: 5000 });
  ok('收藏视图显示已收藏文件');

  // 取消收藏
  await P.click('#favBody [data-unfav]');
  await P.waitForFunction(() => /还没有收藏/.test(document.getElementById('favBody').textContent), { timeout: 5000 });
  ok('取消收藏后列表清空');

  if (errors.length === 0) ok('无控制台 / JS 错误'); else bad('控制台报错', errors.join(' | '));
} catch (e) {
  bad('运行异常', e.message);
} finally {
  await browser.close();
}
console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
process.exit(fail === 0 ? 0 : 1);
