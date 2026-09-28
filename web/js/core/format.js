/* =============================================================================
   format.js — 前端纯展示逻辑: 文件类型分类 + 图标常量 (无 DOM 依赖)
   · 浏览器: 挂载到 window.CV (Icons / TYPE_MAP / fileType)
   · Node  : module.exports, 供单元测试 (tests/e2e/format_validate.mjs)
   这是一处按扩展名 -> {color, icon} 的策略式查表, 与业务/渲染解耦, 可独立测试与复用。
   ============================================================================= */
(function (root) {
  'use strict';

  const Icons = {
    FOLDER: '<path d="M4 20h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2z"/>',
    SHARE:  '<circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><path d="m8.6 13.5 6.8 4M15.4 6.5 8.6 10.5"/>',
    FILE:   '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>',
  };

  const TYPE_MAP = {
    image: { ext: ['png','jpg','jpeg','gif','webp','svg','bmp','ico'], color: '#ec4899', icon: '<circle cx="9" cy="9" r="2"/><path d="M21 15l-5-5L5 21"/><rect x="3" y="3" width="18" height="18" rx="2"/>' },
    video: { ext: ['mp4','mov','avi','mkv','webm','flv'], color: '#f43f5e', icon: '<rect x="2" y="4" width="20" height="16" rx="2"/><path d="m10 9 5 3-5 3z"/>' },
    audio: { ext: ['mp3','wav','flac','aac','ogg','m4a'], color: '#f59e0b', icon: '<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>' },
    pdf:   { ext: ['pdf'], color: '#ef4444', icon: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>' },
    doc:   { ext: ['doc','docx','txt','md','rtf'], color: '#3b82f6', icon: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6M8 13h8M8 17h8"/>' },
    sheet: { ext: ['xls','xlsx','csv'], color: '#10b981', icon: '<rect x="3" y="3" width="18" height="18" rx="2"/><path d="M3 9h18M3 15h18M9 3v18M15 3v18"/>' },
    zip:   { ext: ['zip','rar','7z','tar','gz'], color: '#a855f7', icon: '<path d="M21 8v13H3V8M1 3h22v5H1zM10 12h4"/>' },
    code:  { ext: ['js','ts','c','cpp','h','py','java','go','html','css','json','sh','rs'], color: '#6366f1', icon: '<path d="m16 18 6-6-6-6M8 6l-6 6 6 6"/>' },
  };

  // 按文件名扩展名返回 {color, icon}; 未匹配返回灰色通用文件图标。
  function fileType(name) {
    const ext = (String(name).split('.').pop() || '').toLowerCase();
    for (const k in TYPE_MAP) if (TYPE_MAP[k].ext.includes(ext)) return TYPE_MAP[k];
    return { color: '#9aa0b8', icon: Icons.FILE };
  }

  const Fmt = { Icons, TYPE_MAP, fileType };
  if (typeof module !== 'undefined' && module.exports) module.exports = Fmt;
  if (root) {
    root.CV = root.CV || {};
    root.CV.Icons = Icons;
    root.CV.TYPE_MAP = TYPE_MAP;
    root.CV.fileType = fileType;
  }
})(typeof window !== 'undefined' ? window : null);
