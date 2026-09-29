/* =============================================================================
   activity.js — 活动日志动作 → 展示元数据 的映射 (纯查表, 无 DOM 依赖)

   这是「审计动作 → 中文标签/图标/配色」映射 (Visitor 思想):
   后端只落库动作名 (action), 前端在此集中决定其展示形态。
   新增动作只需在此加一行, 前后端解耦, 便于独立测试与跨视图复用。

   · 浏览器: 挂载到 window.CV.Activity
   · Node  : module.exports, 供单元测试 (tests/e2e/activity_validate.mjs)
   ============================================================================= */
(function (root) {
  'use strict';

  const ERR = '<circle cx="12" cy="12" r="10"/><path d="M15 9l-6 6M9 9l6 6"/>';

  // action -> { label, color, icon }
  const ACT_META = {
    login:              { label: '登录', color: '#10b981', icon: '<path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/><path d="M10 17l5-5-5-5M15 12H3"/>' },
    login_fail:         { label: '登录失败', color: '#ef4444', icon: ERR },
    login_blocked:      { label: '登录被限流', color: '#ef4444', icon: '<circle cx="12" cy="12" r="10"/><path d="M4.9 4.9l14.2 14.2"/>' },
    login_2fa_fail:     { label: '两步验证失败', color: '#ef4444', icon: ERR },
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
    admin_set_role:     { label: '管理员·变更角色', color: '#0d9488', icon: '<path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M22 21v-2a4 4 0 0 0-3-3.87M16 3.13a4 4 0 0 1 0 7.75"/>' },
    admin_set_quota:    { label: '管理员·调整配额', color: '#0d9488', icon: '<ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M21 12c0 1.66-4 3-9 3s-9-1.34-9-3"/><path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"/>' },
    admin_set_disabled: { label: '管理员·启用/禁用账户', color: '#0d9488', icon: '<circle cx="12" cy="12" r="10"/><path d="M4.9 4.9l14.2 14.2"/>' },
  };

  const FALLBACK = { label: '', color: '#94a3b8', icon: '<circle cx="12" cy="12" r="10"/>' };

  // 取动作的展示元数据; 未知动作回退为灰色默认 (label 沿用原动作名)。
  function actMeta(action) {
    const m = ACT_META[action];
    if (m) return m;
    return { label: action, color: FALLBACK.color, icon: FALLBACK.icon };
  }

  const Activity = { ACT_META, actMeta };
  if (typeof module !== 'undefined' && module.exports) module.exports = Activity;
  if (root) { root.CV = root.CV || {}; root.CV.Activity = Activity; }
})(typeof window !== 'undefined' ? window : null);
