/* =============================================================================
   humansize_contract_validate.mjs —— 字节格式化跨语言一致性契约

   后端 server/util/FormatUtil.h 的 human_size() 与前端 web/js/core/api.js 的
   humanSize() 必须对同一字节数产出**完全相同**的字符串; 否则同一个文件在页面
   列表里显示 "5.0 MB"、在配额报错里却显示 "5.0 MB " 或 "5 MB", 用户会困惑。

   做法: 现场编译一个 C++ 探针 (仅 include FormatUtil.h), 让它对一批数值输出
   结果, 再与前端实现逐值比对。若环境无 g++ 则跳过跨语言部分 (仍跑 JS 自检)。
   ============================================================================= */
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'fs';
import { execFileSync } from 'child_process';
import { join, dirname } from 'path';
import { tmpdir } from 'os';
import { createRequire } from 'module';
import { fileURLToPath } from 'url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const require_ = createRequire(import.meta.url);

let fail = 0, total = 0, skipped = 0;
function check(cond, msg, extra) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (extra ? '\n      ' + extra : '')); }
}

// ---- 前端实现 (与被测页面同一份代码) ----
const Api = require_(join(ROOT, 'web/js/core/api.js'));
const jsHuman = Api.humanSize;
check(typeof jsHuman === 'function', '前端 humanSize 可加载');

// ---- 测试数值: 覆盖各单位段、取整边界与异常输入 ----
const VALUES = [
  -1048576, -1, 0, 1, 100, 512, 1023,
  1024, 1025, 1536, 10240, 101376, 102400, 153600, 1048575,
  1048576, 1572864, 5242880, 104857600, 1073741823,
  1073741824, 1610612736, 107374182400, 1099511627775,
  1099511627776, 10995116277760, 109951162777600, 1099511627776000,
];

// ---- JS 侧自检: 结构必须合理 ----
const jsOut = VALUES.map(v => jsHuman(v));
const SHAPE = /^-?\d+(\.\d+)? (B|KB|MB|GB|TB)$/;
check(jsOut.every(s => SHAPE.test(s)),
  '前端输出格式统一为 "[-]<数> <单位>"',
  jsOut.find(s => !SHAPE.test(s)) || '');
// 负数必须带符号且幅值同样做单位换算 (不能原样输出 "-1048576 B")
check(jsHuman(-1048576) === '-1.0 MB', '前端负数按「符号+幅值」换算 (-1MiB -> -1.0 MB)', jsHuman(-1048576));
check(jsHuman(-1) === '-1 B', '前端 -1 -> -1 B');
check(jsHuman(1023) === '1023 B' && jsHuman(1024) === '1.0 KB', '前端 1KB 进位边界正确');

// ---- 编译 C++ 探针 ----
let cppAvailable = true, cppOut = [];
const dir = mkdtempSync(join(tmpdir(), 'humansize-'));
try {
  const probe = join(dir, 'probe.cpp');
  writeFileSync(probe, [
    '#include <cstdio>',
    '#include <cstdlib>',
    '#include "FormatUtil.h"',
    'int main(int argc, char** argv) {',
    '  for (int i = 1; i < argc; ++i)',
    '    std::printf("%s\\n", fmtutil::human_size(std::atoll(argv[i])).c_str());',
    '  return 0;',
    '}',
  ].join('\n'));
  const bin = join(dir, 'probe');
  execFileSync('g++', ['-std=c++17', '-O0', '-I', join(ROOT, 'server/util'), probe, '-o', bin],
    { stdio: 'pipe' });
  const res = execFileSync(bin, VALUES.map(String), { encoding: 'utf8' });
  cppOut = res.split('\n').filter(l => l.length > 0);
} catch (e) {
  cppAvailable = false;
  console.log('  ! 无法编译 C++ 探针 (缺少 g++?), 跳过跨语言比对: ' + (e.message || e).toString().split('\n')[0]);
} finally {
  try { rmSync(dir, { recursive: true, force: true }); } catch (e) { /* 忽略清理失败 */ }
}

if (cppAvailable) {
  check(cppOut.length === VALUES.length,
    'C++ 探针输出行数与测试值数量一致',
    cppOut.length + ' vs ' + VALUES.length);

  const mismatches = [];
  for (let i = 0; i < VALUES.length; i++) {
    if (cppOut[i] !== jsOut[i]) mismatches.push(VALUES[i] + ': C++="' + cppOut[i] + '" JS="' + jsOut[i] + '"');
  }
  check(mismatches.length === 0,
    '后端 human_size 与前端 humanSize 对全部 ' + VALUES.length + ' 个数值输出完全一致',
    mismatches.slice(0, 6).join('\n      '));

  // 单位序列必须一致 (防止一端多加/少加一档单位)
  const units = s => s.split(' ')[1];
  const cppUnits = cppOut.map(units), jsUnits = jsOut.map(units);
  check(JSON.stringify(cppUnits) === JSON.stringify(jsUnits), '两端单位档位序列一致');
  skipped = 0;
} else {
  skipped = 3;
}

console.log('\n比对数值: ' + VALUES.length + ' 个 (含负数/0/各单位段/取整边界)');
if (cppAvailable) {
  console.log('样例: 1023->' + jsOut[VALUES.indexOf(1023)] +
    '  1536->' + jsOut[VALUES.indexOf(1536)] +
    '  101376->' + jsOut[VALUES.indexOf(101376)] +
    '  10995116277760->' + jsOut[VALUES.indexOf(10995116277760)]);
}
const note = skipped ? ' (跳过 ' + skipped + ' 项跨语言比对)' : '';
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')' + note);
process.exit(fail === 0 ? 0 : 1);
