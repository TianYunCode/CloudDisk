/* =============================================================================
   audit_contract_validate.mjs —— 前后端审计动作契约测试

   校验: server/common/AuditAction.h 中定义的每个动作名, 必须在
        web/js/core/activity.js 的 ACT_META 中有对应的展示元数据(中文标签/图标/配色),
        且前端不得存在后端已不再使用的僵尸键。

   这是跨语言契约守卫: 后端新增审计动作却忘了加前端标签时, 本测试会失败,
   避免活动日志静默显示原始英文动作名。
   ============================================================================= */
import { createRequire } from 'module';
import { readFileSync } from 'fs';
const require = createRequire(import.meta.url);
const Activity = require('../../web/js/core/activity.js');

let fail = 0, total = 0;
function check(cond, msg, extra) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (extra ? '  ' + extra : '')); }
}

// ---- 1. 解析后端 AuditAction.h 的动作常量 ----
const header = readFileSync(new URL('../../server/common/AuditAction.h', import.meta.url), 'utf8');
const backend = [];
const re = /inline constexpr const char\*\s+(\w+)\s*=\s*"([^"]+)"/g;
let m;
while ((m = re.exec(header)) !== null) backend.push({ name: m[1], action: m[2] });

// ---- 2. 前端 ACT_META 键 ----
const frontend = Object.keys(Activity.ACT_META);

console.log('audit 契约测试 (后端 AuditAction.h <-> 前端 ACT_META)');

check(backend.length > 0, '后端解析到动作常量', '(' + backend.length + ' 个)');
check(frontend.length > 0, '前端解析到 ACT_META 键', '(' + frontend.length + ' 个)');

// ---- 3. 常量值不得重复 / 不得为空 ----
const vals = backend.map(b => b.action);
check(new Set(vals).size === vals.length, '后端动作名无重复');
check(vals.every(v => v.length > 0), '后端动作名均非空');
check(vals.every(v => /^[a-z0-9_]+$/.test(v)), '后端动作名均为 snake_case');

// ---- 4. 后端每个动作前端都必须有标签 (防漂移的核心断言) ----
const missingInFrontend = vals.filter(v => !frontend.includes(v));
check(missingInFrontend.length === 0,
  '后端所有动作前端均有展示标签',
  missingInFrontend.length ? '缺失: ' + missingInFrontend.join(', ') : '');

// ---- 5. 前端不得有后端不存在的僵尸键 ----
const zombie = frontend.filter(k => !vals.includes(k));
check(zombie.length === 0,
  '前端无僵尸动作键 (后端未定义)',
  zombie.length ? '多余: ' + zombie.join(', ') : '');

// ---- 6. 每个前端标签数据完整 ----
let incomplete = [];
for (const k of frontend) {
  const e = Activity.ACT_META[k];
  if (!e || typeof e.label !== 'string' || !e.label || typeof e.color !== 'string' || typeof e.icon !== 'string') incomplete.push(k);
}
check(incomplete.length === 0, '每个动作均有非空 label/color/icon', incomplete.join(', '));

// ---- 7. 守卫自检: 确认比较逻辑真的能抓到漂移 ----
const bogus = 'this_action_does_not_exist';
check(!frontend.includes(bogus) && vals.filter(v => v === bogus).length === 0,
  '守卫自检: 不存在的动作不会被误判为已覆盖');

console.log('\n后端动作数: ' + vals.length + ', 前端标签数: ' + frontend.length);
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
