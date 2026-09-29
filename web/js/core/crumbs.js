/* =============================================================================
   crumbs.js —— 面包屑路径构造 (纯业务规则, 无 DOM 依赖)

   业务规则: 根目录永远前置, 面包屑 = [{id:0, name:'根目录'}, ...state.breadcrumb]
   纯函数、可在 Node 单测, 与 DOM 渲染解耦。

   · 浏览器: 挂载到 window.CV.Crumbs
   · Node  : module.exports, 供单元测试 (tests/e2e/crumbs_validate.mjs)
   ============================================================================= */
(function (root) {
  'use strict';

  const ROOT = { id: 0, name: '根目录' };

  // 构造面包屑数据: 根目录永远前置, 后面跟当前路径
  // breadcrumb 为 [{id, name}, ...], 允许缺省/null
  function buildCrumbs(breadcrumb) {
    const path = Array.isArray(breadcrumb) ? breadcrumb : [];
    return [ROOT].concat(path);
  }

  // 判断某一段是否是当前最后一段 (面包屑 UI 用)
  function isLast(crumbs, index) {
    return index === crumbs.length - 1;
  }

  const Crumbs = { ROOT, buildCrumbs, isLast };
  if (typeof module !== 'undefined' && module.exports) module.exports = Crumbs;
  if (root) { root.CV = root.CV || {}; root.CV.Crumbs = Crumbs; }
})(typeof window !== 'undefined' ? window : null);
