/* =============================================================================
   validate_contract_validate.mjs —— 输入校验规则跨语言一致性契约

   用户名/密码规则在前端 (web/js/core/validate.js) 与后端 (server/util/SqlUtil.h)
   各实现一份。历史上两端计量单位不同 —— 后端按 UTF-8 **字节数**, 前端按 UTF-16
   **码元数** —— 导致中文/emoji 密码判定相反:
     · "密码"        (2 字符 / 6 字节 / 2 码元)  后端放行, 前端拦截
     · 33 个汉字      (33 字符 / 99 字节)        前端放行, 后端 400 拒绝
     · "🔒🔒🔒"       (3 字符 / 6 码元)          两端都放行 → 实际只有 3 个字符,
                                                绕过了"至少 6 位"的强度下限

   现两端统一按 **Unicode 码点**计数。本测试现场编译 C++ 探针, 对一批输入逐值
   比对两端判定, 任何再次漂移都会失败。
   ============================================================================= */
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'fs';
import { execFileSync } from 'child_process';
import { join, dirname } from 'path';
import { tmpdir } from 'os';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const require_ = createRequire(import.meta.url);

let fail = 0, total = 0;
function check(cond, msg, extra) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (extra ? '\n      ' + extra : '')); }
}

const V = require_(join(ROOT, 'web/js/core/validate.js'));

// ---- 测试输入: 边界 / 多字节 / emoji / 非法字符 / 空 ----
const PASSWORDS = [
  '', '12345', '123456', '1234567',
  'x'.repeat(63), 'x'.repeat(64), 'x'.repeat(65),
  '密码', '密码啊', '密码啊啊',            // 2/3/4 个汉字
  '中文密码六个字',                        // 恰好 6 个汉字 -> 合法
  '中文密码五个字呀',                      // 7 个汉字
  '一二三四五六七八九十一二三四五六七八九十一一二三四五六七八九十一二',   // 33 汉字
  '汉'.repeat(64), '汉'.repeat(65),        // 码点上下边界
  '🔒🔒🔒', '🔒🔒🔒🔒🔒🔒',                // 3 / 6 个 emoji (代理对)
  '🔒'.repeat(64), '🔒'.repeat(65),
  'abc密码12', 'a🔒b🔒c🔒',                // 混合
  'a'.repeat(300),                         // 超长
];
const USERNAMES = [
  '', 'ab', 'abc', 'abcd',
  'a'.repeat(32), 'a'.repeat(33),
  'user_01', 'user-01', 'USER', 'u1_',
  'bad name', 'bad!name', 'user@x', '用户名字',
  'a_b-c'.repeat(10),
];

// ---- 编译 C++ 探针 ----
const dir = mkdtempSync(join(tmpdir(), 'validate-'));
let cpp = null;
try {
  const probe = join(dir, 'probe.cpp');
  // 输入通过 argv 传入 (UTF-8 原样), 输出 "<密码判定> <用户名判定> <码点数>" 每行一组
  writeFileSync(probe, [
    '#include <cstdio>',
    '#include <cstring>',
    '#include <string>',
    '#include "SqlUtil.h"',
    'int main(int argc, char** argv) {',
    '  const char* mode = argv[1];',
    '  for (int i = 2; i < argc; ++i) {',
    '    std::string s = argv[i];',
    '    if (std::strcmp(mode, "pw") == 0)',
    '      std::printf("%d %zu\\n", SqlUtil::valid_password(s) ? 1 : 0, SqlUtil::utf8_length(s));',
    '    else',
    '      std::printf("%d\\n", SqlUtil::valid_username(s) ? 1 : 0);',
    '  }',
    '  return 0;',
    '}',
  ].join('\n'));
  const bin = join(dir, 'probe');
  execFileSync('g++', ['-std=c++17', '-O0', '-I', join(ROOT, 'server/util'), probe, '-o', bin], { stdio: 'pipe' });
  cpp = bin;
} catch (e) {
  console.log('  ! 无法编译 C++ 探针 (缺少 g++?): ' + (e.message || e).toString().split('\n')[0]);
}

if (!cpp) {
  console.log('\n结果: 跳过 (无编译器, 无法做跨语言比对)');
  process.exit(0);
}

function runProbe(mode, values) {
  // 空串是合法的 argv 元素, C++ 侧收到 "" , 无需哨兵
  const out = execFileSync(cpp, [mode, ...values], { encoding: 'utf8' });
  return out.split('\n').filter(l => l.length > 0);
}

// ---- 密码规则比对 ----
const pwLines = runProbe('pw', PASSWORDS);
check(pwLines.length === PASSWORDS.length, '密码探针输出行数一致', pwLines.length + ' vs ' + PASSWORDS.length);
const pwMismatch = [];
const cpMismatch = [];
for (let i = 0; i < PASSWORDS.length; i++) {
  const p = PASSWORDS[i];
  const parts = pwLines[i].split(' ');
  const beValid = parts[0] === '1';
  const beChars = Number(parts[1]);
  const feValid = (p === '' ? false : V.validPassword(p));
  if (beValid !== feValid) {
    pwMismatch.push(JSON.stringify(p.slice(0, 12)) + ' (len=' + p.length + '): C++=' + beValid + ' JS=' + feValid);
  }
  // 码点数也必须一致 (前端 charLength vs 后端 utf8_length)
  const feChars = V.charLength(p === '' ? '' : p);
  if (beChars !== feChars) cpMismatch.push(JSON.stringify(p.slice(0, 12)) + ': C++=' + beChars + ' JS=' + feChars);
}
check(pwMismatch.length === 0,
  '两端对全部 ' + PASSWORDS.length + ' 个密码输入判定完全一致 (含中文/emoji/边界)',
  pwMismatch.slice(0, 6).join('\n      '));
check(cpMismatch.length === 0,
  '两端码点计数完全一致 (SqlUtil::utf8_length vs Validate.charLength)',
  cpMismatch.slice(0, 6).join('\n      '));

// ---- 用户名规则比对 ----
const unLines = runProbe('un', USERNAMES);
check(unLines.length === USERNAMES.length, '用户名探针输出行数一致');
const unMismatch = [];
for (let i = 0; i < USERNAMES.length; i++) {
  const u = USERNAMES[i];
  const beValid = unLines[i].trim() === '1';
  const feValid = (u === '' ? false : V.validUsername(u));
  if (beValid !== feValid) unMismatch.push(JSON.stringify(u.slice(0, 12)) + ': C++=' + beValid + ' JS=' + feValid);
}
check(unMismatch.length === 0,
  '两端对全部 ' + USERNAMES.length + ' 个用户名输入判定完全一致',
  unMismatch.slice(0, 6).join('\n      '));

// ---- 语义锚定: 修复后的行为必须成立 ----
check(V.validPassword('密码') === false, '"密码" (2 字符) 不满足 6 字符下限');
check(V.validPassword('中文密码六个字') === true, '恰好 6 个汉字合法');
check(V.validPassword('🔒🔒🔒') === false, '"🔒🔒🔒" (3 字符) 不再因代理对被误判为 6 位');
check(V.validPassword('🔒🔒🔒🔒🔒🔒') === true, '6 个 emoji 合法');
check(V.validPassword('汉'.repeat(64)) === true, '64 个汉字合法 (码点上限)');
check(V.validPassword('汉'.repeat(65)) === false, '65 个汉字被拒');
check(V.validPassword('a'.repeat(300)) === false, '超长密码被拒');
// 旧的按字节/码元实现会给出相反答案的两个关键用例
check(V.charLength('🔒') === 1, '单个 emoji 计为 1 个字符 (非 2 个码元)');
check(V.charLength('汉') === 1, '单个汉字计为 1 个字符 (非 3 个字节)');

try { rmSync(dir, { recursive: true, force: true }); } catch (e) { /* 忽略 */ }

console.log('\n比对: 密码 ' + PASSWORDS.length + ' 例 + 用户名 ' + USERNAMES.length + ' 例 (含中文/emoji/代理对/边界/非法字符)');
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
