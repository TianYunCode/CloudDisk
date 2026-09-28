// P4 上传增强端到端: 分片/断点续传 (大文件) + 离线下载 + 文件夹上传入口
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
  const A = await ctx.newPage();
  A.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(m.text()); });
  A.on('pageerror', e => errors.push('pageerror: ' + e.message));

  await A.goto(BASE + '/', { waitUntil: 'networkidle' });
  await A.click('.tab[data-tab="register"]');
  await A.fill('#username', 'up_' + rnd); await A.fill('#password', 'secret123'); await A.fill('#confirm', 'secret123');
  await A.click('#submitBtn');
  await A.waitForURL('**/app', { timeout: 8000 });
  await A.waitForSelector('#breadcrumb', { timeout: 5000 });

  // 分片上传大文件 (6.3MB > 4MB 阈值 → 走 /api/upload/*)
  await A.setInputFiles('#fileInput', '/tmp/big6.bin');
  await A.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'big6.bin'), { timeout: 30000 });
  ok('大文件分片上传后出现在列表');
  // 校验下载内容长度一致
  const sizeOk = await A.evaluate(async () => {
    const rows = [...document.querySelectorAll('#fileBody tr[data-kind="file"]')];
    const r = rows.find(x => x.querySelector('.txt').textContent === 'big6.bin');
    const id = r.dataset.id; const tok = localStorage.getItem('cv_token');
    const res = await fetch('/api/file/content?id=' + id + '&token=' + tok);
    const buf = await res.arrayBuffer();
    return buf.byteLength === 6300000;
  });
  sizeOk ? ok('分片合并后文件大小正确 (6300000 字节)') : bad('合并大小不符');

  // 秒传: 再次上传同一文件 → instant 徽标
  await A.setInputFiles('#fileInput', '/tmp/big6.bin');
  await A.waitForFunction(() => document.querySelectorAll('.badge-instant').length >= 1, { timeout: 20000 });
  ok('相同大文件再次上传命中秒传');

  // 文件夹上传入口存在 (目录选择由浏览器原生弹出, 此处校验控件就位)
  const hasFolderInput = await A.evaluate(() => {
    const inp = document.getElementById('folderInput');
    return !!inp && inp.hasAttribute('webkitdirectory') && !!document.getElementById('folderUploadBtn');
  });
  hasFolderInput ? ok('文件夹上传入口就位 (webkitdirectory)') : bad('文件夹上传入口缺失');

  // 离线下载: 通过 UI 下载本站静态文件
  await A.click('#offlineBtn');
  await A.waitForSelector('#offUrl', { timeout: 4000 });
  await A.fill('#offUrl', BASE + '/static/css/style.css');
  await A.fill('#offName', 'style_offline.css');
  // 点击弹窗确认按钮
  await A.evaluate(() => {
    const btn = [...document.querySelectorAll('#modalRoot button')].find(b => /确定|确认|下载|保存|创建|开始/.test(b.textContent));
    (btn || document.querySelector('#modalRoot .btn-primary')).click();
  });
  await A.waitForFunction(() => [...document.querySelectorAll('#fileBody .txt')].some(e => e.textContent === 'style_offline.css'), { timeout: 25000 });
  ok('离线下载完成并出现在列表');

  if (errors.length === 0) ok('无控制台 / JS 错误');
  else bad('存在控制台错误', errors.slice(0, 6).join(' | '));
} catch (e) {
  bad('E2E 异常', e.message + '\n' + (e.stack || '').split('\n').slice(0, 3).join('\n'));
} finally {
  await browser.close();
  console.log('\n结果: ' + pass + ' 通过, ' + fail + ' 失败');
  process.exit(fail === 0 ? 0 : 1);
}
