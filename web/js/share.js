/* =============================================================================
   share.js — 公开分享页 (提取码校验 / 文件夹浏览 / 下载 / 转存)
   ============================================================================= */
(function () {
  'use strict';
  const { Store, Api, toast, humanSize, escapeHtml } = window.CV;
  const $ = (id) => document.getElementById(id);
  const root = $('spContent');

  const token = new URLSearchParams(location.search).get('t') || '';
  const state = { code: '', info: null, isFolder: false, rootName: '', folderId: 0 };

  const FOLDER_ICON = '<path d="M4 20h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2z"/>';
  const TYPES = {
    image:{e:['png','jpg','jpeg','gif','webp','svg','bmp','ico'],c:'#ec4899',i:'<circle cx="9" cy="9" r="2"/><path d="M21 15l-5-5L5 21"/><rect x="3" y="3" width="18" height="18" rx="2"/>'},
    video:{e:['mp4','mov','avi','mkv','webm','flv'],c:'#f43f5e',i:'<rect x="2" y="4" width="20" height="16" rx="2"/><path d="m10 9 5 3-5 3z"/>'},
    audio:{e:['mp3','wav','flac','aac','ogg','m4a'],c:'#f59e0b',i:'<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>'},
    pdf:{e:['pdf'],c:'#ef4444',i:'<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>'},
    doc:{e:['doc','docx','txt','md','rtf'],c:'#3b82f6',i:'<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6M8 13h8M8 17h8"/>'},
    zip:{e:['zip','rar','7z','tar','gz'],c:'#a855f7',i:'<path d="M21 8v13H3V8M1 3h22v5H1zM10 12h4"/>'},
    code:{e:['js','ts','c','cpp','h','py','java','go','html','css','json','sh','rs'],c:'#6366f1',i:'<path d="m16 18 6-6-6-6M8 6l-6 6 6 6"/>'},
  };
  function ftype(name) {
    const ext = (String(name).split('.').pop() || '').toLowerCase();
    for (const k in TYPES) if (TYPES[k].e.includes(ext)) return TYPES[k];
    return { c:'#9aa0b8', i:'<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>' };
  }
  const svg = (inner, cls) => '<svg class="icon' + (cls ? ' ' + cls : '') + '" viewBox="0 0 24 24">' + inner + '</svg>';

  function showError(msg) {
    root.innerHTML = '<div class="empty">' +
      svg('<circle cx="12" cy="12" r="10"/><path d="m15 9-6 6M9 9l6 6"/>') +
      '<h3>无法访问该分享</h3><p></p></div>';
    root.querySelector('p').textContent = msg || '链接无效或已失效';
  }

  function headHtml() {
    const t = state.isFolder ? { c:'#f59e0b', i:FOLDER_ICON } : ftype(state.rootName);
    return '<div class="share-head">' +
      '<span class="ftype" style="background:' + t.c + '">' + svg(t.i) + '</span>' +
      '<div><div class="share-title"></div></div></div>';
  }
  function setHeadTitle() {
    const el = root.querySelector('.share-title');
    if (!el) return;
    el.textContent = state.rootName;
    const sub = document.createElement('small');
    sub.textContent = state.isFolder ? '文件夹分享' : '文件分享';
    el.appendChild(sub);
  }

  function showCodeGate(wrong) {
    root.innerHTML = headHtml() +
      '<div class="code-gate">' +
        '<p>该分享受提取码保护，请输入提取码后访问。</p>' +
        '<input class="input" id="codeInput" maxlength="16" placeholder="提取码" autocomplete="off">' +
        '<button class="btn btn-primary" id="codeBtn">确定</button>' +
      '</div>';
    setHeadTitle();
    const input = $('codeInput'), btn = $('codeBtn');
    input.focus();
    if (wrong) toast('提取码错误', 'err');
    const submit = () => { state.code = input.value.trim(); if (!state.code) { toast('请输入提取码', 'warn'); return; } loadBrowse(state.folderId); };
    btn.onclick = submit;
    input.onkeydown = (e) => { if (e.key === 'Enter') submit(); };
  }

  async function loadBrowse(folderId) {
    state.folderId = folderId || 0;
    try {
      const data = await Api.shareBrowse(token, state.code, state.folderId);
      renderBrowse(data);
    } catch (e) {
      if (e && e.code === 403) { showCodeGate(true); return; }
      if (e && (e.code === 410 || e.code === 404)) { showError(e.message); return; }
      toast((e && e.message) || '加载失败', 'err');
    }
  }

  function renderBrowse(data) {
    state.isFolder = !!data.isFolder;
    state.rootName = data.rootName || state.rootName;
    let html = headHtml();

    if (state.isFolder) {
      html += '<div class="sp-crumbs" id="spCrumbs"></div>';
    }
    html += '<div class="card"><table class="files"><thead><tr>' +
      '<th>名称</th><th class="hide-sm">大小</th><th class="col-actions">操作</th>' +
      '</tr></thead><tbody id="spBody"></tbody></table></div>';

    // 转存栏
    html += '<div class="trash-head" style="margin-top:16px">' +
      '<p class="trash-note" id="spNote"></p>' +
      '<button class="btn btn-primary btn-sm" id="spSave">转存到我的网盘</button></div>';

    root.innerHTML = html;
    setHeadTitle();

    // 面包屑
    if (state.isFolder) {
      const crumbs = (data.breadcrumb && data.breadcrumb.length) ? data.breadcrumb : [{ id: 0, name: state.rootName }];
      const cr = $('spCrumbs');
      crumbs.forEach((c, i) => {
        const last = i === crumbs.length - 1;
        const a = document.createElement('span');
        a.className = 'sp-crumb' + (last ? ' current' : '');
        a.textContent = c.name;
        if (!last) a.onclick = () => loadBrowse(c.id);
        cr.appendChild(a);
        if (!last) { const s = document.createElement('span'); s.className = 'sp-sep'; s.textContent = ' / '; cr.appendChild(s); }
      });
    }

    // 列表
    const body = $('spBody');
    const folders = data.folders || [], files = data.files || [];
    if (!folders.length && !files.length) {
      body.innerHTML = '<tr><td colspan="3" style="text-align:center;color:var(--muted);padding:24px">（空文件夹）</td></tr>';
    }
    folders.forEach(f => {
      const tr = document.createElement('tr');
      tr.innerHTML = '<td><div class="fname folder-name"><span class="ftype folder">' + svg(FOLDER_ICON) + '</span>' +
        '<div style="min-width:0"><div class="txt link"></div></div></div></td>' +
        '<td class="hide-sm">—</td><td class="col-actions"></td>';
      tr.querySelector('.txt').textContent = f.name;
      tr.querySelector('.txt.link').onclick = () => loadBrowse(f.id);
      body.appendChild(tr);
    });
    files.forEach(f => {
      const t = ftype(f.filename);
      const tr = document.createElement('tr');
      tr.innerHTML = '<td><div class="fname"><span class="ftype" style="background:' + t.c + '">' + svg(t.i) + '</span>' +
        '<div style="min-width:0"><div class="txt"></div></div></div></td>' +
        '<td class="hide-sm">' + humanSize(f.size) + '</td>' +
        '<td class="col-actions"><div class="row-actions">' +
          '<button class="act" data-act="dl" title="下载">' + svg('<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="M7 10l5 5 5-5M12 15V3"/>') + '</button>' +
        '</div></td>';
      tr.querySelector('.txt').textContent = f.filename;
      tr.querySelector('[data-act="dl"]').onclick = () => download(f);
      body.appendChild(tr);
    });

    // 转存
    const note = $('spNote'), saveBtn = $('spSave');
    if (Store.token) {
      note.textContent = state.isFolder ? '将把整个文件夹（含子目录）复制到你的网盘根目录' : '将把该文件复制到你的网盘根目录';
      saveBtn.onclick = doSave;
    } else {
      note.textContent = '登录后可一键转存到自己的网盘';
      saveBtn.textContent = '登录后转存';
      saveBtn.onclick = () => { location.href = '/?redirect=' + encodeURIComponent(location.pathname + location.search); };
    }
  }

  function download(f) {
    const a = document.createElement('a');
    a.href = Api.shareDownloadUrl(token, state.code, f.id);
    a.download = f.filename;
    document.body.appendChild(a); a.click(); a.remove();
  }

  async function doSave() {
    const btn = $('spSave'); btn.disabled = true;
    try {
      const r = await Api.shareSave(token, state.code, 0);
      toast('已转存到「我的网盘」根目录', 'ok');
    } catch (e) {
      if (e && e.code === 401) { toast('请先登录', 'warn'); location.href = '/?redirect=' + encodeURIComponent(location.pathname + location.search); return; }
      toast((e && e.message) || '转存失败', 'err');
    } finally { btn.disabled = false; }
  }

  // ---- 启动 ----
  async function init() {
    if (!token) { showError('缺少分享标识'); return; }
    try {
      const info = await Api.shareInfo(token);
      state.info = info;
      state.isFolder = !!info.isFolder;
      state.rootName = info.name || '分享内容';
      if (info.needCode) showCodeGate(false);
      else loadBrowse(0);
    } catch (e) { showError((e && e.message) || '链接无效或已失效'); }
  }
  init();
})();
