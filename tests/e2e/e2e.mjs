// CloudVault E2E test using Playwright (Chromium, headless, no-sandbox)
// Run: node e2e.mjs
import { chromium } from 'playwright';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const BASE = 'http://127.0.0.1:8888';
const SAMPLE = path.join(__dirname, 'sample.txt');
const UPLOAD_NAME = 'sample.txt';

const results = [];
function rec(step, ok, detail = '') {
  results.push({ step, ok, detail });
  const tag = ok ? 'PASS' : 'FAIL';
  console.log(`[${tag}] ${step}${detail ? ' :: ' + detail : ''}`);
}

const rnd = 'e2e_' + Math.random().toString(36).slice(2, 10);
const PW = 'test123456';

const consoleErrors = [];
const pageErrors = [];

(async () => {
  const browser = await chromium.launch({
    headless: true,
    args: ['--no-sandbox', '--disable-setuid-sandbox'],
  });
  const context = await browser.newContext();
  const page = await context.newPage();

  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', (err) => {
    pageErrors.push(err.message + (err.stack ? '\n' + err.stack : ''));
  });

  try {
    // ---- Step 1: open login page ----
    await page.goto(BASE + '/', { waitUntil: 'networkidle' });
    const title = await page.title();
    rec('open / (login page)', /登录|CloudVault/.test(title), 'title=' + JSON.stringify(title));

    // ---- Step 2: switch to register tab ----
    await page.click('.tab[data-tab="register"]');
    await page.waitForSelector('#confirmField', { state: 'visible' });
    const submitTxt = await page.textContent('#submitText');
    rec('switch to register tab', (submitTxt || '').includes('创建'), 'submitText=' + JSON.stringify(submitTxt));

    // ---- Step 3: register + auto-login + redirect to /app ----
    await page.fill('#username', rnd);
    await page.fill('#password', PW);
    await page.fill('#confirm', PW);
    await page.click('#submitBtn');
    await page.waitForURL('**/app', { timeout: 15000 });
    rec('register -> auto-login -> redirect /app', page.url().endsWith('/app'), 'url=' + page.url() + ' user=' + rnd);

    // ---- Step 4: file list loads ----
    await page.waitForSelector('#fileBody', { state: 'attached', timeout: 10000 });
    // wait until skeleton clears (either rows or empty state)
    await page.waitForFunction(() => {
      const b = document.getElementById('fileBody');
      const extra = document.getElementById('listExtra');
      const hasSkeleton = b && b.querySelector('.skeleton');
      return !hasSkeleton && ((b && b.children.length >= 0));
    }, { timeout: 10000 });
    rec('file list loaded on /app', true, '');

    // ---- Step 5: upload file via hidden #fileInput ----
    await page.setInputFiles('#fileInput', SAMPLE);
    // wait for file to appear in table body
    await page.waitForFunction((name) => {
      const rows = document.querySelectorAll('#fileBody tr');
      for (const r of rows) {
        const t = r.querySelector('.txt');
        if (t && t.textContent === name) return true;
      }
      return false;
    }, UPLOAD_NAME, { timeout: 20000 });
    rec('upload file appears in #fileBody', true, 'name=' + UPLOAD_NAME);

    // ---- Step 6: search filter ----
    await page.fill('#searchInput', UPLOAD_NAME);
    await page.waitForTimeout(600); // debounce 300ms + request
    await page.waitForFunction(() => {
      const b = document.getElementById('fileBody');
      return b && !b.querySelector('.skeleton');
    }, { timeout: 10000 });
    const matchCount = await page.evaluate((name) => {
      const rows = [...document.querySelectorAll('#fileBody tr')];
      return rows.filter((r) => {
        const t = r.querySelector('.txt');
        return t && t.textContent === name;
      }).length;
    }, UPLOAD_NAME);
    const totalRows = await page.evaluate(() => document.querySelectorAll('#fileBody tr').length);
    rec('search filters to uploaded file', matchCount === 1, 'matchRows=' + matchCount + ' totalRows=' + totalRows);

    // ---- Step 7: rename ----
    const NEWNAME = 'renamed_' + rnd + '.txt';
    // find the row for UPLOAD_NAME and click its rename button
    await page.evaluate((name) => {
      const rows = [...document.querySelectorAll('#fileBody tr')];
      const row = rows.find((r) => { const t = r.querySelector('.txt'); return t && t.textContent === name; });
      row.querySelector('button[data-act="rename"]').click();
    }, UPLOAD_NAME);
    await page.waitForSelector('#renameInput', { state: 'visible', timeout: 8000 });
    await page.fill('#renameInput', NEWNAME);
    await page.click('button[data-x="ok"]');
    // clear search so renamed file (may not match old keyword) is visible; then look for new name
    await page.fill('#searchInput', NEWNAME);
    await page.waitForTimeout(600);
    await page.waitForFunction((name) => {
      const rows = document.querySelectorAll('#fileBody tr');
      for (const r of rows) { const t = r.querySelector('.txt'); if (t && t.textContent === name) return true; }
      return false;
    }, NEWNAME, { timeout: 15000 });
    rec('rename file (new name appears)', true, 'newName=' + NEWNAME);

    // ---- Step 8: delete ----
    await page.evaluate((name) => {
      const rows = [...document.querySelectorAll('#fileBody tr')];
      const row = rows.find((r) => { const t = r.querySelector('.txt'); return t && t.textContent === name; });
      row.querySelector('button[data-act="delete"]').click();
    }, NEWNAME);
    await page.waitForSelector('button[data-x="ok"]', { state: 'visible', timeout: 8000 });
    await page.click('button[data-x="ok"]');
    await page.waitForFunction((name) => {
      const rows = document.querySelectorAll('#fileBody tr');
      for (const r of rows) { const t = r.querySelector('.txt'); if (t && t.textContent === name) return false; }
      return true;
    }, NEWNAME, { timeout: 15000 });
    rec('delete file (disappears)', true, 'deleted=' + NEWNAME);

    // ---- Step 9: dark mode toggle ----
    await page.click('#themeToggle');
    await page.waitForFunction(() => document.documentElement.getAttribute('data-theme') === 'dark', { timeout: 5000 });
    const theme = await page.getAttribute('html', 'data-theme');
    rec('dark mode toggle sets data-theme=dark', theme === 'dark', 'data-theme=' + theme);

    // ---- Step 10: logout ----
    await page.click('#logoutBtn');
    await page.waitForURL((url) => {
      const u = new URL(url);
      return u.pathname === '/' || u.pathname.endsWith('index.html');
    }, { timeout: 10000 });
    rec('logout redirects to /', true, 'url=' + page.url());
  } catch (err) {
    rec('EXCEPTION during test', false, (err && err.message ? err.message : String(err)));
  } finally {
    // any JS error => overall fail signal
    if (consoleErrors.length) rec('no console errors', false, consoleErrors.length + ' console error(s)');
    else rec('no console errors', true, '');
    if (pageErrors.length) rec('no pageerror events', false, pageErrors.length + ' pageerror(s)');
    else rec('no pageerror events', true, '');

    await browser.close();

    const passed = results.filter((r) => r.ok).length;
    const total = results.length;
    console.log('\n================ SUMMARY ================');
    for (const r of results) console.log(`  ${r.ok ? 'PASS' : 'FAIL'}  ${r.step}`);
    console.log(`  TOTAL: ${passed}/${total} passed`);
    if (consoleErrors.length) {
      console.log('\n---- CONSOLE ERRORS (verbatim) ----');
      consoleErrors.forEach((e, i) => console.log(`[${i + 1}] ${e}`));
    }
    if (pageErrors.length) {
      console.log('\n---- PAGEERROR EVENTS (verbatim) ----');
      pageErrors.forEach((e, i) => console.log(`[${i + 1}] ${e}`));
    }
    console.log('========================================');
    process.exit(passed === total ? 0 : 1);
  }
})();
