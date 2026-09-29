/* =============================================================================
   bus.js —— 极简事件总线 (Observer / Mediator)

   用于把"数据变更后刷新多个视图"这类跨视图耦合改为发布/订阅:
     · 视图/控制器 订阅 (on) 关心的事件
     · 发布者只 emit 事件名, 不知道谁在监听
     · 订阅函数返回退订句柄, 防止组件销毁后残留

   纯逻辑、无 DOM 依赖、线程安全的 JS 语义 (同一事件循环内同步分发)。

   · 浏览器: 挂载到 window.CV.Bus
   · Node  : module.exports, 供单元测试 (tests/e2e/bus_validate.mjs)
   ============================================================================= */
(function (root) {
  'use strict';

  const handlers = new Map();   // event -> Set<fn>

  function on(event, fn) {
    if (typeof fn !== 'function') throw new TypeError('handler must be a function');
    if (!handlers.has(event)) handlers.set(event, new Set());
    handlers.get(event).add(fn);
    // 返回退订句柄
    return function off() { remove(event, fn); };
  }

  function once(event, fn) {
    const off = on(event, function wrapper(payload) {
      off();
      fn(payload);
    });
    return off;
  }

  function remove(event, fn) {
    const set = handlers.get(event);
    if (!set) return false;
    const ok = set.delete(fn);
    if (set.size === 0) handlers.delete(event);
    return ok;
  }

  function emit(event, payload) {
    const set = handlers.get(event);
    if (!set) return 0;
    // 拷贝一份快照再分发, 允许 handler 内部退订或新增
    let n = 0;
    for (const fn of Array.from(set)) {
      try {
        fn(payload);
        n++;
      } catch (e) {
        // 一个订阅者抛错不影响其它订阅者
        if (typeof console !== 'undefined' && console.error) console.error('[Bus] handler error in "' + event + '":', e);
      }
    }
    return n;
  }

  function clear(event) {
    if (event === undefined) handlers.clear();
    else handlers.delete(event);
  }

  function listenerCount(event) {
    const set = handlers.get(event);
    return set ? set.size : 0;
  }

  const Bus = { on, once, remove, emit, clear, listenerCount };
  if (typeof module !== 'undefined' && module.exports) module.exports = Bus;
  if (root) { root.CV = root.CV || {}; root.CV.Bus = Bus; }
})(typeof window !== 'undefined' ? window : null);
