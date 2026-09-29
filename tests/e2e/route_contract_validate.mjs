/* =============================================================================
   route_contract_validate.mjs —— 前后端 API 路由契约测试

   校验三条不变量:
     1. 前端调用的每个路径, 后端都已注册  → 防止拼写漂移导致的静默 404
     2. 后端注册的每个 /api 路由, 前端都有调用方 → 防止死端点堆积
     3. 后端非 /api 路由只能是已知的页面/运维端点白名单

   数据来源:
     · 后端: 扫描 server/gateway 下所有 .cpp/.h 中的 .GET/.POST/.PUT/.DELETE/.PATCH("...")
     · 前端: 扫描 web/js/core 与 web/js/views 下所有 .js 中的 '/api/...' 字面量 (截断查询串)
   ============================================================================= */
import { readFileSync, readdirSync, statSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

let fail = 0, total = 0;
function check(cond, msg, extra) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (extra ? '\n      ' + extra : '')); }
}

function walk(dir, out) {
  for (const e of readdirSync(dir)) {
    if (e === 'node_modules' || e === '.git') continue;
    const p = join(dir, e);
    const st = statSync(p);
    if (st.isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}

// ---- 1. 后端路由 ----
const backend = new Set();
const backendFiles = walk(join(ROOT, 'server', 'gateway'), [])
  .filter(f => /\.(cpp|h)$/.test(f));
const routeRe = /\.(GET|POST|PUT|DELETE|PATCH)\(\s*"([^"]+)"/g;
for (const f of backendFiles) {
  const src = readFileSync(f, 'utf8');
  let m;
  while ((m = routeRe.exec(src)) !== null) backend.add(m[2]);
}

// ---- 2. 前端调用路径 ----
const frontend = new Set();
const feFiles = [
  ...walk(join(ROOT, 'web', 'js', 'core'), []),
  ...walk(join(ROOT, 'web', 'js', 'views'), []),
].filter(f => f.endsWith('.js'));
const pathRe = /'(\/api\/[^']*)'|"((\/api\/)[^"]*)"/g;
for (const f of feFiles) {
  const src = readFileSync(f, 'utf8');
  let m;
  while ((m = pathRe.exec(src)) !== null) {
    const raw = m[1] || m[2];
    frontend.add(raw.split('?')[0]);   // 截断查询串
  }
}

const backendApi = [...backend].filter(r => r.startsWith('/api/')).sort();
const backendNonApi = [...backend].filter(r => !r.startsWith('/api/')).sort();
const frontendPaths = [...frontend].sort();

console.log('route 契约测试 (前端调用路径 <-> 后端注册路由)');
check(backend.size > 0, '后端解析到路由', '(' + backend.size + ' 条, 扫描 ' + backendFiles.length + ' 个文件)');
check(frontend.size > 0, '前端解析到 API 路径', '(' + frontend.size + ' 条, 扫描 ' + feFiles.length + ' 个文件)');

// ---- 不变量 1: 前端调用的路径后端必须已注册 ----
const notRegistered = frontendPaths.filter(p => !backend.has(p));
check(notRegistered.length === 0,
  '前端所有调用路径后端均已注册 (无静默 404)',
  notRegistered.length ? '未注册: ' + notRegistered.join(', ') : '');

// ---- 不变量 2: 后端 /api 路由必须有前端调用方 (无死端点) ----
const deadRoutes = backendApi.filter(r => !frontend.has(r));
check(deadRoutes.length === 0,
  '后端所有 /api 路由均有前端调用方 (无死端点)',
  deadRoutes.length ? '无调用方: ' + deadRoutes.join(', ') : '');

// ---- 不变量 3: 非 /api 路由白名单 ----
const NON_API_ALLOWLIST = ['/', '/app', '/healthz', '/metrics', '/share.html'];
const unexpectedNonApi = backendNonApi.filter(r => !NON_API_ALLOWLIST.includes(r));
check(unexpectedNonApi.length === 0,
  '后端非 /api 路由均在白名单内 (页面/运维端点)',
  unexpectedNonApi.length ? '意外: ' + unexpectedNonApi.join(', ') : '');
const missingAllow = NON_API_ALLOWLIST.filter(r => !backend.has(r));
check(missingAllow.length === 0,
  '白名单端点均确实存在',
  missingAllow.length ? '缺失: ' + missingAllow.join(', ') : '');

// ---- 守卫自检 ----
check(!backend.has('/api/this_route_does_not_exist'),
  '守卫自检: 不存在的路由不会被误判为已注册');

console.log('\n后端: ' + backend.size + ' 条路由 (其中 /api ' + backendApi.length + ' 条, 非 API ' + backendNonApi.length + ' 条)');
console.log('前端: ' + frontendPaths.length + ' 条 API 调用路径');
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
