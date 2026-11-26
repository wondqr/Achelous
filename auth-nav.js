/* =============================================================
   auth-nav.js — เมื่อล็อกอินแล้ว เปลี่ยนปุ่ม "Login" บน navbar เป็นอีเมลผู้ใช้
   กดที่อีเมลเพื่อเปิดเมนู แล้วกด "ออกจากระบบ" ได้

   ต้องโหลดหลัง firebase-app-compat.js, firebase-auth-compat.js และ firebase-config.js
   ใช้ได้ทุกหน้าที่มี <a class="navbar__login">
   ============================================================= */
(function () {
  var TEXT = {
    en: { logout: 'Log out', account: 'Account menu', signedIn: 'Signed in as' },
    th: { logout: 'ออกจากระบบ', account: 'เมนูบัญชี', signedIn: 'เข้าสู่ระบบในชื่อ' },
    ko: { logout: '로그아웃', account: '계정 메뉴', signedIn: '로그인 계정' }
  };
  function tr(key) {
    var l = (document.documentElement.lang || 'en').slice(0, 2);
    return (TEXT[l] || TEXT.en)[key];
  }

  var link = document.querySelector('.navbar__login');
  if (!link || !window.ACHELOUS_initFirebase || !window.ACHELOUS_initFirebase() || !firebase.auth) return;
  var auth = firebase.auth();

  var style = document.createElement('style');
  style.textContent =
    '.user-nav { position: relative; }' +
    '.user-nav[hidden] { display: none; }' +
    '.user-nav__btn { display: flex; align-items: center; gap: 8px; max-width: 240px; padding: 5px 12px 5px 5px; border: 1px solid var(--line); border-radius: 999px; background: none; font-family: var(--font-display); font-weight: 600; font-size: 15px; color: var(--deep-2); cursor: pointer; transition: background 0.2s ease, border-color 0.2s ease; }' +
    '.user-nav__btn:hover, .user-nav__btn[aria-expanded="true"] { background: var(--mist); border-color: var(--deep-2); }' +
    '.user-nav__avatar { flex: none; display: grid; place-items: center; width: 26px; height: 26px; border-radius: 50%; background: var(--deep-2); color: #fff; font-size: 13px; }' +
    '.user-nav__email { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }' +
    '.user-nav__menu { position: absolute; right: 0; top: calc(100% + 10px); z-index: 60; min-width: 220px; padding: 6px; background: var(--paper); border: 1px solid var(--line); border-radius: 12px; box-shadow: 0 12px 32px rgba(14, 42, 47, 0.14); }' +
    '.user-nav__menu[hidden] { display: none; }' +
    '.user-nav__who { margin: 0; padding: 8px 10px 10px; border-bottom: 1px solid var(--line); font-size: 13px; color: var(--ink-soft); }' +
    '.user-nav__who b { display: block; margin-top: 2px; font-size: 14px; color: var(--ink); overflow-wrap: anywhere; }' +
    '.user-nav__logout { display: block; width: 100%; margin-top: 6px; padding: 9px 10px; border: 0; border-radius: 8px; background: none; text-align: left; font: inherit; font-weight: 600; color: #a12722; cursor: pointer; }' +
    '.user-nav__logout:hover { background: #fbeceb; }' +
    '@media (max-width: 860px) { .user-nav__email { display: none; } .user-nav__btn { padding: 4px; } }';
  document.head.appendChild(style);

  var wrap = document.createElement('div');
  wrap.className = 'user-nav';
  wrap.hidden = true;
  wrap.innerHTML =
    '<button type="button" class="user-nav__btn" aria-haspopup="true" aria-expanded="false" aria-controls="userNavMenu">' +
      '<span class="user-nav__avatar" aria-hidden="true"></span><span class="user-nav__email"></span>' +
    '</button>' +
    '<div class="user-nav__menu" id="userNavMenu" hidden>' +
      '<p class="user-nav__who"><span class="user-nav__label"></span><b></b></p>' +
      '<button type="button" class="user-nav__logout"></button>' +
    '</div>';
  link.insertAdjacentElement('afterend', wrap);

  var btn = wrap.querySelector('.user-nav__btn');
  var menu = wrap.querySelector('.user-nav__menu');
  var avatar = wrap.querySelector('.user-nav__avatar');
  var emailEl = wrap.querySelector('.user-nav__email');
  var whoEl = wrap.querySelector('.user-nav__who b');
  var labelEl = wrap.querySelector('.user-nav__label');
  var logoutBtn = wrap.querySelector('.user-nav__logout');

  function applyText() {
    labelEl.textContent = tr('signedIn');
    logoutBtn.textContent = tr('logout');
    btn.setAttribute('aria-label', tr('account') + (emailEl.textContent ? ': ' + emailEl.textContent : ''));
  }

  function setOpen(open) {
    menu.hidden = !open;
    btn.setAttribute('aria-expanded', String(open));
  }

  btn.addEventListener('click', function () { setOpen(menu.hidden); });
  document.addEventListener('click', function (e) {
    if (!menu.hidden && !wrap.contains(e.target)) setOpen(false);
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && !menu.hidden) { setOpen(false); btn.focus(); }
  });

  logoutBtn.addEventListener('click', function () {
    logoutBtn.disabled = true;
    auth.signOut()
      .catch(function (error) { console.error('Sign-out failed:', error); })
      .then(function () { logoutBtn.disabled = false; setOpen(false); });
  });

  auth.onAuthStateChanged(function (user) {
    var email = user ? (user.email || '') : '';
    emailEl.textContent = email;
    whoEl.textContent = email;
    avatar.textContent = email ? email.charAt(0).toUpperCase() : '';
    btn.title = email;
    link.hidden = !!user;
    wrap.hidden = !user;
    setOpen(false);
    applyText();
  });

  // ตามภาษาที่ผู้ใช้เลือก (ทุกหน้าตั้งค่า <html lang="...">)
  new MutationObserver(applyText).observe(document.documentElement, { attributes: true, attributeFilter: ['lang'] });
  applyText();
})();
