/* =============================================================================
   api.js — 共享的 API 封装、Toast、工具函数 (原生 JS, 无依赖)
   ============================================================================= */
(function (global) {
  'use strict';

  const TOKEN_KEY = 'cv_token';
  const USER_KEY  = 'cv_user';

  const Store = {
    get token() { return localStorage.getItem(TOKEN_KEY) || ''; },
    set token(v) { v ? localStorage.setItem(TOKEN_KEY, v) : localStorage.removeItem(TOKEN_KEY); },
    get user() { try { return JSON.parse(localStorage.getItem(USER_KEY) || 'null'); } catch { return null; } },
    set user(v) { v ? localStorage.setItem(USER_KEY, JSON.stringify(v)) : localStorage.removeItem(USER_KEY); },
    clear() { this.token = ''; this.user = null; },
  };

  // ---- fetch 封装: 自动带 Bearer, 统一解析 {code,message,data} ----
  async function request(method, url, body, opts) {
    opts = opts || {};
    const headers = {};
    if (Store.token) headers['Authorization'] = 'Bearer ' + Store.token;
    let payload = body;
    if (body && !(body instanceof FormData)) {
      headers['Content-Type'] = 'application/json';
      payload = JSON.stringify(body);
    }
    let res;
    try {
      res = await fetch(url, { method, headers, body: payload });
    } catch (e) {
      throw { code: -1, message: '网络错误，请检查服务是否运行' };
    }
    if (res.status === 401 && !opts.noAuthRedirect) {
      Store.clear();
      if (!location.pathname.endsWith('index.html') && location.pathname !== '/') {
        // 跳转前必须说明原因: 静默 location.href = '/' 会让用户以为页面坏了,
        // 且正在进行的批量上传被丢弃时也毫无线索。
        toast('登录已过期，请重新登录', 'err');
        setTimeout(() => { location.href = '/'; }, 1200);
      }
    }
    let data = null;
    try { data = await res.json(); } catch { /* 非 JSON 响应 */ }
    if (!res.ok || (data && data.code !== 0)) {
      throw data || { code: res.status, message: '请求失败 (' + res.status + ')' };
    }
    return data ? data.data : null;
  }

  const Api = {
    get:  (u, o) => request('GET', u, null, o),
    post: (u, b, o) => request('POST', u, b, o),
    register: (username, password) => request('POST', '/api/auth/register', { username, password }, { noAuthRedirect: true }),
    login:    (username, password, code) => request('POST', '/api/auth/login', { username, password, code: code || '' }, { noAuthRedirect: true }),
    userInfo: () => request('GET', '/api/user/info'),
    // 账户
    profileUpdate:  (nickname, email) => request('POST', '/api/user/profile', { nickname, email }),
    passwordChange: (oldPassword, newPassword) => request('POST', '/api/user/password', { oldPassword, newPassword }),
    avatarUrl:      (uid, tag) => '/api/user/avatar?uid=' + encodeURIComponent(uid) + (tag ? '&v=' + encodeURIComponent(tag) : ''),
    avatarUpload:   (file) => { const fd = new FormData(); fd.append('avatar', file, file.name); return request('POST', '/api/user/avatar', fd); },
    twofaSetup:   () => request('POST', '/api/2fa/setup', {}),
    twofaEnable:  (code) => request('POST', '/api/2fa/enable', { code }),
    twofaDisable: (code) => request('POST', '/api/2fa/disable', { code }),
    // 开发者 / API 令牌
    tokenList:   () => request('GET', '/api/tokens'),
    tokenCreate: (name, expiresDays) => request('POST', '/api/tokens', { name, expiresDays: expiresDays || 0 }),
    tokenRevoke: (id) => request('POST', '/api/tokens/revoke', { id }),
    // 收藏夹
    favToggle: (itemType, itemId) => request('POST', '/api/favorite/toggle', { itemType, itemId }),
    favBatch:  (fileIds, folderIds) => request('POST', '/api/favorite/batch', { fileIds, folderIds }),
    favList:   () => request('GET', '/api/favorites'),
    // 全局搜索
    search:    (q) => request('GET', '/api/search?q=' + encodeURIComponent(q)),
    // 文件版本历史
    versions:        (fileId) => request('GET', '/api/file/versions?fileId=' + encodeURIComponent(fileId)),
    versionRestore:  (fileId, versionId) => request('POST', '/api/file/version/restore', { fileId, versionId }),
    versionUpload:   (fileId, file) => { const fd = new FormData(); fd.append('file', file, file.name); return request('POST', '/api/file/version?fileId=' + encodeURIComponent(fileId), fd); },
    versionDownloadUrl: (versionId) => '/api/file/version/download?versionId=' + encodeURIComponent(versionId) + '&token=' + encodeURIComponent(Store.token),
    // 统计分析
    stats:     () => request('GET', '/api/stats'),
    // 标签
    tagList:     () => request('GET', '/api/tags'),
    tagCreate:   (name, color) => request('POST', '/api/tags', { name, color }),
    tagDelete:   (tagId) => request('POST', '/api/tags/delete', { tagId }),
    fileTagsGet: (fileId) => request('GET', '/api/file/tags?fileId=' + encodeURIComponent(fileId)),
    fileTagsSet: (fileId, tagIds) => request('POST', '/api/file/tags/set', { fileId, tagIds }),
    filesByTag:  (tagId) => request('GET', '/api/files/by-tag?tagId=' + encodeURIComponent(tagId)),
    // 活动日志
    activity:    (action) => request('GET', '/api/activity' + (action ? '?action=' + encodeURIComponent(action) : '')),
    // 管理后台
    adminStats:       () => request('GET', '/api/admin/stats'),
    adminUsers:       (limit, offset) => request('GET', '/api/admin/users?limit=' + (limit || 100) + '&offset=' + (offset || 0)),
    adminSetQuota:    (uid, quota) => request('POST', '/api/admin/user/quota', { uid, quota }),
    adminSetRole:     (uid, role) => request('POST', '/api/admin/user/role', { uid, role }),
    adminSetDisabled: (uid, disabled) => request('POST', '/api/admin/user/disable', { uid, disabled }),
    adminAudit:       (limit, offset) => request('GET', '/api/admin/audit?limit=' + (limit || 100) + '&offset=' + (offset || 0)),
    list: (params) => {
      const q = new URLSearchParams(params).toString();
      return request('GET', '/api/file/list?' + q);
    },
    instant: (filename, hash, size, parentId) => request('POST', '/api/file/instant', { filename, hash, size, parentId: parentId || 0 }),
    // 分片 / 断点续传
    uploadInit:     (payload) => request('POST', '/api/upload/init', payload),
    uploadComplete: (uploadId) => request('POST', '/api/upload/complete', { uploadId }),
    uploadChunkUrl: (uploadId, index) => '/api/upload/chunk?uploadId=' + encodeURIComponent(uploadId) + '&index=' + index,
    // 文件夹上传 (递归确保路径)
    folderEnsure: (path, parentId) => request('POST', '/api/folder/ensure', { path, parentId: parentId || 0 }),
    // URL 离线下载
    offlineCreate: (url, parentId, filename) => request('POST', '/api/offline/create', { url, parentId: parentId || 0, filename: filename || '' }),
    offlineList:   () => request('GET', '/api/offline/list'),
    rename:  (id, newname) => request('POST', '/api/file/rename', { id, newname }),
    downloadUrl: (id) => '/api/file/download?id=' + encodeURIComponent(id) + '&token=' + encodeURIComponent(Store.token),
    contentUrl: (id) => '/api/file/content?id=' + encodeURIComponent(id) + '&token=' + encodeURIComponent(Store.token),
    thumbUrl: (id, size) => '/api/file/thumb?id=' + encodeURIComponent(id) + '&token=' + encodeURIComponent(Store.token) + '&size=' + (size || 256),
    // 文件夹
    folderCreate: (name, parentId) => request('POST', '/api/folder/create', { name, parentId: parentId || 0 }),
    folderRename: (id, newname) => request('POST', '/api/folder/rename', { id, newname }),
    // 移动 / 删除 (批量)
    move:     (fileIds, folderIds, targetId) => request('POST', '/api/fs/move', { fileIds, folderIds, targetId: targetId || 0 }),
    fsDelete: (fileIds, folderIds) => request('POST', '/api/fs/delete', { fileIds, folderIds }),
    // 回收站
    trashList:    () => request('GET', '/api/trash/list'),
    trashRestore: (fileIds, folderIds) => request('POST', '/api/trash/restore', { fileIds, folderIds }),
    trashDelete:  (fileIds, folderIds) => request('POST', '/api/trash/delete', { fileIds, folderIds }),
    trashEmpty:   () => request('POST', '/api/trash/empty', {}),
    // 分享 (创建/管理需鉴权)
    shareCreate: (opts) => request('POST', '/api/share/create', opts),
    shareMine:   () => request('GET', '/api/share/mine'),
    shareCancel: (id) => request('POST', '/api/share/cancel', { id }),
    shareSave:   (token, code, targetId) => request('POST', '/api/share/save', { token, code, targetId: targetId || 0 }),
    // 分享 (公开访问, 不强制鉴权)
    shareInfo:   (token) => request('GET', '/api/share/info?token=' + encodeURIComponent(token), null, { noAuthRedirect: true }),
    shareBrowse: (token, code, folderId) => {
      const q = new URLSearchParams({ token, code: code || '', folderId: folderId || 0 }).toString();
      return request('GET', '/api/share/browse?' + q, null, { noAuthRedirect: true });
    },
    shareDownloadUrl: (token, code, id) =>
      '/api/share/download?token=' + encodeURIComponent(token) +
      '&code=' + encodeURIComponent(code || '') + '&id=' + encodeURIComponent(id),
  };

  // ---- Toast ----
  const ICONS = {
    ok:   '<path d="M20 6 9 17l-5-5"/>',
    err:  '<circle cx="12" cy="12" r="10"/><path d="m15 9-6 6M9 9l6 6"/>',
    warn: '<path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/><path d="M12 9v4M12 17h.01"/>',
    info: '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/>',
  };
  function toast(msg, type) {
    type = type || 'info';
    const root = document.getElementById('toasts');
    if (!root) return;
    const el = document.createElement('div');
    el.className = 'toast ' + type;
    el.innerHTML = '<svg class="icon" viewBox="0 0 24 24">' + (ICONS[type] || ICONS.info) + '</svg><div class="msg"></div>';
    el.querySelector('.msg').textContent = msg;   // textContent 防 XSS
    root.appendChild(el);
    setTimeout(() => {
      el.style.transition = 'opacity .3s, transform .3s';
      el.style.opacity = '0'; el.style.transform = 'translateX(30px)';
      setTimeout(() => el.remove(), 300);
    }, type === 'err' ? 4200 : 2600);
  }

  // ---- 工具函数 ----
  // 与后端 server/util/FormatUtil.h 的 human_size() 严格一致
  // (由 tests/e2e/humansize_contract_validate.mjs 做跨语言逐值校验)。
  // 负数按「符号 + 幅值」输出, 而非抹成 0: 差值型指标 (如去重节省) 可能为负。
  function humanSize(bytes) {
    bytes = Number(bytes) || 0;
    const neg = bytes < 0;
    let v = Math.abs(bytes);
    let body;
    if (v < 1024) {
      body = v + ' B';
    } else {
      const u = ['KB', 'MB', 'GB', 'TB']; let i = -1;
      do { v /= 1024; i++; } while (v >= 1024 && i < u.length - 1);
      body = v.toFixed(v >= 100 ? 0 : 1) + ' ' + u[i];
    }
    return (neg ? '-' : '') + body;
  }
  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c]));
  }

  // ---- 主题 ----
  const Theme = {
    apply(t) { document.documentElement.setAttribute('data-theme', t); localStorage.setItem('cv_theme', t); },
    get()    { return localStorage.getItem('cv_theme') || 'light'; },
    toggle() { const n = this.get() === 'dark' ? 'light' : 'dark'; this.apply(n); return n; },
    init()   { this.apply(this.get()); },
  };
  // 仅在有 DOM 的环境下自动应用主题 (Node 下加载本模块做纯函数测试时跳过)
  if (typeof document !== 'undefined') Theme.init();

  const exports_ = { Store, Api, toast, humanSize, escapeHtml, Theme };
  if (global) global.CV = exports_;
  if (typeof module !== 'undefined' && module.exports) module.exports = exports_;
})(typeof window !== 'undefined' ? window : null);
