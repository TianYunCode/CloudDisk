import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const C = require('../../web/js/core/crumbs.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (arguments[2] !== undefined ? '  <' + arguments[2] + '>' : '')); }
}

console.log('crumbs.js 单元测试 (面包屑业务规则)');

// buildCrumbs: 根目录永远前置
check(C.buildCrumbs([])[0].id === 0, '空路径前置根目录 id=0');
check(C.buildCrumbs([])[0].name === '根目录', '空路径前置根目录 name');
check(C.buildCrumbs([]).length === 1, '空路径仅 1 项 (根目录)');

// 真实路径
const c1 = C.buildCrumbs([{ id: 10, name: '文档' }]);
check(c1.length === 2, '一段路径 -> 2 项');
check(c1[0].id === 0 && c1[1].id === 10, '顺序: 根 -> 文档');

const c2 = C.buildCrumbs([{ id: 10, name: '文档' }, { id: 11, name: '2024' }]);
check(c2.length === 3, '两段路径 -> 3 项');
check(c2[0].id === 0 && c2[1].id === 10 && c2[2].id === 11, '顺序: 根 -> 文档 -> 2024');
check(c2[2].name === '2024', '末段名字保留');

// 缺省 / 非法输入
check(C.buildCrumbs(null).length === 1, 'null 入参 -> 仅根目录');
check(C.buildCrumbs(undefined).length === 1, 'undefined 入参 -> 仅根目录');
check(C.buildCrumbs('bad').length === 1, '非数组入参 -> 仅根目录');

// ROOT 常量
check(C.ROOT.id === 0 && C.ROOT.name === '根目录', 'ROOT 常量');

// isLast
const c3 = C.buildCrumbs([{ id: 10, name: 'A' }, { id: 11, name: 'B' }]);
check(C.isLast(c3, 2) === true, '末位 isLast=true');
check(C.isLast(c3, 0) === false, '首位 isLast=false');
check(C.isLast(c3, 1) === false, '中间 isLast=false');

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
