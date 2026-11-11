// Settings sections for board administration: Wi-Fi, hostname, admin password
// and a live log viewer. No main-page card. API in core/web_settings.c.

const css = `
.nets { display: grid; gap: 4px; margin-top: 8px; max-height: 240px; overflow-y: auto; }
.nets button { display: flex; justify-content: space-between; gap: 12px; text-align: left; padding: 8px 10px; }
.nets button span:last-child { color: var(--muted); white-space: nowrap; font-variant-numeric: tabular-nums; }
.nets button.sel { border-color: var(--accent); }
.suffix { color: var(--muted); }
.logview {
  height: 360px; overflow: auto; margin: 12px 0 0; padding: 8px 10px; background: var(--bg);
  border: 1px solid var(--line); border-radius: 8px; white-space: pre-wrap; overflow-wrap: anywhere;
  font: 12px/1.45 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
.logview .l-E { color: var(--err); } .logview .l-W { color: var(--warn); }
.logview .mark { color: var(--muted); font-style: italic; }
.logview.f-E .l-W, .logview.f-E .l-I, .logview.f-E .l-D, .logview.f-E .l-V,
.logview.f-W .l-I, .logview.f-W .l-D, .logview.f-W .l-V,
.logview.f-I .l-D, .logview.f-I .l-V, .logview.f-D .l-V { display: none; }
.logbar { margin-top: 0; }
.logbar .grow { flex: 1; }
`;

const sleep = ms => new Promise(r => setTimeout(r, ms));

function bind(el, html) {
  el.innerHTML = html;
  const k = {};
  el.querySelectorAll('[data-k]').forEach(e => { k[e.dataset.k] = e; });
  return k;
}

function say(msgEl, text, cls = '') {
  msgEl.className = 'msg' + (cls ? ' ' + cls : '');
  msgEl.textContent = text || '';
}

const validName = n => /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,29}[A-Za-z0-9])?$/.test(n);

// ---- Wi-Fi ----

function wifiSection(App) {
  let k, ctx;
  const host = () => (App.info && App.info.wifi && App.info.wifi.hostname) || location.hostname;

  async function loadCurrent() {
    try { k.cur.textContent = (await App.api('/api/settings/wifi')).ssid || 'none'; } catch (e) { }
  }

  function edited() { ctx.dirty(k.ssid.value !== '' || k.pass.value !== ''); }

  async function scan() {
    k.scan.disabled = true;
    say(k.msg, 'Scanning… (a few seconds)');
    try {
      const nets = await App.api('/api/wifi/scan');
      k.nets.innerHTML = '';
      for (const n of nets) {
        const b = document.createElement('button');
        b.type = 'button';
        b.innerHTML = '<span></span><span></span>';
        b.firstChild.textContent = n.ssid;
        b.lastChild.textContent = `${n.rssi} dBm · ch ${n.channel} · ${n.auth}`;
        b.onclick = () => {
          k.nets.querySelectorAll('.sel').forEach(x => x.classList.remove('sel'));
          b.classList.add('sel');
          k.ssid.value = n.ssid;
          k.pass.value = '';
          edited();
          if (n.auth !== 'open') k.pass.focus();
        };
        k.nets.appendChild(b);
      }
      k.nets.hidden = !nets.length;
      say(k.msg, nets.length ? '' : 'No networks found.');
    } catch (e) {
      say(k.msg, e.message, 'err');
    } finally {
      k.scan.disabled = false;
    }
  }

  return {
    id: 'wifi', title: 'Wi-Fi', category: 'network', order: 20,
    mount(el, c) {
      ctx = c;
      k = bind(el, `
        <dl><dt>Saved network</dt><dd data-k="cur">–</dd></dl>
        <label>New network
          <div class="inrow"><input data-k="ssid" maxlength="32" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="SSID">
          <button type="button" data-k="scan">Scan</button></div>
        </label>
        <div class="nets" data-k="nets" hidden></div>
        <label>Password<input type="password" data-k="pass" maxlength="63" autocomplete="new-password" placeholder="empty for an open network"></label>
        <p class="hint">On Save the board tries the new network for up to 30 s and keeps it only if it gets an
          address; otherwise it goes back to the saved one. This page may lose contact meanwhile. On a different
          network the board gets a new IP: reopen it from that network at <b data-k="url"></b>.</p>
        <div class="msg" data-k="msg"></div>`);
      k.url.textContent = `http://${host()}.local/`;
      k.ssid.oninput = k.pass.oninput = edited;
      k.scan.onclick = scan;
      loadCurrent();
    },
    show() { loadCurrent(); k.url.textContent = `http://${host()}.local/`; },
    async save() {
      const ssid = k.ssid.value, password = k.pass.value;
      if (!ssid) throw new Error('Wi-Fi: enter or pick a network name');
      if (password && password.length < 8) throw new Error('Wi-Fi: the password needs at least 8 characters');
      const start = await App.api('/api/settings/wifi', { method: 'POST', body: { ssid, password } });
      say(k.msg, `Switching to '${ssid}'… this can take up to ${start.timeout_s} s.`);
      const deadline = Date.now() + (start.timeout_s + 45) * 1000;
      while (Date.now() < deadline) {
        await sleep(2000);
        let st;
        try { st = await App.api('/api/settings/wifi'); }
        catch (e) { say(k.msg, `Switching to '${ssid}'… waiting for the board to answer.`); continue; }
        if (st.attempt !== start.attempt || st.state === 'trying') continue;
        if (st.state === 'ok') {
          say(k.msg, st.message, 'ok');
          k.cur.textContent = ssid;
          k.ssid.value = k.pass.value = '';
          k.nets.hidden = true;
          return;
        }
        say(k.msg, st.message, 'err');
        throw new Error(st.message);
      }
      const msg = `Lost contact with the board. If '${ssid}' is a different network, join it and open http://${host()}.local/`;
      say(k.msg, msg, 'err');
      throw new Error(msg);
    },
    reset() {
      k.ssid.value = k.pass.value = '';
      k.nets.hidden = true;
      say(k.msg, '');
    },
  };
}

// ---- Hostname ----

function nameSection(App) {
  let k, ctx, current = '', def = '', editing = false;

  async function load() {
    try {
      const r = await App.api('/api/settings/name');
      current = r.name; def = r.default;
      if (!editing) k.name.value = current;
      k.def.textContent = def;
    } catch (e) { }
  }

  // Resolve when http://<name>.local/ answers (no-cors: we only need reachability).
  async function waitFor(name) {
    await sleep(4000);
    const deadline = Date.now() + 90000;
    while (Date.now() < deadline) {
      try {
        await fetch(`http://${name}.local/api/info`, { mode: 'no-cors', cache: 'no-store' });
        return true;
      } catch (e) { }
      await sleep(2000);
    }
    return false;
  }

  return {
    id: 'hostname', title: 'Hostname', category: 'network', order: 30,
    mount(el, c) {
      ctx = c;
      k = bind(el, `
        <label>Name on the network
          <div class="inrow"><input data-k="name" maxlength="31" autocomplete="off" autocapitalize="off" spellcheck="false">
          <span class="suffix">.local</span></div>
        </label>
        <p class="hint">Letters, digits and '-' (not first or last), up to 31 characters. Empty resets it to
          <b data-k="def">–</b>. Saving reboots the board, then this page moves to the new address.</p>
        <div class="msg" data-k="msg"></div>`);
      k.name.oninput = () => {
        const v = k.name.value.trim();
        editing = true;
        say(k.msg, v && !validName(v) ? "Use letters, digits and '-', not at the start or end." : '', 'err');
        ctx.dirty(v.toLowerCase() !== current.toLowerCase());
      };
      load();
    },
    show() { load(); },
    async save() {
      const name = k.name.value.trim();
      if (name && !validName(name)) throw new Error("Hostname: use letters, digits and '-'");
      const r = await App.api('/api/settings/name', { method: 'POST', body: { name } });
      editing = false;
      if (!r.reboot) return;
      say(k.msg, `Saved. Rebooting as ${r.name}.local…`);
      if (await waitFor(r.name)) {
        location.href = `http://${r.name}.local/`;
        await new Promise(() => { });  // the page is navigating away
      }
      const msg = `The board was renamed but ${r.name}.local is not answering yet. Try http://${r.name}.local/ or its IP.`;
      say(k.msg, msg, 'err');
      current = r.name;
      throw new Error(msg);
    },
    reset() {
      editing = false;
      k.name.value = current;
      say(k.msg, '');
    },
  };
}

// ---- Admin password ----

function passwordSection(App) {
  let k, ctx, enabled = false, removing = false;

  function showState() {
    if (!k) return;
    k.st.textContent = enabled ? 'Set (user: admin)' : 'Not set: anyone on the network can change settings';
    k.off.hidden = !enabled;
    k.off.textContent = removing ? 'Keep password' : 'Remove password';
    k.pw.disabled = k.pw2.disabled = removing;
  }

  async function load() {
    try { enabled = (await App.api('/api/settings/password')).enabled; } catch (e) { }
    showState();
  }

  function edited() {
    removing = false;
    showState();
    ctx.dirty(k.pw.value !== '' || k.pw2.value !== '');
  }

  return {
    id: 'password', title: 'Admin password', category: 'security', order: 10,
    get enabled() { return enabled; },
    load,
    mount(el, c) {
      ctx = c;
      k = bind(el, `
        <dl><dt>Status</dt><dd data-k="st">–</dd></dl>
        <label>New password<input type="password" data-k="pw" maxlength="64" autocomplete="new-password"></label>
        <label>Confirm<input type="password" data-k="pw2" maxlength="64" autocomplete="new-password"></label>
        <div class="row"><button type="button" data-k="off" hidden>Remove password</button></div>
        <p class="hint">With a password, updates, reboots, settings changes and the log viewer ask for it
          (user <b>admin</b> if you open an admin URL directly). The main page and status stay open.
          Forgotten? On the USB console run <code>passwd -</code>.</p>
        <div class="msg" data-k="msg"></div>`);
      k.pw.oninput = k.pw2.oninput = edited;
      k.off.onclick = () => {
        removing = !removing;
        k.pw.value = k.pw2.value = '';
        showState();
        say(k.msg, removing ? 'The password will be removed on Save.' : '', 'warn');
        ctx.dirty(removing);
      };
      load();
    },
    show() { load(); },
    async save() {
      let password = '';
      if (removing) {
        if (!confirm('Remove the admin password? Anyone on the network will be able to change settings.')) {
          throw new Error('Admin password: not removed');
        }
      } else {
        password = k.pw.value;
        if (password.length < 4) throw new Error('Admin password: use at least 4 characters');
        if (password !== k.pw2.value) throw new Error('Admin password: the two entries differ');
      }
      const r = await App.api('/api/settings/password', { method: 'POST', body: { password } });
      App.signIn(r.token || null);  // the old token was signed out with the old password
      enabled = r.enabled;
      removing = false;
      k.pw.value = k.pw2.value = '';
      showState();
      say(k.msg, enabled ? 'Saved.' : 'Removed.', 'ok');
    },
    reset() {
      removing = false;
      k.pw.value = k.pw2.value = '';
      showState();
      say(k.msg, '');
    },
  };
}

// ---- Logs ----

function logsSection(App, pw) {
  const MAX_LINES = 1500;
  let k, seq = 0, boot = null, timer = null, running = false, paused = false, visible = false, authorised = false;

  function addLine(text, cls) {
    const atBottom = k.view.scrollTop + k.view.clientHeight >= k.view.scrollHeight - 8;
    const d = document.createElement('div');
    d.className = cls;
    d.textContent = text;
    k.view.appendChild(d);
    while (k.view.childElementCount > MAX_LINES) k.view.firstElementChild.remove();
    if (atBottom) k.view.scrollTop = k.view.scrollHeight;
  }

  const levelOf = line => (/^[EWIDV] \(/.test(line) ? 'l-' + line[0] : 'l-I');

  async function poll() {
    timer = null;
    if (!running) return;
    try {
      const r = await App.api(`/api/logs?after=${seq}`);
      authorised = true;
      if (boot !== null && r.boot !== boot && seq) {
        // Rebooted: sequence numbers started over, so fetch the new boot from the start.
        addLine('— board restarted —', 'mark');
        boot = r.boot;
        seq = 0;
        timer = setTimeout(poll, 0);
        return;
      }
      boot = r.boot;
      if (r.dropped && seq) addLine('— some lines were dropped —', 'mark');
      r.lines.forEach(l => addLine(l, levelOf(l)));
      seq = r.seq;
      say(k.msg, '');
    } catch (e) {
      if (e.status === 401 || e.status === 429) {
        stop();
        authorised = false;
        say(k.msg, e.status === 401 ? 'Admin password required.' : e.message, 'err');
        k.start.hidden = false;
        return;
      }
      say(k.msg, 'Board not answering, retrying…');
    }
    if (running) timer = setTimeout(poll, 1000);
  }

  function start() {
    if (running || paused) return;
    running = true;
    k.start.hidden = true;
    poll();
  }

  function stop() {
    running = false;
    clearTimeout(timer);
    timer = null;
  }

  async function open() {
    visible = true;
    await pw.load();
    if (!visible) return;
    // With a password set, only start on a click, so opening Settings doesn't
    // pop up a sign-in prompt by itself.
    if (pw.enabled && !authorised && !App.signedIn) {
      k.start.hidden = false;
      say(k.msg, 'The log viewer needs the admin password.');
    } else {
      start();
    }
  }

  return {
    id: 'logs', title: 'Logs', category: 'system', order: 40,
    mount(el) {
      k = bind(el, `
        <div class="row logbar">
          <select data-k="level" aria-label="Level">
            <option value="">All levels</option><option value="E">Errors</option>
            <option value="W">Warnings and up</option><option value="I">Info and up</option>
            <option value="D">Debug and up</option>
          </select>
          <span class="grow"></span>
          <button type="button" data-k="start" class="primary" hidden>Show logs</button>
          <button type="button" data-k="pause">Pause</button>
          <button type="button" data-k="clear">Clear</button>
        </div>
        <div class="logview" data-k="view" role="log"></div>
        <div class="msg" data-k="msg"></div>`);
      k.level.onchange = () => {
        k.view.className = 'logview' + (k.level.value ? ' f-' + k.level.value : '');
        k.view.scrollTop = k.view.scrollHeight;
      };
      k.pause.onclick = () => {
        paused = !paused;
        k.pause.textContent = paused ? 'Resume' : 'Pause';
        if (paused) stop(); else start();
      };
      k.clear.onclick = () => { k.view.innerHTML = ''; };
      k.start.onclick = () => { paused = false; k.pause.textContent = 'Pause'; start(); };
    },
    show() { open(); },
    hide() { visible = false; stop(); },
  };
}

export default function (App) {
  if (!document.getElementById('admin-css')) {
    const style = document.createElement('style');
    style.id = 'admin-css';
    style.textContent = css;
    document.head.appendChild(style);
  }
  const pw = passwordSection(App);
  App.settings(wifiSection(App));
  App.settings(nameSection(App));
  App.settings(pw);
  App.settings(logsSection(App, pw));
}
