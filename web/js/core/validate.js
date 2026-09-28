/* =============================================================================
   validate.js — 前端输入校验规则 (与后端 SqlUtil 规则保持一致的单一真源)
   · 浏览器: 挂载到 window.CV.Validate
   · Node  : module.exports, 供单元测试 (tests/e2e/validate_validate.mjs)

   规则须与后端 server/util/SqlUtil.h 对齐, 避免前后端漂移:
     用户名: 3-32 位, 字母/数字/下划线/中划线
     密码  : 6-64 位
   ============================================================================= */
(function (root) {
  'use strict';

  const USERNAME_RE = /^[A-Za-z0-9_-]{3,32}$/;
  const PASSWORD_MIN = 6;
  const PASSWORD_MAX = 64;

  function validUsername(u) {
    return typeof u === 'string' && USERNAME_RE.test(u);
  }
  function validPassword(p) {
    return typeof p === 'string' && p.length >= PASSWORD_MIN && p.length <= PASSWORD_MAX;
  }
  // 返回第一条错误信息 (合法则返回空串), 供表单直接提示。
  function registerError(username, password, confirm) {
    if (!username || !password) return '请输入用户名和密码';
    if (!validUsername(username)) return '用户名需为 3-32 位字母/数字/_/-';
    if (!validPassword(password)) return '密码需为 6-64 位';
    if (confirm !== undefined && password !== confirm) return '两次输入的密码不一致';
    return '';
  }

  const Validate = { USERNAME_RE, PASSWORD_MIN, PASSWORD_MAX, validUsername, validPassword, registerError };
  if (typeof module !== 'undefined' && module.exports) module.exports = Validate;
  if (root) { root.CV = root.CV || {}; root.CV.Validate = Validate; }
})(typeof window !== 'undefined' ? window : null);
