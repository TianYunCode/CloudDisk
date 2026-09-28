// P5 账户 & 管理端到端: 资料 / 头像 / 2FA / 管理后台
import { chromium } from 'playwright';
import { execSync } from 'node:child_process';
import crypto from 'node:crypto';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

// base32 secret -> 6 位 TOTP
function totp(secretB32) {
  const clean = secretB32.replace(/=+$/,'');
  const alpha = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
  let bits = '';
  for (const c of clean) { const v = alpha.indexOf(c); if (v < 0) continue; bits += v.toString(2).padStart(5,'0'); }
  const bytes = [];
  for (let i = 0; i + 8 <= bits.length; i += 8) bytes.push(parseInt(bits.slice(i, i + 8), 2));
  const key = Buffer.from(bytes);
  const counter = Math.floor(Date.now() / 1000 / 30);
  const msg = Buffer.alloc(8); msg.writeBigUInt64BE(BigInt(counter));
  const h = crypto.createHmac('sha1', key).update(msg).digest();
  const o = h[h.length - 1] & 0xf;
  const code = ((h[o] & 0x7f) << 24 | h[o+1] << 16 | h[o+2] << 8 | h[o+3]) % 1000000;
  return String(code).padStart(6, '0');
}
const sql = (q) => execSync(`mysql -uroot -p1234 test -N -e "${q}" 2>/dev/null`).toString().trim();

const browser = await chromium.launch();
const errors = [];
try {
  const ctx = await browser.newContext();
  const P = await ctx.newPage();
  P.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  P.on('pageerror', e => errors.push('pageerror: ' + e.message));

  const user = 'acc_' + rnd;
  await P.goto(BASE + '/', { waitUntil: 'networkidle' });
  await P.click('.tab[data-tab="register"]');
  await P.fill('#username', user); await P.fill('#password', 'secret123'); await P.fill('#confirm', 'secret123');
  await P.click('#submitBtn');
  await P.waitForURL('**/app', { timeout: 8000 });
  await P.waitForSelector('#breadcrumb');

  // 进入账户设置
  await P.click('#navAccount');
  await P.waitForSelector('#accountView:not([hidden])', { timeout: 4000 });
  ok('账户设置视图打开');

  // 资料保存
  await P.fill('#acctNickname', '云端小明');
  await P.fill('#acctEmail', 'ming@example.com');
  await P.click('#saveProfileBtn');
  await P.waitForFunction(() => document.getElementById('userName').textContent === '云端小明', { timeout: 5000 });
  ok('资料保存后侧栏昵称更新');

  // 头像上传
  await P.setInputFiles('#avatarInput', '/tmp/avatar.png');
  await P.waitForFunction(() => document.getElementById('avatar').classList.contains('has-img'), { timeout: 8000 });
  ok('头像上传后侧栏显示图片');

  // 2FA 启用
  await P.click('#tfaEnableBtn');
  await P.waitForSelector('.tfa-secret', { timeout: 5000 });
  const secret = (await P.textContent('.tfa-secret')).trim();
  ok('2FA 密钥与二维码展示 (len=' + secret.length + ')');
  await P.fill('#tfaCode', totp(secret));
  await P.click('#tfaConfirmBtn');
  await P.waitForFunction(() => /已启用/.test(document.getElementById('tfaStatus').textContent), { timeout: 5000 });
  ok('输入动态码后 2FA 启用成功');

  // 2FA 关闭 (走 window.prompt)
  P.once('dialog', async d => { await d.accept(totp(secret)); });
  await P.click('#tfaDisableBtn');
  await P.waitForFunction(() => /未启用/.test(document.getElementById('tfaStatus').textContent), { timeout: 5000 });
  ok('2FA 关闭成功');

  // ---- 管理后台 (提升为管理员) ----
  const admin = 'adme_' + rnd;
  const ctx2 = await browser.newContext();
  const A = await ctx2.newPage();
  A.on('pageerror', e => errors.push('admin pageerror: ' + e.message));
  await A.goto(BASE + '/', { waitUntil: 'networkidle' });
  await A.click('.tab[data-tab="register"]');
  await A.fill('#username', admin); await A.fill('#password', 'secret123'); await A.fill('#confirm', 'secret123');
  await A.click('#submitBtn');
  await A.waitForURL('**/app', { timeout: 8000 });
  // 提升为管理员 + 重新登录使前端拿到 role
  sql(`UPDATE tbl_user SET role=1 WHERE username='${admin}'`);
  await A.evaluate(() => { localStorage.clear(); });
  await A.goto(BASE + '/', { waitUntil: 'networkidle' });
  await A.fill('#username', admin); await A.fill('#password', 'secret123');
  await A.click('#submitBtn');
  await A.waitForURL('**/app', { timeout: 8000 });
  await A.waitForFunction(() => { const n = document.getElementById('navAdmin'); return n && !n.hidden; }, { timeout: 6000 });
  ok('管理员登录后显示"管理后台"入口');

  await A.click('#navAdmin');
  await A.waitForSelector('#adminView:not([hidden])', { timeout: 4000 });
  await A.waitForSelector('.stat-card', { timeout: 5000 });
  const cards = await A.$$eval('.stat-card', els => els.length);
  cards >= 5 ? ok('概览统计卡片渲染 (' + cards + ' 张)') : bad('统计卡片过少', cards);

  await A.click('.admin-tab[data-atab="users"]');
  await A.waitForFunction(() => document.querySelectorAll('#adminUserBody tr').length > 0, { timeout: 5000 });
  const rows = await A.$$eval('#adminUserBody tr', els => els.length);
  ok('用户管理表格加载 (' + rows + ' 行)');

  await A.click('.admin-tab[data-atab="audit"]');
  await A.waitForFunction(() => document.querySelectorAll('#adminAuditBody tr').length > 0, { timeout: 5000 });
  ok('审计日志表格加载');

  if (errors.length === 0) ok('无控制台 / JS 错误');
  else bad('存在控制台错误', errors.slice(0, 6).join(' | '));
} catch (e) {
  bad('E2E 异常', e.message + '\n' + (e.stack || '').split('\n').slice(0, 3).join('\n'));
} finally {
  await browser.close();
  console.log('\n结果: ' + pass + ' 通过, ' + fail + ' 失败');
  process.exit(fail === 0 ? 0 : 1);
}
