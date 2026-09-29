import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const Bus = require('../../web/js/core/bus.js');

let fail = 0, total = 0;
function check(cond, msg) {
  total++;
  if (cond) { console.log('  \u2713 ' + msg); }
  else { fail++; console.log('  \u2717 ' + msg); }
}

console.log('bus.js 单元测试 (事件总线 / Observer-Mediator)');
Bus.clear();

// 基本订阅/发布
let got = [];
const off1 = Bus.on('e1', p => got.push('A:' + p));
const off2 = Bus.on('e1', p => got.push('B:' + p));
check(Bus.emit('e1', 1) === 2, 'emit 返回被调用的 handler 数量');
check(got.join(',') === 'A:1,B:1', '两个订阅者均收到');

// 多事件隔离
let other = 0;
Bus.on('e2', () => other++);
Bus.emit('e1', 2);
check(other === 0, '不同事件不互相干扰');
check(got.length === 4, 'e1 两次 emit 共 4 次分发');

// 退订
off1();
Bus.emit('e1', 3);
check(got.filter(s => s.startsWith('A:')).length === 2, '退订后不再收到');
check(Bus.listenerCount('e1') === 1, 'listenerCount 反映退订');

// once 只触发一次
let onceCount = 0;
Bus.once('e3', () => onceCount++);
Bus.emit('e3'); Bus.emit('e3');
check(onceCount === 1, 'once 仅触发一次');

// 一个 handler 抛错不影响其它
let okRan = 0;
Bus.on('e4', () => { throw new Error('boom'); });
Bus.on('e4', () => okRan++);
Bus.emit('e4');
check(okRan === 1, 'handler 抛错不阻断其它订阅者');

// 退订句柄返回值
check(typeof off1 === 'function', 'on() 返回退订函数');

// clear
Bus.clear('e5');  // 不存在的事件不报错
Bus.on('e6', () => {});
check(Bus.listenerCount('e6') === 1, '订阅后 listenerCount=1');
Bus.clear('e6');
check(Bus.listenerCount('e6') === 0, 'clear(event) 清空指定事件');
Bus.clear();
check(Bus.listenerCount('e1') === 0 && Bus.listenerCount('e3') === 0, 'clear() 清空全部');

// 非法参数
let threw = false;
try { Bus.on('e7', 'not-a-function'); } catch (e) { threw = true; }
check(threw, 'on 非函数 handler 抛 TypeError');

console.log('\n结果: ' + (total - fail) + ' 通过, ' + fail + ' 失败 (共 ' + total + ')');
process.exit(fail === 0 ? 0 : 1);
