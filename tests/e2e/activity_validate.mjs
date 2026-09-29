import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const A = require('../../web/js/core/activity.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (arguments[2] !== undefined ? '  <' + arguments[2] + '>' : '')); }
}

console.log('activity.js 单元测试 (活动动作映射)');

// 已知动作应映射到预期 label/color/icon
check(A.actMeta('login').label === '登录', 'login 标签');
check(A.actMeta('login').color === '#10b981', 'login 颜色');
check(A.actMeta('login_fail').color === '#ef4444', 'login_fail 红色');
check(A.actMeta('file_upload').label === '上传', 'file_upload 标签');
check(A.actMeta('2fa_enable').label === '启用两步验证', '2fa_enable 标签 (数字开头 key)');
check(A.actMeta('token_revoke').label === '吤销令牌' || A.actMeta('token_revoke').label === '吊销令牌', 'token_revoke 标签');

// 未知动作: 沿用原 action 名 + 灰色回退
const unk = A.actMeta('unknown_action');
check(unk.label === 'unknown_action', '未知动作保留原名');
check(unk.color === '#94a3b8', '未知动作回退灰色');
check(typeof unk.icon === 'string' && unk.icon.length > 0, '未知动作有图标');

// 数据完整性: 每个已知动作都有 label & color & icon
let allOk = true;
for (const k in A.ACT_META) {
  const m = A.ACT_META[k];
  if (typeof m.label !== 'string' || typeof m.color !== 'string' || typeof m.icon !== 'string') { allOk = false; break; }
}
check(allOk, '所有已知动作均含 label/color/icon');

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
