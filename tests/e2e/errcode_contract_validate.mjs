/* =============================================================================
   errcode_contract_validate.mjs —— 前后端错误码契约测试

   统一响应信封为 {code, message, data}: code=0 表示成功, 非 0 为错误码。
   错误码分两类:
     · 标准 HTTP 语义码 (400/401/403/404/409/410/413/429/500/503) — 前端走通用
       消息提示即可, 无需特殊分支
     · 自定义业务码 (如 4012 = 需要两步验证码) — 前端必须有显式处理分支,
       否则用户会看到无法行动的原始提示

   校验的不变量:
     1. 后端发出的每个自定义业务码, 前端都有显式处理  → 防止"发了没人接"
     2. 前端显式比较的每个码, 后端确实会发出            → 防止死分支
     3. code=0 是唯一的成功哨兵, 两端一致使用
     4. 后端错误码全部为正整数
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
    if (statSync(p).isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}

// 具备标准 HTTP 语义、前端可用通用提示覆盖的码
const STANDARD_HTTP = new Set([400, 401, 403, 404, 409, 410, 413, 429, 500, 503]);

// ---- 1. 后端发出的业务码 (api::fail 的第 3 个参数) ----
const backendCodes = new Set();
const beFiles = walk(join(ROOT, 'server', 'gateway'), []).filter(f => /\.(cpp|h)$/.test(f));
const failRe = /api::fail\(\s*[^,]+,\s*(\d+)\s*,\s*(\d+)\s*,/g;
for (const f of beFiles) {
  const src = readFileSync(f, 'utf8');
  let m;
  while ((m = failRe.exec(src)) !== null) backendCodes.add(Number(m[2]));
}

// ---- 2. 前端显式比较的码 ----
const frontendCodes = new Set();
const feFiles = [
  ...walk(join(ROOT, 'web', 'js', 'core'), []),
  ...walk(join(ROOT, 'web', 'js', 'views'), []),
].filter(f => f.endsWith('.js'));
const cmpRe = /\.code\s*[=!]==?\s*(\d+)/g;
for (const f of feFiles) {
  const src = readFileSync(f, 'utf8');
  let m;
  while ((m = cmpRe.exec(src)) !== null) frontendCodes.add(Number(m[1]));
}

const beList = [...backendCodes].sort((a, b) => a - b);
const feList = [...frontendCodes].sort((a, b) => a - b);
const customBackend = beList.filter(c => !STANDARD_HTTP.has(c) && c !== 0);

console.log('errcode 契约测试 (前后端错误码)');
check(beList.length > 0, '后端解析到业务码', '(' + beList.join(', ') + ')');
check(feList.length > 0, '前端解析到显式比较的码', '(' + feList.join(', ') + ')');

// ---- 不变量 4: 后端码均为正整数 ----
check(beList.every(c => Number.isInteger(c) && c > 0), '后端业务码均为正整数');

// ---- 不变量 1: 自定义业务码必须被前端显式处理 ----
const unhandled = customBackend.filter(c => !frontendCodes.has(c));
check(unhandled.length === 0,
  '后端每个自定义业务码前端均有显式处理分支',
  unhandled.length ? '未处理: ' + unhandled.join(', ') : '(自定义码: ' + (customBackend.join(', ') || '无') + ')');

// ---- 不变量 2: 前端比较的码后端确实会发出 (0 为成功哨兵, 除外) ----
const deadBranch = feList.filter(c => c !== 0 && !backendCodes.has(c));
check(deadBranch.length === 0,
  '前端显式比较的每个码后端均会发出 (无死分支)',
  deadBranch.length ? '死分支: ' + deadBranch.join(', ') : '');

// ---- 不变量 3: code=0 为唯一成功哨兵 ----
check(frontendCodes.has(0), '前端使用 code=0 判定成功');
check(!backendCodes.has(0), '后端错误路径从不发出 code=0 (0 专属成功)');

// ---- 守卫自检 ----
check(!frontendCodes.has(9999), '守卫自检: 不存在的码不会被误判为已处理');

console.log('\n后端业务码: ' + beList.join(', '));
console.log('其中自定义业务码: ' + (customBackend.join(', ') || '无'));
console.log('前端显式处理: ' + feList.join(', '));
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
