// App shell: main page of feature cards, a separate Settings page, WebSocket
// client and fetch helper.
//
// Feature UIs live in /features/<name>.js (main/web/features/). The build lists
// them in /features.json and the shell loads each with import(). A feature
// module exports a default function that receives App and registers its cards:
//
//   export default function (App) {
//     App.card({
//       id: 'demo', title: 'Demo', order: 50,   // lower order = earlier
//       mount(el, App) {                          // el = the card body, called once
//         el.innerHTML = '<span data-k="v">–</span>';
//         App.on('demo', d => { el.querySelector('[data-k=v]').textContent = d.value; });
//         App.on('$open', () => App.send('demo')); // ask for state on (re)connect
//       },
//     });
//   }
//
// API:
//   App.on(topic, cb) -> unsubscribe   messages {"t":topic,"d":data} from web_ws_broadcast();
//                                      local topics '$open' / '$close' track the socket
//   App.send(topic, data) -> bool      to web_ws_on() handlers; false if not connected
//   App.api(path, {method, body})      fetch; JSON in and out; throws Error(text) with .status
//                                      on HTTP errors. On 401 it asks for the admin password
//                                      and retries (401 thrown = the user cancelled)
//   App.signIn(password|null), App.signedIn   the admin password the page sends
//   App.card({id, title, order, wide, mount})
//   App.settings({id, title, order, mount(el, ctx), save, reset})
//                                      a section on the Settings page. Editable sections
//                                      call ctx.dirty(true) when edited; Save then awaits
//                                      save() of every dirty section and closes the page
//                                      (a throw keeps it open and shows the message);
//                                      Cancel/close call reset(). Optional show()/hide()
//                                      run when the Settings page opens/closes.
//   App.connected, App.info (last /api/info), App.fmt (kb, mb, duration)

import coreSettings from '/settings.js';

const $ = id => document.getElementById(id);
const handlers = new Map();
const cards = [];
const sections = [];
let ws = null;
let retryMs = 1000;
let cardsReady = false;
let everConnected = false;

// ---- Admin sign-in ----
// Admin requests from this page get a bare 401 (auth.c sends no WWW-Authenticate to
// fetch() calls, which browsers handle inconsistently): App.api() asks here, then
// sends the password itself. Kept for the browser session.

const AUTH_KEY = 'esp32-admin-auth';
let auth = null;
try { auth = sessionStorage.getItem(AUTH_KEY); } catch (e) { }
let asking = null;  // one dialog shared by concurrent 401s

function setAuth(password) {
  auth = null;
  if (password != null) {
    const bytes = new TextEncoder().encode('admin:' + password);
    auth = 'Basic ' + btoa(String.fromCharCode(...bytes));
  }
  try {
    if (auth) sessionStorage.setItem(AUTH_KEY, auth); else sessionStorage.removeItem(AUTH_KEY);
  } catch (e) { }
}

// Resolves true once a password was entered, false if cancelled.
function askPassword(wrong) {
  if (!asking) {
    asking = new Promise(resolve => {
      const dlg = $('signin'), pw = $('signin-pw');
      pw.value = '';
      $('signin-msg').textContent = wrong ? 'Wrong password, try again.' : '';
      dlg.returnValue = '';
      $('signin-cancel').onclick = () => dlg.close('cancel');
      dlg.onclose = () => {
        asking = null;
        const ok = dlg.returnValue === 'ok' && pw.value !== '';
        if (ok) setAuth(pw.value);
        pw.value = '';
        resolve(ok);
      };
      dlg.showModal();
    });
  }
  return asking;
}

function emit(topic, data) {
  for (const cb of handlers.get(topic) || []) {
    try { cb(data); } catch (e) { console.error(`handler for '${topic}' failed`, e); }
  }
}

export const App = {
  connected: false,
  info: null,

  on(topic, cb) {
    if (!handlers.has(topic)) handlers.set(topic, new Set());
    handlers.get(topic).add(cb);
    return () => handlers.get(topic).delete(cb);
  },

  send(topic, data = null) {
    if (!App.connected) return false;
    ws.send(JSON.stringify({ t: topic, d: data }));
    return true;
  },

  async api(path, { method = 'GET', body } = {}) {
    for (;;) {
      // The server rejects state-changing requests without this header (CSRF guard).
      const headers = { 'X-Requested-With': 'fetch' };
      const sent = auth;
      if (sent) headers.Authorization = sent;
      // A board that stops answering must not leave callers (e.g. a disabled Save) waiting forever.
      const opts = { method, headers, cache: 'no-store', signal: AbortSignal.timeout(30000) };
      if (body !== undefined) {
        if (typeof body === 'string') {
          opts.body = body;
        } else {
          headers['Content-Type'] = 'application/json';
          opts.body = JSON.stringify(body);
        }
      }
      let r;
      try {
        r = await fetch(path, opts);
      } catch (e) {
        throw new Error(e.name === 'TimeoutError' ? 'The board did not respond' : 'Board unreachable');
      }
      const isJson = (r.headers.get('Content-Type') || '').includes('json');
      const data = isJson ? await r.json() : await r.text();
      if (r.status === 401) {
        // Sent before a sign-in or password change finished: retry with the new one.
        if (auth && sent !== auth) continue;
        if (sent) setAuth(null);  // stale or wrong: forget it
        if (await askPassword(!!sent)) continue;
      }
      if (!r.ok) {
        const text = r.status === 401 ? 'Admin password required'
          : typeof data === 'string' && data ? data : `HTTP ${r.status}`;
        const err = new Error(text);
        err.status = r.status;
        throw err;
      }
      return data;
    }
  },

  // The admin password the page sends; call after changing it (null after removing it).
  signIn(password) { setAuth(password); },
  get signedIn() { return !!auth; },

  card(def) {
    cards.push({ order: 100, wide: false, ...def });
    if (cardsReady) renderCards();
  },

  settings(def) {
    sections.push({ order: 100, ...def });
    renderSections();
  },

  fmt: {
    kb: b => Math.round(b / 1024) + ' KB',
    mb: b => (b / 1048576).toFixed(b >= 10485760 ? 0 : 1) + ' MB',
    duration(s) {
      const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
      return d ? `${d}d ${h}h ${m}m` : h ? `${h}h ${m}m` : `${m}m ${s % 60}s`;
    },
  },
};

// ---- Header ----

function showHeader() {
  const w = App.info && App.info.wifi;
  if (w) {
    $('host').textContent = w.hostname;
    document.title = `${w.hostname} · ${App.info.firmware.version}`;
  }
  $('netdot').className = 'dot ' + (App.connected ? 'ok' : everConnected ? 'err' : '');
  $('netline').textContent = App.connected ? (w && w.connected ? `${w.ssid} · ${w.ip}` : 'connected')
    : everConnected ? 'board unreachable, reconnecting…' : 'connecting…';
}

async function refreshInfo() {
  try { App.info = await App.api('/api/info'); } catch (e) { }
  showHeader();
}

// ---- WebSocket ----

function connect() {
  const sock = ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  sock.onopen = () => {
    App.connected = everConnected = true;
    retryMs = 1000;
    refreshInfo();
    emit('$open');
  };
  sock.onmessage = e => {
    let m;
    try { m = JSON.parse(e.data); } catch (err) { return; }
    if (m && typeof m.t === 'string') emit(m.t, m.d);
  };
  sock.onclose = () => {
    const was = App.connected;
    App.connected = false;
    showHeader();
    if (was) emit('$close');
    setTimeout(connect, retryMs);
    retryMs = Math.min(retryMs * 2, 10000);
  };
  sock.onerror = () => sock.close();
}

// Traffic keeps the server from recycling an idle socket; unknown topics are ignored.
setInterval(() => App.send('ping'), 25000);

// ---- Main page ----

const mainPage = {
  mount(el) {
    el.innerHTML = '<div class="grid"></div>';
    this.grid = el.firstElementChild;
    if (cardsReady) renderCards();
  },
};

function renderCards() {
  const grid = mainPage.grid;
  if (!grid) return;
  cards.sort((a, b) => a.order - b.order || String(a.title).localeCompare(b.title));
  for (const c of cards) {
    if (!c.el) {
      c.el = document.createElement('section');
      c.el.className = 'card' + (c.wide ? ' wide' : '');
      c.el.innerHTML = '<h2></h2><div></div>';
      c.el.firstElementChild.textContent = c.title;
      try {
        c.mount(c.el.lastElementChild, App);
      } catch (e) {
        console.error(`card '${c.id}' failed`, e);
        c.el.lastElementChild.className = 'msg err';
        c.el.lastElementChild.textContent = `failed: ${e.message}`;
      }
    }
    grid.appendChild(c.el);  // re-appending keeps the sorted order
  }
  if (!cards.length) grid.innerHTML = '<p class="empty">No features yet: add one in main/web/features/.</p>';
  else grid.querySelector('.empty')?.remove();
}

async function loadFeatures() {
  let urls = [];
  try { urls = await App.api('/features.json'); } catch (e) { console.error('features.json', e); }
  const results = await Promise.allSettled(urls.map(u => import(u)));
  results.forEach((r, i) => {
    try {
      if (r.status === 'rejected') throw r.reason;
      r.value.default(App);
    } catch (e) {
      console.error(urls[i], e);
      App.card({ id: urls[i], title: urls[i], order: 1000, mount() { throw e; } });
    }
  });
  cardsReady = true;
  renderCards();
}

// ---- Settings page ----

const settingsPage = {
  mount(el) {
    el.innerHTML = '<div class="settings"></div>';
    this.list = el.firstElementChild;
    renderSections();
  },
  show() { sections.forEach(s => s.show?.()); },
  hide() { sections.forEach(s => s.hide?.()); },
};

function renderSections() {
  const list = settingsPage.list;
  if (!list) return;
  sections.sort((a, b) => a.order - b.order || String(a.title).localeCompare(b.title));
  for (const s of sections) {
    if (!s.el) {
      s.el = document.createElement('section');
      s.el.className = 'card';
      s.el.innerHTML = '<h2></h2><div></div>';
      s.el.firstElementChild.textContent = s.title;
      s.isDirty = false;
      const ctx = { App, dirty: (d = true) => { s.isDirty = d; updateSaveBar(); } };
      try {
        s.mount(s.el.lastElementChild, ctx);
      } catch (e) {
        console.error(`settings '${s.id}' failed`, e);
        s.el.lastElementChild.className = 'msg err';
        s.el.lastElementChild.textContent = `failed: ${e.message}`;
      }
    }
    list.appendChild(s.el);
  }
}

const isDirty = () => sections.some(s => s.isDirty);

function updateSaveBar(msg) {
  $('savebar').hidden = !isDirty() || currentPath() !== '/settings';
  $('savemsg').className = msg ? 'msg err' : 'msg';
  $('savemsg').textContent = msg || 'Unsaved changes';
}

function resetSections() {
  for (const s of sections.filter(s => s.isDirty)) {
    try { s.reset?.(); } catch (e) { console.error(e); }
    s.isDirty = false;
  }
  updateSaveBar();
}

// Leave Settings for the main page. Returns false if the user keeps editing.
function closeSettings() {
  if (isDirty() && !confirm('Discard unsaved changes?')) return false;
  resetSections();
  location.hash = '#/';
  return true;
}

async function saveSettings() {
  $('save').disabled = $('cancel').disabled = true;
  try {
    for (const s of sections.filter(s => s.isDirty)) {
      await s.save?.();
      s.isDirty = false;
    }
    updateSaveBar();
    location.hash = '#/';
  } catch (e) {
    updateSaveBar(e.message || 'Save failed');
  } finally {
    $('save').disabled = $('cancel').disabled = false;
  }
}

document.querySelectorAll('[data-close]').forEach(b => { b.onclick = closeSettings; });
$('cancel').onclick = () => { resetSections(); location.hash = '#/'; };
$('save').onclick = saveSettings;
document.addEventListener('keydown', e => {
  // Esc in the sign-in dialog only closes the dialog.
  if (e.key === 'Escape' && currentPath() === '/settings' && !$('signin').open) closeSettings();
});
window.addEventListener('beforeunload', e => { if (isDirty()) e.preventDefault(); });

// ---- Router ----

const pages = { '/': mainPage, '/settings': settingsPage };
const els = {};
let shown = null;

function currentPath() {
  return pages[location.hash.slice(1)] ? location.hash.slice(1) : '/';
}

function route() {
  const path = currentPath();
  // Browser back from Settings with unsaved edits: confirm before leaving.
  if (shown === '/settings' && path !== '/settings' && isDirty()) {
    if (!confirm('Discard unsaved changes?')) { location.hash = '#/settings'; return; }
    resetSections();
  }
  for (const [p, page] of Object.entries(pages)) {
    const active = p === path;
    if (active && !els[p]) {
      els[p] = document.createElement('div');
      $('view').appendChild(els[p]);
      page.mount(els[p]);
    }
    if (!els[p]) continue;
    els[p].hidden = !active;
    if (p !== shown && active) page.show?.();
    if (p === shown && !active) page.hide?.();
  }
  $('hdr-main').hidden = path === '/settings';
  $('hdr-settings').hidden = path !== '/settings';
  shown = path;
  updateSaveBar();
  window.scrollTo(0, 0);
}

window.addEventListener('hashchange', route);
coreSettings(App);
route();
showHeader();
refreshInfo();
connect();
loadFeatures();
