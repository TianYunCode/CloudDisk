/* =============================================================================
   preview.js — 前端预览相关纯逻辑: 类型归类 + 极简 Markdown 渲染 (无 DOM 依赖)
   · 浏览器: 挂载到 window.CV.Preview
   · Node  : module.exports, 供单元测试 (tests/e2e/preview_validate.mjs)
   先转义防 XSS, 再套用有限 Markdown 语法; 与页面渲染解耦, 可独立测试与复用。
   ============================================================================= */
(function (root) {
  'use strict';

  const EXT_IMG = ['png','jpg','jpeg','gif','webp','bmp','svg','ico'];
  const EXT_VID = ['mp4','webm','mov','mkv','ogv'];
  const EXT_AUD = ['mp3','wav','flac','aac','ogg','m4a'];
  const EXT_TXT = ['txt','log','csv','json','xml','yml','yaml','js','ts','css','html','c','cpp','h','hpp','py','java','go','sh','rs','sql','ini','conf','mjs'];

  const ext = (n) => (String(n).split('.').pop() || '').toLowerCase();
  const isImage = (n) => EXT_IMG.includes(ext(n));

  function previewCat(n) {
    const e = ext(n);
    if (EXT_IMG.includes(e)) return 'image';
    if (EXT_VID.includes(e)) return 'video';
    if (EXT_AUD.includes(e)) return 'audio';
    if (e === 'pdf') return 'pdf';
    if (e === 'md' || e === 'markdown') return 'markdown';
    if (EXT_TXT.includes(e)) return 'text';
    return null;
  }

  // 默认转义 (Node 环境无 window.CV.escapeHtml 时使用, 与前端实现一致)
  function builtinEscape(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c]));
  }
  function escapeHtml(s) {
    return (root && root.CV && typeof root.CV.escapeHtml === 'function') ? root.CV.escapeHtml(s) : builtinEscape(s);
  }

  function safeUrl(u) { return /^(https?:\/\/|\/|#|mailto:)/i.test(u) ? u : '#'; }

  function inlineMd(s) {
    s = s.replace(/`([^`]+)`/g, (m, c) => '<code>' + c + '</code>');
    s = s.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, (m, a, u) => '<img alt="' + a + '" src="' + safeUrl(u) + '" style="max-width:100%">');
    s = s.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, (m, t, u) => '<a href="' + safeUrl(u) + '" target="_blank" rel="noopener">' + t + '</a>');
    s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
    s = s.replace(/(^|[^*])\*([^*]+)\*/g, '$1<em>$2</em>');
    s = s.replace(/(^|[^_])_([^_]+)_/g, '$1<em>$2</em>');
    return s;
  }

  function renderMarkdown(src) {
    const lines = escapeHtml(src).split('\n');
    let html = '', inCode = false, inList = null, para = [];
    const flushPara = () => { if (para.length) { html += '<p>' + inlineMd(para.join(' ')) + '</p>'; para = []; } };
    const closeList = () => { if (inList) { html += '</' + inList + '>'; inList = null; } };
    for (let raw of lines) {
      const fence = raw.match(/^```(.*)$/);
      if (fence) { flushPara(); closeList(); if (!inCode) { inCode = true; html += '<pre class="md-code"><code>'; } else { inCode = false; html += '</code></pre>'; } continue; }
      if (inCode) { html += raw + '\n'; continue; }
      if (/^\s*$/.test(raw)) { flushPara(); closeList(); continue; }
      let m;
      if ((m = raw.match(/^(#{1,6})\s+(.*)$/))) { flushPara(); closeList(); const lv = m[1].length; html += '<h' + lv + '>' + inlineMd(m[2]) + '</h' + lv + '>'; continue; }
      if (/^\s*([-*_])\1{2,}\s*$/.test(raw)) { flushPara(); closeList(); html += '<hr>'; continue; }
      if ((m = raw.match(/^>\s?(.*)$/))) { flushPara(); closeList(); html += '<blockquote>' + inlineMd(m[1]) + '</blockquote>'; continue; }
      if ((m = raw.match(/^\s*[-*+]\s+(.*)$/))) { flushPara(); if (inList !== 'ul') { closeList(); inList = 'ul'; html += '<ul>'; } html += '<li>' + inlineMd(m[1]) + '</li>'; continue; }
      if ((m = raw.match(/^\s*\d+\.\s+(.*)$/))) { flushPara(); if (inList !== 'ol') { closeList(); inList = 'ol'; html += '<ol>'; } html += '<li>' + inlineMd(m[1]) + '</li>'; continue; }
      para.push(raw);
    }
    if (inCode) html += '</code></pre>';
    flushPara(); closeList();
    return html;
  }

  const Preview = { ext, isImage, previewCat, safeUrl, inlineMd, renderMarkdown };
  if (typeof module !== 'undefined' && module.exports) module.exports = Preview;
  if (root) { root.CV = root.CV || {}; root.CV.Preview = Preview; }
})(typeof window !== 'undefined' ? window : null);
