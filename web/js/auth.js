/* =============================================================================
   auth.js — 登录 / 注册 页逻辑
   ============================================================================= */
(function () {
  'use strict';
  const { Store, Api, toast, Theme } = window.CV;

  // 已登录则直接进应用
  if (Store.token) { location.href = '/app'; return; }

  let mode = 'login'; // login | register

  const $ = (id) => document.getElementById(id);
  const tabs = document.querySelectorAll('.tab');
  const confirmField = $('confirmField');
  const submitText = $('submitText');
  const formTitle = $('formTitle');
  const formSub = $('formSub');
  const pwHint = $('pwHint');

  function setMode(m) {
    mode = m;
    tabs.forEach(t => t.classList.toggle('active', t.dataset.tab === m));
    const reg = m === 'register';
    confirmField.style.display = reg ? '' : 'none';
    pwHint.style.display = reg ? '' : 'none';
    submitText.textContent = reg ? '创建账户' : '登录';
    formTitle.textContent = reg ? '创建你的账户' : '欢迎回来';
    formSub.textContent = reg ? '开启你的云端存储空间' : '登录以访问你的云端文件';
    $('password').setAttribute('autocomplete', reg ? 'new-password' : 'current-password');
  }
  tabs.forEach(t => t.addEventListener('click', () => setMode(t.dataset.tab)));

  // 密码显隐
  $('pwToggle').addEventListener('click', () => {
    const p = $('password');
    p.type = p.type === 'password' ? 'text' : 'password';
  });

  // 主题切换
  const themeLabel = $('themeLabel');
  function syncThemeLabel() { themeLabel.textContent = Theme.get() === 'dark' ? '浅色模式' : '深色模式'; }
  syncThemeLabel();
  $('themeToggle').addEventListener('click', () => { Theme.toggle(); syncThemeLabel(); });

  // 提交
  $('authForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const username = $('username').value.trim();
    const password = $('password').value;
    const btn = $('submitBtn');

    if (!username || !password) { toast('请输入用户名和密码', 'warn'); return; }
    if (mode === 'register') {
      if (!/^[A-Za-z0-9_-]{3,32}$/.test(username)) { toast('用户名需为 3-32 位字母/数字/_/-', 'warn'); return; }
      if (password.length < 6) { toast('密码至少 6 位', 'warn'); return; }
      if (password !== $('confirm').value) { toast('两次输入的密码不一致', 'warn'); return; }
    }

    btn.disabled = true;
    const oldText = submitText.textContent;
    submitText.textContent = mode === 'register' ? '注册中…' : '登录中…';
    try {
      if (mode === 'register') {
        await Api.register(username, password);
        toast('注册成功，正在自动登录…', 'ok');
      }
      let data;
      try {
        data = await Api.login(username, password);
      } catch (err) {
        if (err && err.code === 4012) {   // 需要两步验证码
          const code = window.prompt('该账户已开启两步验证，请输入验证器中的 6 位动态码：');
          if (!code) throw { message: '已取消登录' };
          data = await Api.login(username, password, code.trim());
        } else { throw err; }
      }
      Store.token = data.token;
      Store.user = { username: data.username, createdAt: data.createdAt, role: data.role || 0 };
      toast('登录成功', 'ok');
      setTimeout(() => location.href = '/app', 500);
    } catch (err) {
      toast(err.message || '操作失败', 'err');
      btn.disabled = false;
      submitText.textContent = oldText;
    }
  });

  setMode('login');
})();
