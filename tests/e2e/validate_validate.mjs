import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const V = require('../../web/js/core/validate.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg); }
}

console.log('validate.js 单元测试 (前后端规则一致性)');

// 用户名: 3-32 位 字母/数字/_/-  (对齐后端 SqlUtil::valid_username)
check(V.validUsername('abc'), '3 位合法');
check(V.validUsername('a_b-9'), '含 _ 和 - 合法');
check(V.validUsername('A'.repeat(32)), '32 位边界合法');
check(!V.validUsername('ab'), '2 位过短被拒');
check(!V.validUsername('A'.repeat(33)), '33 位过长被拒');
check(!V.validUsername('bad name'), '含空格被拒');
check(!V.validUsername('中文名'), '非 ASCII 被拒');
check(!V.validUsername(''), '空串被拒');

// 密码: 6-64 位 (对齐后端 SqlUtil::valid_password)
check(!V.validPassword('12345'), '5 位过短被拒');
check(V.validPassword('123456'), '6 位边界合法');
check(V.validPassword('x'.repeat(64)), '64 位边界合法');
check(!V.validPassword('x'.repeat(65)), '65 位过长被拒');

// registerError 组合校验
check(V.registerError('', 'abcdef') === '请输入用户名和密码', '缺用户名报错');
check(V.registerError('ab', 'abcdef').includes('用户名'), '非法用户名报错');
check(V.registerError('alice', '123').includes('密码'), '弱密码报错');
check(V.registerError('alice', 'abcdef', 'xxxxxx').includes('不一致'), '两次密码不一致报错');
check(V.registerError('alice', 'abcdef', 'abcdef') === '', '合法输入无错误');

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
