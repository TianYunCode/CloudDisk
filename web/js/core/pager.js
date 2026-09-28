/* =============================================================================
   pager.js — 前端分页纯逻辑 (无 DOM 依赖, 浏览器挂 window.CV.Pager, Node 可 require)
   · pageCount(total, perPage): 总页数
   · pageRange(total, page, perPage): 当前页展示的 [from,to]
   · pageButtons(pages, current): 生成可交互的分页按钮 HTML (data-page 供事件委托)
   ============================================================================= */
(function (root) {
  'use strict';

  function pageCount(total, perPage) {
    return Math.ceil(total / perPage) || 1;
  }

  function pageRange(total, page, perPage) {
    const from = page * perPage + 1;
    const to = Math.min(total, (page + 1) * perPage);
    return { from, to };
  }

  function pageButtons(pages, current) {
    let html = '';
    for (let p = 0; p < pages; p++)
      html += '<button class="btn btn-sm ' + (p === current ? 'btn-primary' : 'btn-ghost') + '" data-page="' + p + '">' + (p + 1) + '</button>';
    return html;
  }

  function infoHtml(range, total) {
    return '<span class="info">显示文件 ' + range.from + '–' + range.to + ' / 共 ' + total + ' 项</span>';
  }

  const Pager = { pageCount, pageRange, pageButtons, infoHtml };
  if (typeof module !== 'undefined' && module.exports) module.exports = Pager;
  if (root) { root.CV = root.CV || {}; root.CV.Pager = Pager; }
})(typeof window !== 'undefined' ? window : null);
