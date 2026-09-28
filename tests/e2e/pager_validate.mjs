import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const P = require('../../web/js/core/pager.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg); }
}

console.log('pager.js 单元测试 (分页纯逻辑)');

// pageCount
check(P.pageCount(0, 50) === 1, '0 项 -> 1 页');
check(P.pageCount(50, 50) === 1, '整除 -> 1 页');
check(P.pageCount(51, 50) === 2, '超 1 项 -> 2 页');
check(P.pageCount(120, 50) === 3, '120/50 -> 3 页');

// pageRange
check(P.pageRange(120, 0, 50).from === 1 && P.pageRange(120, 0, 50).to === 50, '第 1 页范围 1-50');
check(P.pageRange(120, 2, 50).from === 101 && P.pageRange(120, 2, 50).to === 120, '第 3 页范围 101-120');
check(P.pageRange(30, 0, 50).to === 30, '总数小于页大小时 to=总数');

// pageButtons
const btns = P.pageButtons(3, 1);
check(btns.includes('data-page="0"'), '含第 1 页按钮');
check(btns.includes('data-page="2"'), '含第 3 页按钮');
check(/btn-primary/.test(btns), '当前页高亮');
check((btns.match(/<button/g) || []).length === 3, '按钮数=页数');

// infoHtml
const info = P.infoHtml({ from: 51, to: 100 }, 120);
check(info.includes('51–100'), '显示范围');
check(info.includes('共 120 项'), '显示总数');

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
