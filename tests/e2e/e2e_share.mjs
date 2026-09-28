// P2 分享端到端: 创建分享 / 二维码 / 公开访问 + 提取码 / 转存 / 我的分享管理
import { chromium } from 'playwright';
import fs from 'fs'; import os from 'os'; import path from 'path';

const BASE = process.env.BASE || 'http://127.0.0.1:8888';
const rnd = Date.now();
let pass = 0, fail = 0;
const ok = (m) => { pass++; console.log('  \u2713 ' + m); };
const bad = (m, e) => { fail++; console.log('  \u2717 ' + m + (e ? ' \u2014 ' + e : '')); };

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'shr'));
const f1 = path.join(tmp, 'report.txt'); fs.writeFileSync(f1, 'shared report ' + rnd);

async function api(pathname, body, token) {
  const res = await fetch(BASE + pathname, {
    method: body ? 'POST' : 'GET',
    headers: Object.assign({ 'Content-Type': 'application/json' }, token ? { Authorization: 'Bearer ' + token } : {}),
    body: body ? JSON.stringify(body) : undefined,
  });
  return res.json();
}
async function reg(u) {
  await api('/api/auth/register', { username: u, password: 'secret123' });
  const r = await api('/api/auth/login', { username: u, password: 'secret123' });
  return r.data.token;
}

const browser = await chromium.launch();
const errors = [];
async function newPage(ctx) {
  const p = await ctx.newPage();
  p.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  p.on('pageerror', e => errors.push('pageerror: ' + e.message));
  return p;
}

try {
  const UA = 'shrA_' + rnd;
  // ---- 用户 A: UI 登录并上传 ----
  const ctxA = await browser.newContext();
  const A = await newPage(ctxA);
  await A.goto(BASE + '/', { waitUntil: 'networkidle' });
  await A.click('.tab[data-tab="register"]');
  await A.fill('#username', UA); await A.fill('#password', 'secret123'); await A.fill('#confirm', 'secret123');
  await A.click('#submitBtn');
  await A.waitForURL('**/app', { timeout: 8000 });
  await A.waitForSelector('#breadcrumb', { timeout: 5000 });
  await A.setInputFiles('#fileInput', [f1]);
  await A.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'report.txt'), { timeout: 15000 });
  ok('A 上传 report.txt');

  // ---- 创建分享 (自动提取码, 7天) ----
  const fileRow = await A.$('#fileBody tr[data-kind="file"]');
  await fileRow.$eval('[data-act="share"]', b => b.click());
  await A.waitForSelector('#shCodeMode', { timeout: 4000 });
  await A.selectOption('#shCodeMode', 'auto');
  await A.click('#modalRoot [data-x="ok"]');
  await A.waitForSelector('#shQr svg', { timeout: 5000 });
  ok('分享结果弹窗含二维码 SVG');
  const link = await A.inputValue('#shLink');
  const code = await A.inputValue('#shCodeVal');
  if (/\/share\.html\?t=/.test(link) && code.length >= 3) ok('获得分享链接与提取码 (' + code + ')');
  else bad('链接/提取码异常', link + ' / ' + code);
  await A.click('#modalRoot [data-x="ok"]'); // 完成

  // ---- 我的分享 视图 ----
  await A.click('#navShares');
  await A.waitForFunction(() => [...document.querySelectorAll('#shareBody .txt')].some(e => e.textContent === 'report.txt'), { timeout: 5000 });
  ok('“我的分享”列表显示 report.txt');

  // ---- 公开访问 (未登录): 提取码闸门 ----
  const ctxPub = await browser.newContext();
  const P = await newPage(ctxPub);
  await P.goto(link, { waitUntil: 'networkidle' });
  await P.waitForSelector('#codeInput', { timeout: 5000 });
  ok('公开访问出现提取码输入');
  await P.fill('#codeInput', 'zzzz'); await P.click('#codeBtn');
  await P.waitForFunction(() => document.querySelector('#codeInput') !== null, { timeout: 3000 });
  ok('错误提取码被拦截 (仍在闸门)');
  await P.fill('#codeInput', code); await P.click('#codeBtn');
  await P.waitForFunction(() => [...document.querySelectorAll('#spBody .txt')].some(e => e.textContent === 'report.txt'), { timeout: 5000 });
  ok('正确提取码后看到 report.txt + 下载按钮');
  if (await P.$('#spBody [data-act="dl"]')) ok('下载按钮存在'); else bad('缺少下载按钮');

  // ---- 用户 B: 在分享页转存 ----
  const B = await reg('shrB_' + rnd);
  const ctxB = await browser.newContext();
  const PB = await newPage(ctxB);
  await ctxB.addInitScript((tok) => localStorage.setItem('cv_token', tok), B);
  await PB.goto(link, { waitUntil: 'networkidle' });
  // 有提取码 -> 输入
  if (await PB.$('#codeInput')) { await PB.fill('#codeInput', code); await PB.click('#codeBtn'); }
  await PB.waitForSelector('#spSave', { timeout: 5000 });
  await PB.click('#spSave');
  await PB.waitForFunction(() => [...document.querySelectorAll('#toasts .toast')].some(t => /转存/.test(t.textContent)), { timeout: 6000 });
  ok('B 在分享页转存成功');
  const bList = await api('/api/file/list?parentId=0', null, B);
  if ((bList.data.items || []).some(i => i.filename === 'report.txt')) ok('B 网盘根目录出现 report.txt'); else bad('B 转存文件缺失');

  // ---- 取消分享 -> 链接失效 ----
  await A.click('#shareBody [data-act="cancel"]');
  await A.click('#modalRoot [data-x="ok"]');
  await A.waitForTimeout(600);
  const P2 = await newPage(ctxPub);
  await P2.goto(link, { waitUntil: 'networkidle' });
  await P2.waitForFunction(() => /无法访问|失效|取消/.test(document.body.textContent), { timeout: 5000 });
  ok('取消后公开访问失效');

  if (errors.length === 0) ok('无控制台 / JS 错误');
  else bad('存在控制台错误', errors.slice(0, 6).join(' | '));
} catch (e) {
  bad('E2E 异常', e.message + '\n' + (e.stack || '').split('\n').slice(0,3).join('\n'));
} finally {
  await browser.close();
  console.log('\n结果: ' + pass + ' 通过, ' + fail + ' 失败');
  process.exit(fail === 0 ? 0 : 1);
}
