/* =============================================================================
   config_contract_validate.mjs —— 配置契约测试

   配置读取优先级为 env > config.json > 内置默认 (见 server/config/Config.h)。
   这类"三处来源"最容易漂移: 改了 config.json 却不生效、容器里设了环境变量却没人读。

   校验的不变量:
     1. config.json 的每个键都被 Config.h 真正读取      → 防止无效/拼错的配置项
     2. Config.h 读取的环境变量名唯一且为 SCREAMING_SNAKE → 防止命名混乱/覆盖冲突
     3. docker-compose 的 app 服务注入的每个环境变量,
        要么被 Config.h 读取, 要么被 docker/entrypoint.sh 消费 → 防止孤立变量
     4. 未在 config.json 出现的配置项必须有内置默认值     → 防止启动即崩
     5. 密钥卫生: 仓库内的 config.json 不得包含真实密钥
        (jwt_secret 必须仍是占位符, 真实值只能通过环境变量注入)
   ============================================================================= */
import { readFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = p => readFileSync(join(ROOT, p), 'utf8');

let fail = 0, total = 0;
function check(cond, msg, extra) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg + (extra ? '\n      ' + extra : '')); }
}

// ---- 1. Config.h 的 (json键, 环境变量, 是否有默认值) 三元组 ----
const configH = read('server/config/Config.h');
const accessors = [];
const accRe = /get_(str|int)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*([^)]*)\)/g;
let m;
while ((m = accRe.exec(configH)) !== null) {
  accessors.push({ kind: m[1], key: m[2], env: m[3], def: m[4].trim() });
}
const cfgKeys = accessors.map(a => a.key);
const cfgEnvs = accessors.map(a => a.env);

// ---- 2. config.json 实际提供的键 ----
const configJson = JSON.parse(read('config.json'));
const jsonKeys = Object.keys(configJson);

// ---- 3. docker-compose app 服务注入的环境变量 ----
const compose = read('docker-compose.yml');
const appEnv = [];
{
  const lines = compose.split('\n');
  let inApp = false, inEnv = false;
  for (const ln of lines) {
    if (/^ {2}app:\s*$/.test(ln)) { inApp = true; continue; }
    if (inApp && /^ {2}\S/.test(ln)) { inApp = false; inEnv = false; }   // 下一个顶层服务
    if (!inApp) continue;
    if (/^ {4}environment:\s*$/.test(ln)) { inEnv = true; continue; }
    if (inEnv) {
      const km = ln.match(/^ {6}([A-Z][A-Z0-9_]*):/);
      if (km) appEnv.push(km[1]);
      else if (/^ {4}\S/.test(ln)) inEnv = false;   // environment 块结束
    }
  }
}

// ---- 4. entrypoint.sh 消费的变量 ----
const entrypoint = read('docker/entrypoint.sh');
const entryEnvs = new Set();
let em; const eRe = /\$\{?([A-Z][A-Z0-9_]*)/g;
while ((em = eRe.exec(entrypoint)) !== null) entryEnvs.add(em[1]);

console.log('config 契约测试 (env > config.json > 默认)');
check(accessors.length > 0, 'Config.h 解析到配置访问器', '(' + accessors.length + ' 项)');
check(jsonKeys.length > 0, 'config.json 解析到键', '(' + jsonKeys.length + ' 项)');

// ---- 不变量 1: config.json 的键必须被真正读取 ----
const ignored = jsonKeys.filter(k => !cfgKeys.includes(k));
check(ignored.length === 0,
  'config.json 每个键都被 Config.h 读取 (无无效配置项)',
  ignored.length ? '被忽略: ' + ignored.join(', ') : '');

// ---- 不变量 2: 环境变量名唯一且规范 ----
check(new Set(cfgEnvs).size === cfgEnvs.length, '环境变量名无重复');
check(cfgEnvs.every(e => /^[A-Z][A-Z0-9_]*$/.test(e)), '环境变量名均为 SCREAMING_SNAKE');
check(new Set(cfgKeys).size === cfgKeys.length, 'JSON 键名无重复');

// ---- 不变量 3: compose 注入的变量必须有人消费 ----
const orphan = appEnv.filter(v => !cfgEnvs.includes(v) && !entryEnvs.has(v));
check(appEnv.length > 0, 'compose app 服务解析到环境变量', '(' + appEnv.join(', ') + ')');
check(orphan.length === 0,
  'compose 注入的每个变量都被 Config.h 或 entrypoint.sh 消费 (无孤立变量)',
  orphan.length ? '孤立: ' + orphan.join(', ') : '');

// ---- 不变量 4: 未出现在 config.json 的项必须有默认值 ----
const optional = accessors.filter(a => !jsonKeys.includes(a.key));
const noDefault = optional.filter(a => a.def === '' || a.def === undefined);
check(noDefault.length === 0,
  '未在 config.json 出现的配置项均有内置默认值',
  noDefault.length ? '缺默认: ' + noDefault.map(a => a.key).join(', ') : '');

// ---- 不变量 5: 密钥卫生 ----
const PLACEHOLDER = 'change-me-to-a-long-random-secret-please';
check(configJson.jwt_secret === PLACEHOLDER,
  'config.json 的 jwt_secret 仍是占位符 (未提交真实密钥)',
  '实际值: ' + String(configJson.jwt_secret).slice(0, 12) + '…');
check(!/ghp_[A-Za-z0-9]{20,}/.test(configJson.jwt_secret), 'jwt_secret 不含 GitHub PAT 形态字符串');
const secretsInJson = JSON.stringify(configJson);
check(!/AKID|LTAI[A-Za-z0-9]{10,}|-----BEGIN/.test(secretsInJson),
  'config.json 不含云 AccessKey / 私钥形态字符串');

console.log('\nConfig.h 配置项: ' + cfgKeys.length + ' 个 (env 覆盖: ' + cfgEnvs.length + ' 个)');
console.log('config.json 提供: ' + jsonKeys.length + ' 个');
console.log('可选项 (走默认值): ' + (optional.map(a => a.key).join(', ') || '无'));
console.log('结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
