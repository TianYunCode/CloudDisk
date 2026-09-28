/* =============================================================================
   app.js — 云盘主界面逻辑 (文件夹 / 面包屑 / 多选批量 / 移动 / 回收站)
   ============================================================================= */
(function () {
  'use strict';
  const { Store, Api, toast, humanSize, escapeHtml, Theme } = window.CV;

  if (!Store.token) { location.href = '/'; return; }

  const $ = (id) => document.getElementById(id);
  const QUOTA_FALLBACK = 1073741824; // 后端未返回配额时的兜底: 1 GB
  const PAGE_SIZE = 12;

  const state = {
    view: 'files',           // 'files' | 'trash'
    parentId: 0,             // 当前文件夹
    breadcrumb: [],          // [{id,name}]
    page: 0, total: 0,
    keyword: '', sort: 'created_at', order: 'desc',
    folders: [], items: [],
    selFiles: new Set(), selFolders: new Set(),
    tags: [], tagMap: {},     // 标签缓存: [{id,name,color,count}] + id->tag
  };

  // ---------------- 图标 / 文件类型 (纯逻辑抽出至 core/format.js) ----------------
  const { Icons, fileType } = window.CV;
  const FOLDER_ICON = Icons.FOLDER;
  const SHARE_ICON = Icons.SHARE;
  function svg(inner, cls) { return '<svg class="icon' + (cls ? ' ' + cls : '') + '" viewBox="0 0 24 24">' + inner + '</svg>'; }

  // ---------------- 用户信息 & 存储 ----------------
  async function loadUser() {
    try {
      const info = await Api.userInfo();
      state.me = info;
      $('userName').textContent = info.nickname || info.username;
      // 头像: 有则显示图片, 否则首字母
      const av = $('avatar');
      if (info.hasAvatar) {
        av.textContent = '';
        av.style.backgroundImage = 'url(' + Api.avatarUrl(info.uid, info.avatar) + ')';
        av.classList.add('has-img');
      } else {
        av.textContent = ((info.nickname || info.username)[0] || '?').toUpperCase();
        av.style.backgroundImage = '';
        av.classList.remove('has-img');
      }
      $('userSince').textContent = '注册于 ' + (info.createdAt || '').split(' ')[0];
      $('fileCount').textContent = info.fileCount + ' 个文件';
      const used = Number(info.storageUsed) || 0;
      const quota = Number(info.quota) || QUOTA_FALLBACK;
      const pct = Math.min(100, (used / quota) * 100);
      $('storageFill').style.width = pct.toFixed(1) + '%';
      $('storagePct').textContent = pct.toFixed(pct < 10 ? 1 : 0) + '%';
      $('storageUsed').textContent = humanSize(used);
      $('storageTotal').textContent = '/ ' + humanSize(quota);
      // 管理员导航
      const na = $('navAdmin'); if (na) na.hidden = Number(info.role) !== 1;
    } catch (e) { /* 401 已处理 */ }
  }

  // ---------------- 选择状态 ----------------
  function clearSelection() { state.selFiles.clear(); state.selFolders.clear(); syncBatchBar(); }
  function selCount() { return state.selFiles.size + state.selFolders.size; }
  function syncBatchBar() {
    const n = selCount();
    const bar = $('batchBar');
    bar.hidden = n === 0;
    $('batchCount').textContent = '已选 ' + n + ' 项';
    const sel = $('selAll');
    const totalOnPage = state.folders.length + state.items.length;
    sel.checked = totalOnPage > 0 && n >= totalOnPage;
    sel.indeterminate = n > 0 && n < totalOnPage;
    // 同步复选框
    document.querySelectorAll('#fileBody tr[data-kind]').forEach(tr => {
      const cb = tr.querySelector('.rowcheck');
      if (!cb) return;
      const id = +tr.dataset.id;
      cb.checked = tr.dataset.kind === 'folder' ? state.selFolders.has(id) : state.selFiles.has(id);
      tr.classList.toggle('selected', cb.checked);
    });
  }

  // ---------------- 面包屑 ----------------
  function renderBreadcrumb() {
    const el = $('breadcrumb');
    el.innerHTML = '';
    const crumbs = [{ id: 0, name: '根目录' }].concat(state.breadcrumb || []);
    crumbs.forEach((c, i) => {
      const seg = document.createElement('span');
      seg.className = 'crumb' + (i === crumbs.length - 1 ? ' current' : '');
      seg.textContent = c.name;
      if (i < crumbs.length - 1) seg.onclick = () => navigate(c.id);
      el.appendChild(seg);
      if (i < crumbs.length - 1) {
        const sp = document.createElement('span');
        sp.className = 'crumb-sep';
        sp.innerHTML = svg('<path d="m9 18 6-6-6-6"/>');
        el.appendChild(sp);
      }
    });
  }

  function navigate(id) {
    state.parentId = id;
    state.page = 0;
    clearSelection();
    loadList();
  }

  // ---------------- 文件列表 ----------------
  function skeletonRows() {
    let h = '';
    for (let i = 0; i < 5; i++) h += '<tr><td colspan="5"><div class="skeleton" style="width:' + (40 + Math.random()*50) + '%"></div></td></tr>';
    $('fileBody').innerHTML = h;
    $('listExtra').innerHTML = '';
  }

  async function loadList() {
    skeletonRows();
    let data;
    try {
      data = await Api.list({
        parentId: state.parentId,
        limit: PAGE_SIZE, offset: state.page * PAGE_SIZE,
        keyword: state.keyword, sort: state.sort, order: state.order,
      });
    } catch (e) { toast(e.message || '加载失败', 'err'); return; }
    state.total = data.total;
    state.breadcrumb = data.breadcrumb || [];
    state.folders = data.folders || [];
    state.items = data.items || [];
    renderBreadcrumb();
    renderList();
    renderPager();
    syncBatchBar();
  }

  function renderList() {
    const body = $('fileBody');
    const { folders, items } = state;
    if (folders.length === 0 && items.length === 0) {
      body.innerHTML = '';
      $('listExtra').innerHTML =
        '<div class="empty">' + svg('<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>') +
        '<h3>' + (state.keyword ? '没有匹配的项目' : '这个文件夹是空的') + '</h3>' +
        '<p>' + (state.keyword ? '换个关键词试试' : '新建文件夹，或拖拽文件到此处上传') + '</p></div>';
      return;
    }
    $('listExtra').innerHTML = '';
    body.innerHTML = '';

    // 文件夹行
    folders.forEach(f => {
      const tr = document.createElement('tr');
      tr.dataset.kind = 'folder'; tr.dataset.id = f.id;
      tr.innerHTML =
        '<td class="col-check"><input type="checkbox" class="rowcheck"></td>' +
        '<td><div class="fname folder-name">' +
          '<span class="ftype folder">' + svg(FOLDER_ICON) + '</span>' +
          '<div style="min-width:0"><div class="txt link"></div></div>' +
        '</div></td>' +
        '<td class="hide-sm">—</td>' +
        '<td class="hide-sm">' + escapeHtml(f.createdAt || '') + '</td>' +
        '<td class="col-actions"><div class="row-actions">' +
          '<button class="act" data-act="rename" title="重命名">' + svg('<path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>') + '</button>' +
          '<button class="act" data-act="share" title="分享">' + svg(SHARE_ICON) + '</button>' +
          '<button class="act danger" data-act="delete" title="删除">' + svg('<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>') + '</button>' +
        '</div></td>';
      tr.querySelector('.txt').textContent = f.name;
      tr.querySelector('.txt.link').onclick = () => navigate(f.id);
      const cb = tr.querySelector('.rowcheck');
      cb.onclick = (e) => { e.stopPropagation(); toggleSel('folder', f.id, cb.checked); };
      tr.querySelector('[data-act="rename"]').onclick = () => doRenameFolder(f);
      tr.querySelector('[data-act="share"]').onclick = () => doShare('folder', f);
      tr.querySelector('[data-act="delete"]').onclick = () => doDelete([], [f.id], f.name);
      body.appendChild(tr);
    });

    // 文件行
    items.forEach(f => {
      const t = fileType(f.filename);
      const cat = previewCat(f.filename);
      const tr = document.createElement('tr');
      tr.dataset.kind = 'file'; tr.dataset.id = f.id;
      const thumbCell = isImage(f.filename)
        ? '<span class="ftype thumb-wrap" data-thumb="1"><img class="thumb" alt="" loading="lazy"><span class="ftype-fallback" style="background:' + t.color + '">' + svg(t.icon) + '</span></span>'
        : '<span class="ftype" style="background:' + t.color + '">' + svg(t.icon) + '</span>';
      tr.innerHTML =
        '<td class="col-check"><input type="checkbox" class="rowcheck"></td>' +
        '<td><div class="fname">' + thumbCell +
          '<div style="min-width:0"><div class="txt' + (cat ? ' link' : '') + '"></div><div class="hash"></div><div class="row-tags"></div></div>' +
        '</div></td>' +
        '<td class="hide-sm">' + humanSize(f.size) + '</td>' +
        '<td class="hide-sm">' + escapeHtml(f.lastUpdate || f.createdAt || '') + '</td>' +
        '<td class="col-actions"><div class="row-actions">' +
          (cat ? '<button class="act" data-act="preview" title="预览">' + svg('<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>') + '</button>' : '') +
          '<button class="act" data-act="download" title="下载">' + svg('<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="M7 10l5 5 5-5M12 15V3"/>') + '</button>' +
          '<button class="act" data-act="rename" title="重命名">' + svg('<path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>') + '</button>' +
          '<button class="act" data-act="share" title="分享">' + svg(SHARE_ICON) + '</button>' +
          '<button class="act" data-act="versions" title="历史版本">' + svg('<path d="M3 3v5h5"/><path d="M3.05 13A9 9 0 1 0 6 5.3L3 8"/><path d="M12 7v5l4 2"/>') + '</button>' +
          '<button class="act" data-act="tags" title="标签">' + svg('<path d="M20.6 13.4 12 22l-9-9V3h10z"/><circle cx="7.5" cy="7.5" r="1.5"/>') + '</button>' +
          '<button class="act danger" data-act="delete" title="删除">' + svg('<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>') + '</button>' +
        '</div></td>';
      tr.querySelector('.txt').textContent = f.filename;
      tr.querySelector('.hash').textContent = (f.hash || '').slice(0, 12);
      if (isImage(f.filename)) {
        const img = tr.querySelector('img.thumb');
        img.src = Api.thumbUrl(f.id, 96);
        img.onerror = () => { const w = tr.querySelector('.thumb-wrap'); if (w) w.classList.add('thumb-failed'); };
      }
      const cb = tr.querySelector('.rowcheck');
      cb.onclick = (e) => { e.stopPropagation(); toggleSel('file', f.id, cb.checked); };
      if (cat) { tr.querySelector('.txt.link').onclick = () => openPreview(f); }
      const pv = tr.querySelector('[data-act="preview"]'); if (pv) pv.onclick = () => openPreview(f);
      tr.querySelector('[data-act="download"]').onclick = () => doDownload(f);
      tr.querySelector('[data-act="rename"]').onclick = () => doRenameFile(f);
      tr.querySelector('[data-act="share"]').onclick = () => doShare('file', f);
      tr.querySelector('[data-act="versions"]').onclick = () => openVersions(f);
      tr.querySelector('[data-act="tags"]').onclick = () => openFileTags(f);
      renderRowTags(tr.querySelector('.row-tags'), f.tagIds);
      tr.querySelector('[data-act="delete"]').onclick = () => doDelete([f.id], [], f.filename);
      body.appendChild(tr);
    });
    syncBatchBar();
  }

  function toggleSel(kind, id, on) {
    const set = kind === 'folder' ? state.selFolders : state.selFiles;
    if (on) set.add(id); else set.delete(id);
    syncBatchBar();
  }

  function renderPager() {
    const pages = Math.ceil(state.total / PAGE_SIZE) || 1;
    if (pages <= 1) return;
    const from = state.page * PAGE_SIZE + 1;
    const to = Math.min(state.total, (state.page + 1) * PAGE_SIZE);
    let btns = '';
    for (let p = 0; p < pages; p++)
      btns += '<button class="btn btn-sm ' + (p === state.page ? 'btn-primary' : 'btn-ghost') + '" data-page="' + p + '">' + (p + 1) + '</button>';
    const pager = document.createElement('div');
    pager.className = 'pager';
    pager.innerHTML = '<span class="info">显示文件 ' + from + '–' + to + ' / 共 ' + state.total + ' 项</span><div class="pages">' + btns + '</div>';
    $('listExtra').appendChild(pager);
    pager.querySelectorAll('[data-page]').forEach(b => b.onclick = () => { state.page = +b.dataset.page; clearSelection(); loadList(); });
  }

  // ---------------- 下载 / 重命名 / 删除 ----------------
  // ---------------- 标签 ----------------
  async function loadTags() {
    try {
      const r = await Api.tagList();
      state.tags = r.tags || [];
      state.tagMap = {};
      state.tags.forEach(t => { state.tagMap[t.id] = t; });
    } catch (e) { /* ignore */ }
  }
  function renderRowTags(box, tagIds) {
    if (!box) return;
    const ids = tagIds || [];
    box.innerHTML = ids.map(id => {
      const t = state.tagMap[id]; if (!t) return '';
      return '<span class="tag-chip" style="--tc:' + t.color + '">' + escapeHtml(t.name) + '</span>';
    }).join('');
  }
  function openFileTags(f) {
    const chosen = new Set(f.tagIds || []);
    openModal({
      title: '设置标签 · ' + f.filename,
      body: '<div class="tag-picker" id="tagPicker"></div>' +
            '<div class="tag-new"><input class="input" id="tagNewName" placeholder="新建标签名称" maxlength="64">' +
            '<input type="color" id="tagNewColor" value="#6366f1" class="tag-color-input">' +
            '<button class="btn btn-sm btn-ghost" id="tagNewBtn">新建</button></div>',
      confirmText: '保存',
      onOpen: () => {
        const render = () => {
          const p = $('tagPicker');
          if (!state.tags.length) { p.innerHTML = '<div class="muted" style="font-size:13px">还没有标签，先在下方新建一个。</div>'; return; }
          p.innerHTML = state.tags.map(t =>
            '<button type="button" class="tag-opt' + (chosen.has(t.id) ? ' on' : '') + '" data-tid="' + t.id + '" style="--tc:' + t.color + '">' +
            escapeHtml(t.name) + '</button>').join('');
          p.querySelectorAll('[data-tid]').forEach(b => b.onclick = () => {
            const id = Number(b.dataset.tid);
            if (chosen.has(id)) { chosen.delete(id); b.classList.remove('on'); }
            else { chosen.add(id); b.classList.add('on'); }
          });
        };
        render();
        $('tagNewBtn').onclick = async () => {
          const name = $('tagNewName').value.trim();
          const color = $('tagNewColor').value;
          if (!name) { toast('请输入标签名', 'err'); return; }
          try {
            const t = await Api.tagCreate(name, color);
            await loadTags(); chosen.add(t.id); $('tagNewName').value = ''; render();
            toast('已新建标签', 'ok');
          } catch (e) { toast(e.message || '新建失败', 'err'); }
        };
      },
      onConfirm: async () => {
        try {
          await Api.fileTagsSet(f.id, [...chosen]);
          f.tagIds = [...chosen];
          await loadTags(); loadList();
          toast('标签已保存', 'ok');
          return true;
        } catch (e) { toast(e.message || '保存失败', 'err'); return false; }
      },
    });
  }

  // ---------------- 标签视图 ----------------
  function showTags() {
    state.view = 'tags';
    setNav('navTags');
    $('pageTitle').textContent = '标签';
    loadTagsView();
  }
  async function loadTagsView() {
    const cloud = $('tagCloud');
    $('tagFilesCard').hidden = true;
    cloud.innerHTML = '<div class="empty-cell">加载中…</div>';
    await loadTags();
    if (!state.tags.length) { cloud.innerHTML = '<div class="empty-cell">还没有标签。给文件添加标签后会显示在这里。</div>'; return; }
    cloud.innerHTML = state.tags.map(t =>
      '<div class="tag-card" data-tid="' + t.id + '" style="--tc:' + t.color + '">' +
        '<span class="tag-dot"></span><span class="tag-card-name">' + escapeHtml(t.name) + '</span>' +
        '<span class="tag-card-count">' + t.count + '</span>' +
        '<button class="tag-del" data-del="' + t.id + '" title="删除标签">&times;</button>' +
      '</div>').join('');
    cloud.querySelectorAll('.tag-card').forEach(c => c.onclick = (e) => {
      if (e.target.closest('[data-del]')) return;
      openTagFiles(Number(c.dataset.tid));
    });
    cloud.querySelectorAll('[data-del]').forEach(b => b.onclick = async (e) => {
      e.stopPropagation();
      const id = Number(b.dataset.del); const t = state.tagMap[id];
      openModal({
        title: '删除标签', body: '<p>确定删除标签「' + escapeHtml(t ? t.name : '') + '」吗？它将从所有文件上移除。</p>',
        confirmText: '删除', danger: true,
        onConfirm: async () => {
          try { await Api.tagDelete(id); toast('已删除', 'ok'); loadTagsView(); return true; }
          catch (err) { toast(err.message || '删除失败', 'err'); return false; }
        },
      });
    });
  }
  async function openTagFiles(tagId) {
    const card = $('tagFilesCard'); const body = $('tagFilesBody'); const t = state.tagMap[tagId];
    card.hidden = false;
    $('tagFilesTitle').textContent = '标签「' + (t ? t.name : '') + '」下的文件';
    body.innerHTML = '<div class="empty-cell">加载中…</div>';
    try {
      const r = await Api.filesByTag(tagId);
      const items = r.items || [];
      body.innerHTML = items.length ? items.map(f =>
        '<div class="lg-row"><span class="lg-name">' + escapeHtml(f.filename) + '</span>' +
        '<span><a class="lg-size" href="' + Api.downloadUrl(f.id) + '">' + humanSize(f.size) + '</a> ' +
        '<button class="btn btn-sm btn-ghost" data-go="' + f.parentId + '">前往目录</button></span></div>').join('')
        : '<div class="empty-cell">该标签下暂无文件</div>';
      body.querySelectorAll('[data-go]').forEach(b => b.onclick = () => { state.parentId = Number(b.dataset.go); showFiles(); });
    } catch (e) { body.innerHTML = '<div class="empty-cell">加载失败</div>'; }
  }

  function doDownload(f) {
    const a = document.createElement('a');
    a.href = Api.downloadUrl(f.id);
    a.download = f.filename;
    document.body.appendChild(a); a.click(); a.remove();
  }

  // ---------------- 活动日志 ----------------
  const ACT_META = {
    login:              { label: '登录', color: '#10b981', icon: '<path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/><path d="M10 17l5-5-5-5M15 12H3"/>' },
    login_fail:         { label: '登录失败', color: '#ef4444', icon: '<circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/>' },
    login_blocked:      { label: '登录被限流', color: '#ef4444', icon: '<circle cx="12" cy="12" r="10"/><path d="M4.9 4.9l14.2 14.2"/>' },
    login_2fa_fail:     { label: '两步验证失败', color: '#ef4444', icon: '<circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/>' },
    login_disabled:     { label: '禁用账户登录', color: '#ef4444', icon: '<circle cx="12" cy="12" r="10"/>' },
    register:           { label: '注册', color: '#6366f1', icon: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>' },
    file_upload:        { label: '上传', color: '#0ea5e9', icon: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="M17 8l-5-5-5 5M12 3v12"/>' },
    file_delete:        { label: '删除', color: '#f59e0b', icon: '<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>' },
    file_restore:       { label: '恢复', color: '#10b981', icon: '<path d="M3 3v5h5"/><path d="M3.05 13A9 9 0 1 0 6 5.3L3 8"/>' },
    file_rename:        { label: '重命名', color: '#8b5cf6', icon: '<path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>' },
    folder_create:      { label: '新建文件夹', color: '#6366f1', icon: '<path d="M4 20h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-8l-2-3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2z"/>' },
    share_create:       { label: '创建分享', color: '#ec4899', icon: '<circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><path d="m8.6 13.5 6.8 4M15.4 6.5 8.6 10.5"/>' },
    password_change:    { label: '修改密码', color: '#8b5cf6', icon: '<rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>' },
    password_change_fail: { label: '改密失败', color: '#ef4444', icon: '<rect x="3" y="11" width="18" height="11" rx="2"/>' },
    profile_update:     { label: '资料更新', color: '#6366f1', icon: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>' },
    avatar_update:      { label: '更换头像', color: '#6366f1', icon: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>' },
    '2fa_enable':       { label: '启用两步验证', color: '#10b981', icon: '<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>' },
    '2fa_disable':      { label: '关闭两步验证', color: '#f59e0b', icon: '<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/>' },
    token_create:       { label: '创建令牌', color: '#0ea5e9', icon: '<path d="M21 2l-2 2m-7.6 7.6a5 5 0 1 1-7 7 5 5 0 0 1 7-7z"/>' },
    token_revoke:       { label: '吊销令牌', color: '#f59e0b', icon: '<path d="M21 2l-2 2m-7.6 7.6a5 5 0 1 1-7 7 5 5 0 0 1 7-7z"/>' },
  };
  function showActivity() {
    state.view = 'activity';
    setNav('navActivity');
    $('pageTitle').textContent = '活动日志';
    loadActivity();
  }
  async function loadActivity() {
    const box = $('activityList');
    box.innerHTML = '<div class="empty-cell">加载中…</div>';
    const action = $('activityFilter') ? $('activityFilter').value : '';
    try {
      const r = await Api.activity(action);
      const logs = r.logs || [];
      if (!logs.length) { box.innerHTML = '<div class="empty-cell">暂无记录</div>'; return; }
      box.innerHTML = logs.map(l => {
        const m = ACT_META[l.action] || { label: l.action, color: '#94a3b8', icon: '<circle cx="12" cy="12" r="10"/>' };
        const ip = l.ip && l.ip !== '-' ? ' · ' + escapeHtml(l.ip) : '';
        return '<div class="act-row">' +
          '<span class="act-ic" style="background:' + m.color + '">' + svg(m.icon) + '</span>' +
          '<div class="act-main"><div class="act-label">' + escapeHtml(m.label) +
            (l.detail ? ' <span class="act-detail">' + escapeHtml(l.detail) + '</span>' : '') + '</div>' +
            '<div class="act-time">' + escapeHtml(l.createdAt || '') + ip + '</div></div>' +
          '</div>';
      }).join('');
    } catch (e) { box.innerHTML = '<div class="empty-cell">加载失败</div>'; }
  }

  // ---------------- 历史版本 ----------------
  function openVersions(f) {
    openModal({
      title: '历史版本 · ' + f.filename,
      body: '<div class="ver-wrap">' +
              '<div class="ver-upload"><label class="btn btn-primary btn-sm" for="verFile">上传新版本</label>' +
              '<input type="file" id="verFile" hidden><span class="ver-hint" id="verHint">上传会把当前内容存为一个历史版本</span></div>' +
              '<div class="ver-list" id="verList"><div class="empty-cell">加载中…</div></div>' +
            '</div>',
      confirmText: '关闭',
      onConfirm: () => true,
      onOpen: () => {
        loadVer();
        $('verFile').onchange = async () => {
          const file = $('verFile').files[0]; if (!file) return;
          $('verHint').textContent = '上传中…';
          try {
            const r = await Api.versionUpload(f.id, file);
            toast(r && r.changed === false ? '内容未变化' : '已上传新版本', 'ok');
            loadVer(); loadList(); loadUser();
          } catch (e) { toast(e.message || '上传失败', 'err'); }
          $('verFile').value = ''; $('verHint').textContent = '上传会把当前内容存为一个历史版本';
        };
      },
    });
    async function loadVer() {
      const box = $('verList'); if (!box) return;
      box.innerHTML = '<div class="empty-cell">加载中…</div>';
      try {
        const r = await Api.versions(f.id);
        const cur = r.current || {}; const vs = r.versions || [];
        let html = '<div class="ver-row ver-cur"><div class="ver-meta"><b>当前版本</b>' +
                   '<span class="muted">' + humanSize(cur.size || 0) + ' · ' + escapeHtml(cur.updatedAt || '') + '</span></div>' +
                   '<div class="ver-ops"><span class="tag tag-on">最新</span></div></div>';
        if (!vs.length) {
          html += '<div class="empty-cell">暂无历史版本</div>';
        } else {
          html += vs.map(v =>
            '<div class="ver-row"><div class="ver-meta">版本 #' + v.versionId +
            '<span class="muted">' + humanSize(v.size) + ' · ' + escapeHtml(v.createdAt || '') + '</span></div>' +
            '<div class="ver-ops">' +
              '<a class="btn btn-sm btn-ghost" href="' + Api.versionDownloadUrl(v.versionId) + '">下载</a>' +
              '<button class="btn btn-sm btn-ghost" data-restore="' + v.versionId + '">恢复</button>' +
            '</div></div>').join('');
        }
        box.innerHTML = html;
        box.querySelectorAll('[data-restore]').forEach(b => b.onclick = async () => {
          b.disabled = true;
          try { await Api.versionRestore(f.id, Number(b.dataset.restore)); toast('已恢复到该版本', 'ok'); loadVer(); loadList(); loadUser(); }
          catch (e) { toast(e.message || '恢复失败', 'err'); b.disabled = false; }
        });
      } catch (e) { box.innerHTML = '<div class="empty-cell">加载失败</div>'; }
    }
  }

  function doRenameFile(f) {
    openModal({
      title: '重命名文件',
      body: '<div class="field"><label>新文件名</label><input class="input" id="renameInput"></div>',
      onOpen: () => { const i = $('renameInput'); i.value = f.filename; i.focus(); i.select(); },
      onConfirm: async () => {
        const nn = $('renameInput').value.trim();
        if (!nn) { toast('文件名不能为空', 'warn'); return false; }
        if (nn === f.filename) return true;
        try { await Api.rename(f.id, nn); toast('已重命名', 'ok'); loadList(); }
        catch (e) { toast(e.message || '重命名失败', 'err'); return false; }
      },
    });
  }

  function doRenameFolder(f) {
    openModal({
      title: '重命名文件夹',
      body: '<div class="field"><label>新名称</label><input class="input" id="renameInput"></div>',
      onOpen: () => { const i = $('renameInput'); i.value = f.name; i.focus(); i.select(); },
      onConfirm: async () => {
        const nn = $('renameInput').value.trim();
        if (!nn) { toast('名称不能为空', 'warn'); return false; }
        if (nn === f.name) return true;
        try { await Api.folderRename(f.id, nn); toast('已重命名', 'ok'); loadList(); }
        catch (e) { toast(e.message || '重命名失败', 'err'); return false; }
      },
    });
  }

  function doDelete(fileIds, folderIds, label) {
    const n = fileIds.length + folderIds.length;
    const what = label ? '<b></b>' : (n + ' 个项目');
    openModal({
      title: '移入回收站',
      body: '<p>确定要删除 ' + what + ' 吗？可稍后在回收站恢复。</p>',
      confirmText: '删除',
      danger: true,
      onOpen: () => { if (label) document.querySelector('#modalRoot .modal p b').textContent = label; },
      onConfirm: async () => {
        try {
          await Api.fsDelete(fileIds, folderIds);
          toast('已移入回收站', 'ok');
          clearSelection();
          if (state.page > 0 && state.total - fileIds.length <= state.page * PAGE_SIZE) state.page--;
          loadList(); loadUser();
        } catch (e) { toast(e.message || '删除失败', 'err'); return false; }
      },
    });
  }

  // ---------------- 批量操作 ----------------
  function selectedIds() {
    return { fileIds: Array.from(state.selFiles), folderIds: Array.from(state.selFolders) };
  }
  $('batchClear').onclick = () => { clearSelection(); };
  $('selAll').onclick = (e) => {
    if (e.target.checked) {
      state.folders.forEach(f => state.selFolders.add(f.id));
      state.items.forEach(f => state.selFiles.add(f.id));
    } else clearSelection();
    syncBatchBar();
  };
  $('batchDelete').onclick = () => {
    const { fileIds, folderIds } = selectedIds();
    if (fileIds.length + folderIds.length === 0) return;
    doDelete(fileIds, folderIds, null);
  };
  $('batchDownload').onclick = () => {
    if (state.selFolders.size) toast('文件夹暂不支持打包下载，仅下载所选文件', 'warn');
    const files = state.items.filter(f => state.selFiles.has(f.id));
    if (!files.length) { toast('没有可下载的文件', 'warn'); return; }
    files.forEach((f, i) => setTimeout(() => doDownload(f), i * 300));
  };
  $('batchMove').onclick = () => {
    const { fileIds, folderIds } = selectedIds();
    if (fileIds.length + folderIds.length === 0) return;
    openMovePicker(fileIds, folderIds);
  };
  { const el = $('batchFav'); if (el) el.onclick = async () => {
    const { fileIds, folderIds } = selectedIds();
    if (fileIds.length + folderIds.length === 0) return;
    try { await Api.favBatch(fileIds, folderIds); toast('已加入收藏', 'ok'); clearSelection(); }
    catch (e) { toast(e.message || '收藏失败', 'err'); }
  }; }

  // ---------------- 移动选择器 ----------------
  function openMovePicker(fileIds, folderIds) {
    let pickParent = 0;
    openModal({
      title: '移动到…',
      body: '<div class="picker"><div class="picker-crumbs" id="pkCrumbs"></div><div class="picker-list" id="pkList"></div></div>',
      confirmText: '移动到此处',
      onOpen: () => renderPicker(),
      onConfirm: async () => {
        if (folderIds.includes(pickParent)) { toast('不能移动到所选文件夹自身', 'warn'); return false; }
        try { await Api.move(fileIds, folderIds, pickParent); toast('已移动', 'ok'); clearSelection(); loadList(); loadUser(); }
        catch (e) { toast(e.message || '移动失败', 'err'); return false; }
      },
    });
    async function renderPicker() {
      let data;
      try { data = await Api.list({ parentId: pickParent, limit: 200, offset: 0 }); }
      catch (e) { toast('加载失败', 'err'); return; }
      const crumbs = [{ id: 0, name: '根目录' }].concat(data.breadcrumb || []);
      const cr = $('pkCrumbs'); cr.innerHTML = '';
      crumbs.forEach((c, i) => {
        const a = document.createElement('span'); a.className = 'pk-crumb'; a.textContent = c.name;
        a.onclick = () => { pickParent = c.id; renderPicker(); };
        cr.appendChild(a);
        if (i < crumbs.length - 1) { const s = document.createElement('span'); s.textContent = ' / '; s.className = 'pk-sep'; cr.appendChild(s); }
      });
      const list = $('pkList'); list.innerHTML = '';
      const subs = (data.folders || []).filter(f => !folderIds.includes(f.id));
      if (!subs.length) { list.innerHTML = '<div class="pk-empty">（没有可进入的子文件夹）</div>'; return; }
      subs.forEach(f => {
        const it = document.createElement('div'); it.className = 'pk-item';
        it.innerHTML = svg(FOLDER_ICON) + '<span></span>' + svg('<path d="m9 18 6-6-6-6"/>', 'pk-arrow');
        it.querySelector('span').textContent = f.name;
        it.onclick = () => { pickParent = f.id; renderPicker(); };
        list.appendChild(it);
      });
    }
  }

  // ---------------- 在线预览 ----------------
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

  // 极简 Markdown 渲染 (先转义防 XSS, 再套用有限语法)
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

  let pvKeyHandler = null;
  function closePreview() {
    const root = $('previewRoot');
    root.querySelectorAll('video,audio').forEach(m => { try { m.pause(); m.src = ''; } catch (e) {} });
    root.innerHTML = '';
    if (pvKeyHandler) { document.removeEventListener('keydown', pvKeyHandler); pvKeyHandler = null; }
  }

  function openPreview(file) {
    const root = $('previewRoot');
    const gallery = state.items.filter(f => isImage(f.filename));
    let idx = gallery.findIndex(f => f.id === file.id);
    const useGallery = isImage(file.filename) && gallery.length > 1 && idx >= 0;

    root.innerHTML =
      '<div class="pv-backdrop" id="pvBackdrop">' +
        '<div class="pv-topbar">' +
          '<div class="pv-title" id="pvTitle"></div>' +
          '<div class="pv-tools">' +
            '<button class="btn btn-sm btn-ghost" id="pvDl">' + svg('<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="M7 10l5 5 5-5M12 15V3"/>') + '下载</button>' +
            '<button class="pv-close" id="pvClose" title="关闭 (Esc)">' + svg('<path d="M18 6 6 18M6 6l12 12"/>') + '</button>' +
          '</div>' +
        '</div>' +
        (useGallery ? '<button class="pv-nav pv-prev" id="pvPrev">' + svg('<path d="m15 18-6-6 6-6"/>') + '</button>' : '') +
        '<div class="pv-stage" id="pvStage"></div>' +
        (useGallery ? '<button class="pv-nav pv-next" id="pvNext">' + svg('<path d="m9 18 6-6-6-6"/>') + '</button>' : '') +
      '</div>';

    let current = file;
    function show(f) {
      current = f;
      $('pvTitle').textContent = f.filename + (useGallery ? ('  (' + (idx + 1) + '/' + gallery.length + ')') : '');
      $('pvDl').onclick = () => doDownload(f);
      renderStage(f);
    }
    function renderStage(f) {
      const stage = $('pvStage');
      const cat = previewCat(f.filename);
      const url = Api.contentUrl(f.id);
      if (cat === 'image') {
        stage.innerHTML = '<img class="pv-img" alt="">';
        stage.querySelector('img').src = url;
      } else if (cat === 'video') {
        stage.innerHTML = '<video class="pv-media" controls autoplay playsinline></video>';
        stage.querySelector('video').src = url;
      } else if (cat === 'audio') {
        stage.innerHTML = '<div class="pv-audio">' + svg('<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>', 'icon-lg') +
          '<div class="pv-aname"></div><audio class="pv-media" controls autoplay></audio></div>';
        stage.querySelector('.pv-aname').textContent = f.filename;
        stage.querySelector('audio').src = url;
      } else if (cat === 'pdf') {
        stage.innerHTML = '<iframe class="pv-frame" src="' + url + '"></iframe>';
      } else { // text / markdown / code
        stage.innerHTML = '<div class="pv-doc" id="pvDoc"><div class="empty"><div class="skeleton" style="width:40%;margin:0 auto"></div></div></div>';
        fetch(url).then(r => r.text()).then(txt => {
          const doc = $('pvDoc'); if (!doc) return;
          if (txt.length > 524288) txt = txt.slice(0, 524288) + '\n\n… (内容过大, 已截断预览) …';
          if (cat === 'markdown') { doc.className = 'pv-doc md-body'; doc.innerHTML = renderMarkdown(txt); }
          else { doc.className = 'pv-doc'; const pre = document.createElement('pre'); pre.className = 'pv-pre'; pre.textContent = txt; doc.innerHTML = ''; doc.appendChild(pre); }
        }).catch(() => { const doc = $('pvDoc'); if (doc) doc.textContent = '无法加载内容'; });
      }
    }
    if (useGallery) {
      const go = (d) => { idx = (idx + d + gallery.length) % gallery.length; show(gallery[idx]); };
      $('pvPrev').onclick = () => go(-1);
      $('pvNext').onclick = () => go(1);
    }
    $('pvClose').onclick = closePreview;
    $('pvBackdrop').onclick = (e) => { if (e.target === $('pvBackdrop')) closePreview(); };
    pvKeyHandler = (e) => {
      if (e.key === 'Escape') closePreview();
      else if (useGallery && e.key === 'ArrowLeft') { idx = (idx - 1 + gallery.length) % gallery.length; show(gallery[idx]); }
      else if (useGallery && e.key === 'ArrowRight') { idx = (idx + 1) % gallery.length; show(gallery[idx]); }
    };
    document.addEventListener('keydown', pvKeyHandler);
    show(file);
  }

  // ---------------- 分享 ----------------
  function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(() => toast('已复制到剪贴板', 'ok'), () => fallbackCopy(text));
    } else fallbackCopy(text);
  }
  function fallbackCopy(text) {
    const ta = document.createElement('textarea');
    ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.focus(); ta.select();
    try { document.execCommand('copy'); toast('已复制到剪贴板', 'ok'); }
    catch { toast('复制失败，请手动选择复制', 'warn'); }
    ta.remove();
  }
  function shareLink(token) { return location.origin + '/share.html?t=' + token; }

  function doShare(kind, item) {
    const isFolder = kind === 'folder';
    const label = isFolder ? item.name : item.filename;
    openModal({
      title: '分享 “' + label + '”',
      confirmText: '创建分享链接',
      body:
        '<div class="field"><label>提取码</label>' +
        '<select class="input" id="shCodeMode">' +
          '<option value="none">无（任何人可访问）</option>' +
          '<option value="auto" selected>随机生成 4 位</option>' +
          '<option value="custom">自定义</option>' +
        '</select>' +
        '<input class="input" id="shCodeCustom" placeholder="字母或数字，最多 16 位" style="margin-top:8px;display:none">' +
        '</div>' +
        '<div class="field"><label>有效期</label>' +
        '<select class="input" id="shExpire">' +
          '<option value="0">永久有效</option>' +
          '<option value="1">1 天</option>' +
          '<option value="7" selected>7 天</option>' +
          '<option value="30">30 天</option>' +
        '</select></div>' +
        '<div class="field"><label>下载次数上限（0 = 不限）</label>' +
        '<input class="input" id="shMax" type="number" min="0" value="0"></div>',
      onOpen: () => {
        const sel = $('shCodeMode'), custom = $('shCodeCustom');
        sel.onchange = () => { custom.style.display = sel.value === 'custom' ? '' : 'none'; if (sel.value === 'custom') custom.focus(); };
      },
      onConfirm: async () => {
        const mode = $('shCodeMode').value;
        const opts = isFolder ? { folderId: item.id } : { fileId: item.id };
        opts.expireDays = parseInt($('shExpire').value, 10) || 0;
        opts.maxDownloads = Math.max(0, parseInt($('shMax').value, 10) || 0);
        if (mode === 'auto') opts.autoCode = true;
        else if (mode === 'custom') {
          const c = $('shCodeCustom').value.trim();
          if (!c) { toast('请输入自定义提取码', 'warn'); return false; }
          if (!/^[a-zA-Z0-9]{1,16}$/.test(c)) { toast('提取码仅限字母数字，最多 16 位', 'warn'); return false; }
          opts.code = c;
        }
        try {
          const res = await Api.shareCreate(opts);
          setTimeout(() => openShareResult(res, label), 0); // 等当前弹窗关闭后再打开结果弹窗
          return true;
        } catch (e) { toast(e.message || '创建失败', 'err'); return false; }
      },
    });
  }

  function openShareResult(res, label) {
    const link = shareLink(res.token);
    openModal({
      title: '分享已创建',
      confirmText: '完成',
      body:
        '<div class="share-result">' +
          '<div class="qr" id="shQr"></div>' +
          '<div class="sr-field"><label>分享链接</label>' +
            '<div class="sr-copy"><input class="input" id="shLink" readonly>' +
            '<button class="btn btn-sm btn-primary" id="shCopyLink">复制</button></div></div>' +
          '<div class="sr-field" id="shCodeWrap"><label>提取码</label>' +
            '<div class="sr-copy"><input class="input" id="shCodeVal" readonly>' +
            '<button class="btn btn-sm btn-ghost" id="shCopyBoth">复制链接+提取码</button></div></div>' +
          '<div class="sr-hint" id="shHint"></div>' +
        '</div>',
      onOpen: () => {
        $('shLink').value = link;
        try { $('shQr').innerHTML = window.QR.toSVG(link, { scale: 5, margin: 3 }); }
        catch (e) { $('shQr').textContent = '二维码生成失败'; }
        if (res.code) { $('shCodeVal').value = res.code; }
        else { $('shCodeWrap').style.display = 'none'; }
        const parts = [];
        parts.push(res.expireDays > 0 ? ('有效期 ' + res.expireDays + ' 天') : '永久有效');
        parts.push(res.maxDownloads > 0 ? ('限下载 ' + res.maxDownloads + ' 次') : '下载次数不限');
        $('shHint').textContent = parts.join(' · ');
        $('shCopyLink').onclick = () => copyText(link);
        const both = $('shCopyBoth');
        if (both) both.onclick = () => copyText('链接：' + link + (res.code ? ('\n提取码：' + res.code) : ''));
      },
      onConfirm: () => true,
    });
  }

  // ---------------- 我的分享 ----------------
  async function loadShares() {
    const body = $('shareBody');
    body.innerHTML = '<tr><td colspan="5"><div class="skeleton" style="width:50%"></div></td></tr>';
    $('sharesExtra').innerHTML = '';
    let data;
    try { data = await Api.shareMine(); } catch (e) { toast(e.message || '加载失败', 'err'); return; }
    const shares = data.shares || [];
    if (!shares.length) {
      body.innerHTML = '';
      $('sharesExtra').innerHTML = '<div class="empty">' + svg(SHARE_ICON) +
        '<h3>还没有分享</h3><p>在文件或文件夹上点击「分享」即可生成链接</p></div>';
      return;
    }
    body.innerHTML = '';
    shares.forEach(s => body.appendChild(shareRow(s)));
  }

  function shareRow(s) {
    const tr = document.createElement('tr');
    const link = shareLink(s.token);
    const status = s.expired ? '<span class="tag tag-off">已过期</span>'
      : (s.expireAt ? ('<span class="tag">到期 ' + escapeHtml(s.expireAt.split(' ')[0]) + '</span>') : '<span class="tag tag-on">永久</span>');
    const icon = s.isFolder ? '<span class="ftype folder">' + svg(FOLDER_ICON) + '</span>'
                            : '<span class="ftype" style="background:' + fileType(s.name).color + '">' + svg(fileType(s.name).icon) + '</span>';
    tr.innerHTML =
      '<td><div class="fname">' + icon + '<div style="min-width:0"><div class="txt"></div>' +
        '<div class="hash">' + status + (s.code ? ' <span class="tag">码 ' + escapeHtml(s.code) + '</span>' : '') + '</div></div></div></td>' +
      '<td class="hide-sm">' + s.views + ' 次浏览</td>' +
      '<td class="hide-sm">' + s.downloads + (s.maxDownloads > 0 ? ('/' + s.maxDownloads) : '') + ' 次下载</td>' +
      '<td class="hide-sm">' + escapeHtml((s.createdAt || '').split(' ')[0]) + '</td>' +
      '<td class="col-actions"><div class="row-actions">' +
        '<button class="act" data-act="copy" title="复制链接">' + svg('<rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>') + '</button>' +
        '<button class="act" data-act="open" title="打开">' + svg('<path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/><path d="M15 3h6v6M10 14 21 3"/>') + '</button>' +
        '<button class="act danger" data-act="cancel" title="取消分享">' + svg('<circle cx="12" cy="12" r="10"/><path d="m15 9-6 6M9 9l6 6"/>') + '</button>' +
      '</div></td>';
    tr.querySelector('.txt').textContent = s.name;
    tr.querySelector('[data-act="copy"]').onclick = () => copyText(s.code ? ('链接：' + link + '\n提取码：' + s.code) : link);
    tr.querySelector('[data-act="open"]').onclick = () => window.open(link, '_blank');
    tr.querySelector('[data-act="cancel"]').onclick = () => {
      openModal({
        title: '取消分享', danger: true, confirmText: '取消分享',
        body: '<p>确定要取消对 <b></b> 的分享吗？链接将立即失效。</p>',
        onOpen: () => { document.querySelector('#modalRoot .modal p b').textContent = s.name; },
        onConfirm: async () => {
          try { await Api.shareCancel(s.id); toast('已取消分享', 'ok'); loadShares(); }
          catch (e) { toast(e.message || '操作失败', 'err'); return false; }
        },
      });
    };
    return tr;
  }

  // ---------------- 新建文件夹 ----------------
  // ---------------- 离线下载 ----------------
  function openOffline() {
    openModal({
      title: '离线下载',
      body: '<div class="field"><label>文件链接 (http/https)</label><input class="input" id="offUrl" placeholder="https://example.com/file.zip"></div>' +
            '<div class="field"><label>另存为 (可选)</label><input class="input" id="offName" placeholder="留空则按链接自动命名"></div>' +
            '<div style="color:var(--muted);font-size:12px;line-height:1.5">下载在服务器后台进行，完成后自动出现在当前目录。</div>',
      onOpen: () => { $('offUrl').focus(); },
      onConfirm: async () => {
        const url = $('offUrl').value.trim();
        const name = $('offName').value.trim();
        if (!/^https?:\/\//i.test(url)) { toast('请输入有效的 http/https 链接', 'warn'); return false; }
        try {
          const r = await Api.offlineCreate(url, state.parentId, name);
          toast('已加入离线下载队列', 'ok');
          pollOffline(r.id);
        } catch (e) { toast(e.message || '创建失败', 'err'); return false; }
      },
    });
  }
  function pollOffline(id) {
    let tries = 0;
    const timer = setInterval(async () => {
      tries++;
      let tasks;
      try { tasks = (await Api.offlineList()).tasks; } catch { clearInterval(timer); return; }
      const t = tasks.find(x => x.id === id);
      if (t && t.status === 1) { clearInterval(timer); toast('离线下载完成: ' + t.filename, 'ok'); if (state.view === 'files') { loadList(); loadUser(); } }
      else if (t && t.status === 2) { clearInterval(timer); toast('离线下载失败: ' + (t.message || t.filename), 'err'); }
      if (tries > 150) clearInterval(timer);
    }, 2000);
  }

  $('newFolderBtn').onclick = () => {
    openModal({
      title: '新建文件夹',
      body: '<div class="field"><label>文件夹名称</label><input class="input" id="folderNameInput" placeholder="未命名文件夹"></div>',
      onOpen: () => { $('folderNameInput').focus(); },
      onConfirm: async () => {
        const name = $('folderNameInput').value.trim();
        if (!name) { toast('名称不能为空', 'warn'); return false; }
        try { await Api.folderCreate(name, state.parentId); toast('已创建', 'ok'); loadList(); }
        catch (e) { toast(e.message || '创建失败', 'err'); return false; }
      },
    });
  };

  // ---------------- 上传 ----------------
  function addUploadItem(name) {
    const el = document.createElement('div');
    el.className = 'up-item';
    el.innerHTML =
      '<div class="up-head">' + svg('<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="M7 10l5-5 5 5M12 5v13"/>') +
      '<span class="name"></span><span class="pct">0%</span></div>' +
      '<div class="up-bar"><div class="up-fill"></div></div>';
    el.querySelector('.name').textContent = name;
    $('uploads').prepend(el);
    return el;
  }
  function setProgress(el, ratio) {
    el.querySelector('.up-fill').style.width = (ratio * 100).toFixed(0) + '%';
    el.querySelector('.pct').textContent = (ratio * 100).toFixed(0) + '%';
  }
  function markDone(el, instant) {
    el.classList.add('done');
    el.querySelector('.up-fill').style.width = '100%';
    el.querySelector('.pct').innerHTML = instant ? '<span class="badge-instant">秒传</span>' : '✓';
    setTimeout(() => { el.style.transition = 'opacity .4s'; el.style.opacity = '0'; setTimeout(() => el.remove(), 400); }, 2500);
  }
  function markErr(el, msg) {
    el.classList.add('err');
    el.querySelector('.pct').textContent = '失败';
    toast(msg || '上传失败', 'err');
  }

  function xhrUpload(file, el, parentId) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/file/upload?parentId=' + encodeURIComponent(parentId));
      xhr.setRequestHeader('Authorization', 'Bearer ' + Store.token);
      xhr.upload.onprogress = (e) => { if (e.lengthComputable) setProgress(el, e.loaded / e.total); };
      xhr.onload = () => {
        let d = null; try { d = JSON.parse(xhr.responseText); } catch {}
        if (xhr.status >= 200 && xhr.status < 300 && d && d.code === 0) resolve(d.data);
        else if (xhr.status === 401) { Store.clear(); location.href = '/'; }
        else reject(d || { message: '上传失败 (' + xhr.status + ')' });
      };
      xhr.onerror = () => reject({ message: '网络错误' });
      const fd = new FormData();
      fd.append('file', file, file.name);
      xhr.send(fd);
    });
  }

  const CHUNK_SIZE = 4 * 1024 * 1024;   // 4MB 分片
  const CHUNK_THRESHOLD = CHUNK_SIZE;   // 超过该阈值走分片/断点续传

  // 分片 / 断点续传上传 (hash 已在外部算好)
  async function chunkedUpload(file, el, parentId, hash) {
    const size = file.size;
    const total = Math.max(1, Math.ceil(size / CHUNK_SIZE));
    el.querySelector('.pct').textContent = '准备…';
    const init = await Api.uploadInit({ filename: file.name, hash, size, parentId, chunkSize: CHUNK_SIZE, totalChunks: total });
    if (init.instant) { markDone(el, true); return; }
    const uploadId = init.uploadId;
    const done = new Set(init.uploaded || []);
    let uploadedBytes = 0;
    done.forEach(i => { uploadedBytes += Math.min(CHUNK_SIZE, size - i * CHUNK_SIZE); });
    const prog = () => setProgress(el, size ? Math.min(1, uploadedBytes / size) : 1);
    prog();
    for (let i = 0; i < total; i++) {
      if (done.has(i)) continue;
      const start = i * CHUNK_SIZE;
      const blob = file.slice(start, Math.min(size, start + CHUNK_SIZE));
      let res;
      try {
        res = await fetch(Api.uploadChunkUrl(uploadId, i), {
          method: 'POST',
          headers: { 'Authorization': 'Bearer ' + Store.token, 'Content-Type': 'application/octet-stream' },
          body: blob,
        });
      } catch (e) { throw { message: '网络错误' }; }
      if (res.status === 401) { Store.clear(); location.href = '/'; return; }
      if (!res.ok) throw { message: '分片 ' + i + ' 上传失败' };
      uploadedBytes += blob.size; prog();
    }
    el.querySelector('.pct').textContent = '合并…';
    const c = await Api.uploadComplete(uploadId);
    markDone(el, !!c.instant);
  }

  async function uploadOne(file, parentId) {
    const el = addUploadItem(file.webkitRelativePath || file.name);
    try {
      el.querySelector('.pct').textContent = '校验…';
      const hash = await window.hashFile(file);
      if (file.size > CHUNK_THRESHOLD) {
        await chunkedUpload(file, el, parentId, hash);
      } else {
        const r = await Api.instant(file.name, hash, file.size, parentId);
        if (r.instant) { markDone(el, true); return; }
        await xhrUpload(file, el, parentId);
        markDone(el, false);
      }
    } catch (e) {
      markErr(el, e.message);
    }
  }

  async function handleFiles(fileList) {
    const files = Array.from(fileList);
    if (!files.length) return;
    const parentId = state.parentId;      // 锁定上传目标目录
    for (const f of files) await uploadOne(f, parentId);
    state.page = 0;
    loadList(); loadUser();
  }

  // 文件夹上传: 依据 webkitRelativePath 递归建目录后逐个上传
  async function handleFolderUpload(fileList) {
    const files = Array.from(fileList);
    if (!files.length) return;
    const baseParent = state.parentId;
    const dirCache = new Map(); dirCache.set('', baseParent);
    async function ensureDir(relDir) {
      if (dirCache.has(relDir)) return dirCache.get(relDir);
      const id = (await Api.folderEnsure(relDir, baseParent)).id;
      dirCache.set(relDir, id);
      return id;
    }
    for (const f of files) {
      const rel = f.webkitRelativePath || f.name;
      const slash = rel.lastIndexOf('/');
      const dir = slash >= 0 ? rel.slice(0, slash) : '';
      let pid = baseParent;
      try { pid = await ensureDir(dir); } catch (e) { toast('创建目录失败: ' + dir, 'err'); continue; }
      await uploadOne(f, pid);
    }
    state.page = 0;
    loadList(); loadUser();
    toast('文件夹上传完成', 'ok');
  }

  // ---------------- 拖拽 ----------------
  const dropzone = $('dropzone');
  const overlay = $('dragOverlay');
  let dragCounter = 0;
  ['dragenter','dragover','dragleave','drop'].forEach(ev =>
    window.addEventListener(ev, e => { e.preventDefault(); e.stopPropagation(); }, false));
  window.addEventListener('dragenter', () => { if (state.view !== 'files') return; dragCounter++; overlay.classList.add('show'); });
  window.addEventListener('dragleave', () => { if (--dragCounter <= 0) overlay.classList.remove('show'); });
  window.addEventListener('drop', (e) => { dragCounter = 0; overlay.classList.remove('show'); dropzone.classList.remove('drag'); if (state.view === 'files') handleFiles(e.dataTransfer.files); });
  dropzone.addEventListener('dragenter', () => dropzone.classList.add('drag'));
  dropzone.addEventListener('dragleave', () => dropzone.classList.remove('drag'));
  dropzone.addEventListener('click', () => $('fileInput').click());

  // ---------------- 视图切换 ----------------
  function setNav(active) {
    ['navFiles', 'navFav', 'navStats', 'navTags', 'navActivity', 'navTrash', 'navShares', 'navAccount', 'navAdmin'].forEach(id => { const el = $(id); if (el) el.classList.toggle('active', id === active); });
    $('filesView').hidden = active !== 'navFiles';
    const fv = $('favView'); if (fv) fv.hidden = active !== 'navFav';
    const stv = $('statsView'); if (stv) stv.hidden = active !== 'navStats';
    const tgv = $('tagsView'); if (tgv) tgv.hidden = active !== 'navTags';
    const acv = $('activityView'); if (acv) acv.hidden = active !== 'navActivity';
    $('trashView').hidden = active !== 'navTrash';
    const sv = $('sharesView'); if (sv) sv.hidden = active !== 'navShares';
    const av = $('accountView'); if (av) av.hidden = active !== 'navAccount';
    const dv = $('adminView'); if (dv) dv.hidden = active !== 'navAdmin';
    const showFilesTools = active === 'navFiles';
    $('filesToolbar').style.display = showFilesTools ? '' : 'none';
    $('searchWrap').style.display = showFilesTools ? '' : 'none';
  }
  function showFiles() {
    state.view = 'files';
    setNav('navFiles');
    $('pageTitle').textContent = '我的文件';
    loadList();
  }
  function showTrash() {
    state.view = 'trash';
    setNav('navTrash');
    $('pageTitle').textContent = '回收站';
    loadTrash();
  }
  function showFav() {
    state.view = 'fav';
    setNav('navFav');
    $('pageTitle').textContent = '收藏';
    loadFav();
  }
  async function loadFav() {
    const body = $('favBody');
    body.innerHTML = '<tr><td colspan="4" class="empty-cell">加载中…</td></tr>';
    try {
      const r = await Api.favList();
      const items = r.items || [];
      if (!items.length) { body.innerHTML = '<tr><td colspan="4" class="empty-cell">还没有收藏任何内容</td></tr>'; return; }
      body.innerHTML = items.map(it => {
        const isFolder = it.type === 1;
        const icon = isFolder
          ? '<svg class="icon fav-ic" viewBox="0 0 24 24"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>'
          : '<svg class="icon fav-ic" viewBox="0 0 24 24"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/></svg>';
        const open = isFolder
          ? '<button class="btn btn-sm btn-ghost" data-open="' + it.parentId + '">前往目录</button>'
          : '<button class="btn btn-sm btn-ghost" data-dl="' + it.id + '">下载</button>';
        return '<tr>' +
          '<td>' + icon + escapeHtml(it.name) + '</td>' +
          '<td class="hide-sm">' + (isFolder ? '—' : humanSize(it.size)) + '</td>' +
          '<td class="hide-sm muted">' + escapeHtml(it.favoritedAt || '') + '</td>' +
          '<td class="col-actions">' + open +
          ' <button class="btn btn-sm btn-ghost" data-unfav="' + it.type + ':' + it.id + '">取消收藏</button></td>' +
          '</tr>';
      }).join('');
      body.querySelectorAll('[data-unfav]').forEach(b => b.onclick = async () => {
        const [t, id] = b.dataset.unfav.split(':').map(Number);
        try { await Api.favToggle(t, id); toast('已取消收藏', 'ok'); loadFav(); }
        catch (err) { toast(err.message || '操作失败', 'err'); }
      });
      body.querySelectorAll('[data-dl]').forEach(b => b.onclick = () => { location.href = Api.downloadUrl(Number(b.dataset.dl)); });
      body.querySelectorAll('[data-open]').forEach(b => b.onclick = () => { state.parentId = Number(b.dataset.open); showFiles(); });
    } catch (err) { body.innerHTML = '<tr><td colspan="4" class="empty-cell">加载失败</td></tr>'; }
  }
  function showShares() {
    state.view = 'shares';
    setNav('navShares');
    $('pageTitle').textContent = '我的分享';
    loadShares();
  }
  function showStats() {
    state.view = 'stats';
    setNav('navStats');
    $('pageTitle').textContent = '统计分析';
    loadStats();
  }
  const CAT_META = {
    image:    { label: '图片', color: '#6366f1' },
    video:    { label: '视频', color: '#ec4899' },
    audio:    { label: '音频', color: '#f59e0b' },
    document: { label: '文档', color: '#10b981' },
    archive:  { label: '压缩包', color: '#8b5cf6' },
    code:     { label: '代码', color: '#0ea5e9' },
    other:    { label: '其他', color: '#94a3b8' },
  };
  async function loadStats() {
    const kpi = $('statsKpis'); const tp = $('statsTypes'); const lg = $('statsLargest'); const tl = $('statsTimeline');
    kpi.innerHTML = '<div class="empty-cell">加载中…</div>'; tp.innerHTML = ''; lg.innerHTML = ''; tl.innerHTML = '';
    try {
      const s = await Api.stats();
      const used = s.logicalSize || 0, quota = s.quota || 1, phys = s.physicalSize || 0;
      const saved = Math.max(0, used - phys);
      const pct = quota > 0 ? Math.min(100, Math.round(used / quota * 100)) : 0;
      const cards = [
        { label: '已用空间', value: humanSize(used), sub: pct + '% / ' + humanSize(quota) },
        { label: '文件数', value: s.files || 0, sub: (s.folders || 0) + ' 个文件夹' },
        { label: '去重节省', value: humanSize(saved), sub: '实际占用 ' + humanSize(phys) },
        { label: '分享 / 收藏', value: (s.shares || 0) + ' / ' + (s.favorites || 0), sub: (s.versions || 0) + ' 个历史版本' },
        { label: '回收站', value: s.trash || 0, sub: '可恢复项目' },
      ];
      kpi.innerHTML = cards.map(c =>
        '<div class="kpi-card"><div class="kpi-value">' + escapeHtml(String(c.value)) + '</div>' +
        '<div class="kpi-label">' + escapeHtml(c.label) + '</div>' +
        '<div class="kpi-sub">' + escapeHtml(c.sub) + '</div></div>').join('');

      const types = (s.fileTypes || []).slice().sort((a, b) => b.size - a.size);
      const maxSize = types.reduce((m, t) => Math.max(m, t.size), 0) || 1;
      tp.innerHTML = types.length ? types.map(t => {
        const m = CAT_META[t.category] || CAT_META.other;
        const w = Math.max(3, Math.round(t.size / maxSize * 100));
        return '<div class="bar-row"><span class="bar-name">' + m.label + '</span>' +
          '<div class="bar-track"><div class="bar-fill" style="width:' + w + '%;background:' + m.color + '"></div></div>' +
          '<span class="bar-val">' + t.count + ' 个 · ' + humanSize(t.size) + '</span></div>';
      }).join('') : '<div class="empty-cell">暂无文件</div>';

      const largest = s.largest || [];
      lg.innerHTML = largest.length ? largest.map(f =>
        '<div class="lg-row"><span class="lg-name" title="' + escapeHtml(f.filename) + '">' + escapeHtml(f.filename) + '</span>' +
        '<a class="lg-size" href="' + Api.downloadUrl(f.id) + '">' + humanSize(f.size) + '</a></div>').join('')
        : '<div class="empty-cell">暂无文件</div>';

      // 补齐近 14 天
      const map = {}; (s.timeline || []).forEach(x => map[x.date] = x.count);
      const days = []; const today = new Date();
      for (let i = 13; i >= 0; i--) { const d = new Date(today); d.setDate(d.getDate() - i); days.push(d.toISOString().slice(0, 10)); }
      const maxC = days.reduce((m, d) => Math.max(m, map[d] || 0), 0) || 1;
      tl.innerHTML = days.map(d => {
        const c = map[d] || 0; const h = Math.round(c / maxC * 100);
        return '<div class="tl-col" title="' + d + '：' + c + ' 个"><div class="tl-bar" style="height:' + Math.max(2, h) + '%"></div>' +
          '<span class="tl-lbl">' + d.slice(5) + '</span></div>';
      }).join('');
    } catch (e) { kpi.innerHTML = '<div class="empty-cell">加载失败</div>'; }
  }
  function showAccount() {
    state.view = 'account';
    setNav('navAccount');
    $('pageTitle').textContent = '账户设置';
    loadAccount();
  }
  function showAdmin() {
    state.view = 'admin';
    setNav('navAdmin');
    $('pageTitle').textContent = '管理后台';
    loadAdmin();
  }
  $('navFiles').onclick = showFiles;
  { const el = $('navFav'); if (el) el.onclick = showFav; }
  { const el = $('navStats'); if (el) el.onclick = showStats; }
  { const el = $('navTags'); if (el) el.onclick = showTags; }
  { const el = $('navActivity'); if (el) el.onclick = showActivity; }
  { const el = $('activityFilter'); if (el) el.onchange = loadActivity; }
  $('navTrash').onclick = showTrash;
  { const el = $('navShares'); if (el) el.onclick = showShares; }
  { const el = $('navAccount'); if (el) el.onclick = showAccount; }
  { const el = $('navAdmin'); if (el) el.onclick = showAdmin; }

  // ---------------- 账户设置 ----------------
  function loadAccount() {
    const me = state.me || {};
    $('acctNickname').value = me.nickname || '';
    $('acctEmail').value = me.email || '';
    const img = $('acctAvatar'), fb = $('acctAvatarFallback');
    if (me.hasAvatar) {
      img.src = Api.avatarUrl(me.uid, me.avatar); img.hidden = false; fb.hidden = true;
    } else {
      img.hidden = true; fb.hidden = false; fb.textContent = ((me.nickname || me.username || '?')[0] || '?').toUpperCase();
    }
    const on = !!me.twoFactor;
    $('tfaStatus').textContent = on ? '两步验证已启用，登录时需输入动态验证码。' : '未启用。启用后可显著提升账户安全性。';
    $('tfaEnableBtn').hidden = on;
    $('tfaDisableBtn').hidden = !on;
    $('tfaSetupArea').innerHTML = '';
    loadTokens();
    const wd = $('webdavUrl');
    if (wd) wd.textContent = location.origin + '/webdav/';
  }

  // ---------------- 开发者 · API 令牌 ----------------
  async function loadTokens() {
    const body = $('tokBody'); if (!body) return;
    body.innerHTML = '<tr><td colspan="5" class="empty-cell">加载中…</td></tr>';
    try {
      const r = await Api.tokenList();
      const list = r.tokens || [];
      if (!list.length) { body.innerHTML = '<tr><td colspan="5" class="empty-cell">暂无令牌</td></tr>'; return; }
      body.innerHTML = list.map(t => {
        const exp = t.expiresAt ? (t.expired ? '<span class="badge badge-off">已过期</span>' : escapeHtml(t.expiresAt)) : '永久';
        return '<tr>' +
          '<td>' + escapeHtml(t.name) + '</td>' +
          '<td class="hide-sm"><code>' + escapeHtml(t.prefix) + '…</code></td>' +
          '<td class="hide-sm muted">' + (t.lastUsedAt ? escapeHtml(t.lastUsedAt) : '从未') + '</td>' +
          '<td class="hide-sm">' + exp + '</td>' +
          '<td class="col-actions"><button class="btn btn-sm btn-ghost" data-revoke="' + t.id + '">吊销</button></td>' +
          '</tr>';
      }).join('');
      body.querySelectorAll('[data-revoke]').forEach(b => b.onclick = async () => {
        if (!confirm('确定吊销该令牌？使用它的客户端将立即失效。')) return;
        try { await Api.tokenRevoke(Number(b.dataset.revoke)); toast('令牌已吊销', 'ok'); loadTokens(); }
        catch (err) { toast(err.message || '吊销失败', 'err'); }
      });
    } catch (err) { body.innerHTML = '<tr><td colspan="5" class="empty-cell">加载失败</td></tr>'; }
  }
  {
    const btn = $('tokCreateBtn');
    if (btn) btn.onclick = async () => {
      const name = ($('tokName').value || '').trim() || 'token';
      const days = Number($('tokExpiry').value || 0);
      try {
        const r = await Api.tokenCreate(name, days);
        $('tokName').value = '';
        $('tokNew').innerHTML =
          '<div class="tok-reveal"><p class="acct-hint">令牌已生成，仅显示一次，请立即复制保存：</p>' +
          '<code class="tfa-secret" id="tokRevealVal">' + escapeHtml(r.token) + '</code>' +
          '<button class="btn btn-sm btn-primary" id="tokCopyBtn" type="button">复制</button></div>';
        $('tokCopyBtn').onclick = () => {
          navigator.clipboard && navigator.clipboard.writeText(r.token);
          toast('已复制到剪贴板', 'ok');
        };
        loadTokens();
      } catch (err) { toast(err.message || '生成失败', 'err'); }
    };
  }
  $('avatarUploadBtn').onclick = () => $('avatarInput').click();
  $('avatarInput').onchange = async (e) => {
    const f = e.target.files[0]; e.target.value = '';
    if (!f) return;
    try { await Api.avatarUpload(f); toast('头像已更新', 'ok'); await loadUser(); loadAccount(); }
    catch (err) { toast(err.message || '上传失败', 'err'); }
  };
  $('saveProfileBtn').onclick = async () => {
    try {
      await Api.profileUpdate($('acctNickname').value.trim(), $('acctEmail').value.trim());
      toast('资料已保存', 'ok'); await loadUser();
    } catch (err) { toast(err.message || '保存失败', 'err'); }
  };
  $('changePwBtn').onclick = async () => {
    const oldPw = $('oldPw').value, newPw = $('newPw').value, newPw2 = $('newPw2').value;
    if (newPw.length < 6) { toast('新密码至少 6 位', 'warn'); return; }
    if (newPw !== newPw2) { toast('两次输入的新密码不一致', 'warn'); return; }
    try {
      await Api.passwordChange(oldPw, newPw);
      toast('密码已修改，请重新登录', 'ok');
      setTimeout(() => { Store.clear(); location.href = '/'; }, 1200);
    } catch (err) { toast(err.message || '修改失败', 'err'); }
  };
  $('tfaEnableBtn').onclick = async () => {
    try {
      const r = await Api.twofaSetup();
      const svg = window.QR ? window.QR.toSVG(r.otpauth, { scale: 4, margin: 2 }) : '';
      $('tfaSetupArea').innerHTML =
        '<div class="tfa-setup">' +
        '<div class="tfa-qr">' + svg + '</div>' +
        '<p class="acct-hint">用 Google Authenticator / 1Password 等扫码，或手动输入密钥：</p>' +
        '<code class="tfa-secret">' + escapeHtml(r.secret) + '</code>' +
        '<div class="field"><label>输入验证器显示的 6 位动态码</label><input class="input" id="tfaCode" maxlength="6" inputmode="numeric" placeholder="000000"></div>' +
        '<button class="btn btn-primary" id="tfaConfirmBtn" type="button">确认启用</button></div>';
      $('tfaConfirmBtn').onclick = async () => {
        const code = ($('tfaCode').value || '').trim();
        try { await Api.twofaEnable(code); toast('两步验证已启用', 'ok'); await loadUser(); loadAccount(); }
        catch (err) { toast(err.message || '启用失败', 'err'); }
      };
      $('tfaCode').focus();
    } catch (err) { toast(err.message || '获取密钥失败', 'err'); }
  };
  $('tfaDisableBtn').onclick = async () => {
    const code = window.prompt('请输入当前验证器中的 6 位动态码以关闭两步验证：');
    if (!code) return;
    try { await Api.twofaDisable(code.trim()); toast('两步验证已关闭', 'ok'); await loadUser(); loadAccount(); }
    catch (err) { toast(err.message || '关闭失败', 'err'); }
  };

  // ---------------- 管理后台 ----------------
  function loadAdmin() {
    document.querySelectorAll('.admin-tab').forEach(t => t.classList.toggle('active', t.dataset.atab === 'stats'));
    ['adminStats', 'adminUsers', 'adminAudit'].forEach(id => { $(id).hidden = id !== 'adminStats'; });
    loadAdminStats();
  }
  document.querySelectorAll('.admin-tab').forEach(tab => {
    tab.onclick = () => {
      const t = tab.dataset.atab;
      document.querySelectorAll('.admin-tab').forEach(x => x.classList.toggle('active', x === tab));
      $('adminStats').hidden = t !== 'stats';
      $('adminUsers').hidden = t !== 'users';
      $('adminAudit').hidden = t !== 'audit';
      if (t === 'stats') loadAdminStats();
      else if (t === 'users') loadAdminUsers();
      else loadAdminAudit();
    };
  });
  async function loadAdminStats() {
    const box = $('adminStats');
    box.innerHTML = '<div class="skeleton" style="width:60%;height:80px"></div>';
    try {
      const s = await Api.adminStats();
      const card = (label, val) => '<div class="stat-card"><div class="stat-val">' + val + '</div><div class="stat-label">' + label + '</div></div>';
      box.innerHTML = '<div class="stat-grid">' +
        card('用户总数', s.users) +
        card('文件总数', s.files) +
        card('存储用量', humanSize(s.storage)) +
        card('去重块数', s.blobs) +
        card('分享数', s.shares) +
        card('离线任务', s.offline) +
        card('管理员', s.admins) +
        '</div>';
    } catch (e) { box.innerHTML = '<div class="empty"><h3>' + escapeHtml(e.message || '加载失败') + '</h3></div>'; }
  }
  async function loadAdminUsers() {
    const body = $('adminUserBody');
    body.innerHTML = '<tr><td colspan="6"><div class="skeleton" style="width:50%"></div></td></tr>';
    try {
      const d = await Api.adminUsers(200, 0);
      body.innerHTML = '';
      (d.users || []).forEach(u => {
        const tr = document.createElement('tr');
        const roleBadge = u.role === 1 ? '<span class="badge badge-admin">管理员</span>' : '普通';
        const disBadge = u.disabled ? ' <span class="badge badge-off">已禁用</span>' : '';
        tr.innerHTML =
          '<td><b>' + escapeHtml(u.username) + '</b>' + (u.nickname ? ' <span class="muted">(' + escapeHtml(u.nickname) + ')</span>' : '') + disBadge + '</td>' +
          '<td class="hide-sm">' + roleBadge + '</td>' +
          '<td class="hide-sm">' + humanSize(u.storageUsed) + ' / ' + humanSize(u.quota) + '</td>' +
          '<td class="hide-sm">' + (u.twoFactor ? '✓' : '—') + '</td>' +
          '<td class="hide-sm">' + escapeHtml((u.createdAt || '').split(' ')[0]) + '</td>' +
          '<td class="col-actions"></td>';
        const act = tr.querySelector('.col-actions');
        const mkBtn = (txt, fn) => { const b = document.createElement('button'); b.className = 'btn btn-ghost btn-sm'; b.textContent = txt; b.onclick = fn; return b; };
        act.appendChild(mkBtn('配额', () => editQuota(u)));
        act.appendChild(mkBtn(u.role === 1 ? '取消管理员' : '设为管理员', () => setRole(u)));
        act.appendChild(mkBtn(u.disabled ? '启用' : '禁用', () => setDisabled(u)));
        body.appendChild(tr);
      });
      if (!body.children.length) body.innerHTML = '<tr><td colspan="6" class="empty-cell">暂无用户</td></tr>';
    } catch (e) { body.innerHTML = '<tr><td colspan="6" class="empty-cell">' + escapeHtml(e.message || '加载失败') + '</td></tr>'; }
  }
  function editQuota(u) {
    openModal({
      title: '设置配额 · ' + u.username,
      body: '<div class="field"><label>配额 (GB，填 0 表示使用全局默认)</label>' +
            '<input class="input" id="quotaInput" type="number" min="0" step="0.5" value="' + (u.quota / 1073741824).toFixed(2) + '"></div>',
      onOpen: () => $('quotaInput').focus(),
      onConfirm: async () => {
        const gb = parseFloat($('quotaInput').value);
        if (isNaN(gb) || gb < 0) { toast('请输入有效数值', 'warn'); return false; }
        try { await Api.adminSetQuota(u.id, Math.round(gb * 1073741824)); toast('配额已更新', 'ok'); loadAdminUsers(); }
        catch (e) { toast(e.message || '失败', 'err'); return false; }
      },
    });
  }
  async function setRole(u) {
    try { await Api.adminSetRole(u.id, u.role === 1 ? 0 : 1); toast('角色已更新', 'ok'); loadAdminUsers(); }
    catch (e) { toast(e.message || '失败', 'err'); }
  }
  async function setDisabled(u) {
    try { await Api.adminSetDisabled(u.id, !u.disabled); toast(u.disabled ? '已启用' : '已禁用', 'ok'); loadAdminUsers(); }
    catch (e) { toast(e.message || '失败', 'err'); }
  }
  async function loadAdminAudit() {
    const body = $('adminAuditBody');
    body.innerHTML = '<tr><td colspan="5"><div class="skeleton" style="width:50%"></div></td></tr>';
    try {
      const d = await Api.adminAudit(200, 0);
      body.innerHTML = '';
      (d.logs || []).forEach(l => {
        const tr = document.createElement('tr');
        tr.innerHTML =
          '<td>' + escapeHtml(l.createdAt || '') + '</td>' +
          '<td>' + escapeHtml(l.username || ('#' + l.uid)) + '</td>' +
          '<td><span class="badge badge-act">' + escapeHtml(l.action) + '</span></td>' +
          '<td class="hide-sm">' + escapeHtml(l.detail || '') + '</td>' +
          '<td class="hide-sm">' + escapeHtml(l.ip || '') + '</td>';
        body.appendChild(tr);
      });
      if (!body.children.length) body.innerHTML = '<tr><td colspan="5" class="empty-cell">暂无日志</td></tr>';
    } catch (e) { body.innerHTML = '<tr><td colspan="5" class="empty-cell">' + escapeHtml(e.message || '加载失败') + '</td></tr>'; }
  }

  // ---------------- 回收站 ----------------
  async function loadTrash() {
    const body = $('trashBody');
    body.innerHTML = '<tr><td colspan="4"><div class="skeleton" style="width:50%"></div></td></tr>';
    let data;
    try { data = await Api.trashList(); } catch (e) { toast(e.message || '加载失败', 'err'); return; }
    const folders = data.folders || [], files = data.files || [];
    if (!folders.length && !files.length) {
      body.innerHTML = '';
      $('trashExtra').innerHTML = '<div class="empty">' + svg('<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>') +
        '<h3>回收站是空的</h3><p>删除的文件和文件夹会出现在这里</p></div>';
      return;
    }
    $('trashExtra').innerHTML = '';
    body.innerHTML = '';
    folders.forEach(f => body.appendChild(trashRow('folder', f)));
    files.forEach(f => body.appendChild(trashRow('file', f)));
  }

  function trashRow(kind, f) {
    const tr = document.createElement('tr');
    const isFolder = kind === 'folder';
    const icon = isFolder ? '<span class="ftype folder">' + svg(FOLDER_ICON) + '</span>'
                          : '<span class="ftype" style="background:' + fileType(f.filename).color + '">' + svg(fileType(f.filename).icon) + '</span>';
    tr.innerHTML =
      '<td><div class="fname">' + icon + '<div style="min-width:0"><div class="txt"></div></div></div></td>' +
      '<td class="hide-sm">' + (isFolder ? '—' : humanSize(f.size)) + '</td>' +
      '<td class="hide-sm">' + escapeHtml(f.deletedAt || '') + '</td>' +
      '<td class="col-actions"><div class="row-actions">' +
        '<button class="act" data-act="restore" title="恢复">' + svg('<path d="M3 7v6h6"/><path d="M3 13a9 9 0 1 0 3-7.7L3 8"/>') + '</button>' +
        '<button class="act danger" data-act="purge" title="彻底删除">' + svg('<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>') + '</button>' +
      '</div></td>';
    tr.querySelector('.txt').textContent = isFolder ? f.name : f.filename;
    const ids = isFolder ? [[], [f.id]] : [[f.id], []];
    tr.querySelector('[data-act="restore"]').onclick = async () => {
      try { await Api.trashRestore(ids[0], ids[1]); toast('已恢复', 'ok'); loadTrash(); loadUser(); }
      catch (e) { toast(e.message || '恢复失败', 'err'); }
    };
    tr.querySelector('[data-act="purge"]').onclick = () => {
      openModal({
        title: '彻底删除', danger: true, confirmText: '彻底删除',
        body: '<p>确定要彻底删除 <b></b> 吗？此操作<strong>不可恢复</strong>。</p>',
        onOpen: () => { document.querySelector('#modalRoot .modal p b').textContent = isFolder ? f.name : f.filename; },
        onConfirm: async () => {
          try { await Api.trashDelete(ids[0], ids[1]); toast('已彻底删除', 'ok'); loadTrash(); loadUser(); }
          catch (e) { toast(e.message || '删除失败', 'err'); return false; }
        },
      });
    };
    return tr;
  }

  $('emptyTrashBtn').onclick = () => {
    openModal({
      title: '清空回收站', danger: true, confirmText: '清空',
      body: '<p>确定要清空回收站吗？其中所有项目将被<strong>彻底删除且不可恢复</strong>。</p>',
      onConfirm: async () => {
        try { await Api.trashEmpty(); toast('回收站已清空', 'ok'); loadTrash(); loadUser(); }
        catch (e) { toast(e.message || '清空失败', 'err'); return false; }
      },
    });
  };

  // ---------------- 搜索 / 排序 ----------------
  let searchTimer;
  $('searchInput').addEventListener('input', (e) => {
    clearTimeout(searchTimer);
    const val = e.target.value.trim();
    searchTimer = setTimeout(() => {
      if ($('searchScope') && $('searchScope').value === 'global') {
        runGlobalSearch(val);
      } else {
        state.keyword = val; state.page = 0; clearSelection(); loadList();
      }
    }, 300);
  });
  { const sc = $('searchScope'); if (sc) sc.addEventListener('change', () => {
    const val = $('searchInput').value.trim();
    const global = sc.value === 'global';
    $('searchInput').placeholder = global ? '搜索全部文件与文件夹…' : '搜索当前目录…';
    if (global) runGlobalSearch(val);
    else { state.keyword = val; state.page = 0; clearSelection(); loadList(); }
  }); }
  async function runGlobalSearch(q) {
    const body = $('fileBody');
    $('listExtra').innerHTML = '';
    clearSelection();
    if (!q) { body.innerHTML = '<tr><td colspan="5" class="empty-cell">输入关键词，跨全部目录搜索</td></tr>'; return; }
    body.innerHTML = '<tr><td colspan="5" class="empty-cell">搜索中…</td></tr>';
    try {
      const r = await Api.search(q);
      const items = r.items || [];
      if (!items.length) { body.innerHTML = '<tr><td colspan="5" class="empty-cell">没有匹配「' + escapeHtml(q) + '」的项目</td></tr>'; return; }
      body.innerHTML = items.map(it => {
        const isFolder = it.type === 1;
        const icon = isFolder
          ? '<svg class="icon fav-ic" viewBox="0 0 24 24"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>'
          : '<svg class="icon fav-ic" viewBox="0 0 24 24"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/></svg>';
        const act = isFolder
          ? '<button class="btn btn-sm btn-ghost" data-go="' + it.id + '">打开</button>'
          : '<button class="btn btn-sm btn-ghost" data-gop="' + it.parentId + '">前往目录</button>' +
            ' <button class="btn btn-sm btn-ghost" data-dl="' + it.id + '">下载</button>';
        return '<tr>' +
          '<td colspan="2">' + icon + escapeHtml(it.name) + '</td>' +
          '<td class="hide-sm">' + (isFolder ? '文件夹' : humanSize(it.size)) + '</td>' +
          '<td class="hide-sm muted">全局</td>' +
          '<td class="col-actions">' + act + '</td>' +
          '</tr>';
      }).join('');
      body.querySelectorAll('[data-go]').forEach(b => b.onclick = () => { state.parentId = Number(b.dataset.go); resetSearchToCurrent(); showFiles(); });
      body.querySelectorAll('[data-gop]').forEach(b => b.onclick = () => { state.parentId = Number(b.dataset.gop); resetSearchToCurrent(); showFiles(); });
      body.querySelectorAll('[data-dl]').forEach(b => b.onclick = () => { location.href = Api.downloadUrl(Number(b.dataset.dl)); });
    } catch (err) { body.innerHTML = '<tr><td colspan="5" class="empty-cell">搜索失败</td></tr>'; }
  }
  function resetSearchToCurrent() {
    const sc = $('searchScope'); if (sc) sc.value = 'cur';
    $('searchInput').value = ''; state.keyword = '';
    $('searchInput').placeholder = '搜索当前目录…';
  }
  $('sortSelect').addEventListener('change', (e) => {
    const [s, o] = e.target.value.split(':');
    state.sort = s; state.order = o; state.page = 0; loadList();
  });
  document.querySelectorAll('#filesView table.files thead th[data-sort]').forEach(th => {
    th.onclick = () => {
      const col = th.dataset.sort;
      if (state.sort === col) state.order = state.order === 'asc' ? 'desc' : 'asc';
      else { state.sort = col; state.order = 'asc'; }
      $('sortSelect').value = state.sort + ':' + state.order;
      state.page = 0; loadList();
    };
  });

  // ---------------- 事件绑定 ----------------
  $('uploadBtn').onclick = () => $('fileInput').click();
  $('fileInput').onchange = (e) => { handleFiles(e.target.files); e.target.value = ''; };
  $('folderUploadBtn').onclick = () => $('folderInput').click();
  $('folderInput').onchange = (e) => { handleFolderUpload(e.target.files); e.target.value = ''; };
  $('offlineBtn').onclick = openOffline;

  const themeLabel = $('themeLabel');
  function syncTheme() { themeLabel.textContent = Theme.get() === 'dark' ? '浅色模式' : '深色模式'; }
  syncTheme();
  $('themeToggle').onclick = () => { Theme.toggle(); syncTheme(); };
  $('logoutBtn').onclick = () => { Store.clear(); location.href = '/'; };

  // ---------------- Modal ----------------
  function openModal(opts) {
    const root = $('modalRoot');
    root.innerHTML =
      '<div class="modal"><h3></h3><div class="mbody"></div>' +
      '<div class="modal-actions">' +
      '<button class="btn btn-ghost" data-x="cancel">取消</button>' +
      '<button class="btn ' + (opts.danger ? 'btn-danger' : 'btn-primary') + '" data-x="ok"></button>' +
      '</div></div>';
    root.querySelector('h3').textContent = opts.title;
    root.querySelector('.mbody').innerHTML = opts.body || '';
    root.querySelector('[data-x="ok"]').textContent = opts.confirmText || '确定';
    root.classList.add('show');
    if (opts.onOpen) opts.onOpen();

    function close() { root.classList.remove('show'); root.innerHTML = ''; document.removeEventListener('keydown', onKey); }
    async function confirm() {
      const btn = root.querySelector('[data-x="ok"]');
      btn.disabled = true;
      const res = await opts.onConfirm();
      if (res === false) { btn.disabled = false; return; }
      close();
    }
    function onKey(e) { if (e.key === 'Escape') close(); if (e.key === 'Enter') confirm(); }
    root.querySelector('[data-x="cancel"]').onclick = close;
    root.querySelector('[data-x="ok"]').onclick = confirm;
    root.onclick = (e) => { if (e.target === root) close(); };
    document.addEventListener('keydown', onKey);
  }

  // ---------------- 启动 ----------------
  loadUser();
  loadTags().then(() => loadList());

  // 标签视图“新建标签”按钮
  { const el = $('newTagBtn'); if (el) el.onclick = () => {
    openModal({
      title: '新建标签',
      body: '<div class="tag-new"><input class="input" id="tagCName" placeholder="标签名称" maxlength="64">' +
            '<input type="color" id="tagCColor" value="#6366f1" class="tag-color-input"></div>',
      confirmText: '创建',
      onConfirm: async () => {
        const name = $('tagCName').value.trim(); const color = $('tagCColor').value;
        if (!name) { toast('请输入标签名', 'err'); return false; }
        try { await Api.tagCreate(name, color); toast('已创建', 'ok'); loadTagsView(); return true; }
        catch (e) { toast(e.message || '创建失败', 'err'); return false; }
      },
    });
  }; }
})();
