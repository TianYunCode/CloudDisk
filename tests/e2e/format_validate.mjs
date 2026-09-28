import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const Fmt = require('../../web/js/core/format.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg); }
}

console.log('format.js 单元测试 (纯逻辑)');
check(typeof Fmt.fileType === 'function', '导出 fileType');
check(Fmt.Icons && typeof Fmt.Icons.FOLDER === 'string', '导出 Icons.FOLDER');
check(Fmt.Icons.SHARE && Fmt.Icons.FILE, '导出 Icons.SHARE / FILE');

check(Fmt.fileType('a.png').color === '#ec4899', 'png 归类为 image (粉色)');
check(Fmt.fileType('IMG.JPG').color === '#ec4899', '大写扩展名归类正确');
check(Fmt.fileType('doc.pdf').color === '#ef4444', 'pdf 归类 (红色)');
check(Fmt.fileType('main.cpp').color === '#6366f1', 'cpp 归类为 code');
check(Fmt.fileType('data.zip').color === '#a855f7', 'zip 归类');
check(Fmt.fileType('sheet.xlsx').color === '#10b981', 'xlsx 归类为 sheet');
check(Fmt.fileType('noext').color === '#9aa0b8', '未知类型回退灰色');
check(Fmt.fileType('noext').icon === Fmt.Icons.FILE, '未知类型使用通用文件图标');
check(typeof Fmt.fileType('x.png').icon === 'string' && Fmt.fileType('x.png').icon.length > 0, '返回非空 icon');

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
