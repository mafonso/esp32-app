// Settings view: firmware/OTA, board, runtime and network cards.
const html = `
<div class="grid">
  <section class="card wide">
    <h2>Firmware</h2>
    <p class="version" data-k="ver">–</p>
    <div class="sub" data-k="build">–</div>
    <div class="row">
      <button data-k="check" class="primary">Check for updates</button>
      <button data-k="install" hidden>Install</button>
      <button data-k="reboot">Reboot</button>
    </div>
    <div class="bar" data-k="bar"><div></div></div>
    <div class="msg" data-k="otamsg"></div>
  </section>

  <section class="card">
    <h2>Board</h2>
    <dl>
      <dt>Chip</dt><dd data-k="chip">–</dd>
      <dt>CPU</dt><dd data-k="cpu">–</dd>
      <dt>Flash</dt><dd data-k="flash">–</dd>
      <dt>Temperature</dt><dd data-k="temp">–</dd>
      <dt>MAC</dt><dd data-k="mac">–</dd>
    </dl>
  </section>

  <section class="card">
    <h2>Runtime</h2>
    <dl>
      <dt>Uptime</dt><dd data-k="uptime">–</dd>
      <dt>Last reset</dt><dd data-k="reset">–</dd>
      <dt>Heap free</dt><dd data-k="heap">–</dd>
      <dt>Heap low-water</dt><dd data-k="heapmin">–</dd>
      <dt>PSRAM free</dt><dd data-k="psram">–</dd>
    </dl>
  </section>

  <section class="card">
    <h2>Network</h2>
    <dl>
      <dt>SSID</dt><dd data-k="ssid">–</dd>
      <dt>Signal</dt><dd data-k="rssi">–</dd>
      <dt>IP</dt><dd data-k="ip">–</dd>
      <dt>Hostname</dt><dd data-k="hostname">–</dd>
    </dl>
  </section>
</div>`;

function signal(rssi) {
  const q = rssi >= -60 ? 'excellent' : rssi >= -70 ? 'good' : rssi >= -80 ? 'fair' : 'weak';
  return `${rssi} dBm (${q})`;
}

let $, App, timer;

async function refreshInfo() {
  const { kb, mb, duration } = App.fmt;
  let info;
  try { info = await App.api('/api/info'); } catch (e) { return; }
  const { firmware: f, board: b, wifi: w } = info;
  $('ver').textContent = f.version;
  $('build').textContent = `${f.project} · built ${f.build_date} ${f.build_time} · ESP-IDF ${f.idf} · slot ${f.partition}`;
  $('chip').textContent = `${b.chip.toUpperCase()} rev ${Math.floor(b.revision / 100)}.${b.revision % 100}`;
  $('cpu').textContent = `${b.cores} cores @ ${b.cpu_mhz} MHz`;
  $('flash').textContent = mb(b.flash_bytes);
  $('temp').textContent = b.temp_c !== undefined ? b.temp_c.toFixed(1) + ' °C' : 'n/a';
  $('mac').textContent = b.mac;
  $('uptime').textContent = duration(b.uptime_s);
  $('reset').textContent = b.reset_reason;
  $('heap').textContent = kb(b.heap_free);
  $('heapmin').textContent = kb(b.heap_min_free);
  $('psram').textContent = b.psram_bytes ? `${mb(b.psram_free)} of ${mb(b.psram_bytes)}` : 'none';
  $('ssid').textContent = w.ssid || '–';
  $('rssi').textContent = w.connected ? signal(w.rssi) : '–';
  $('ip').textContent = w.ip || '–';
  $('hostname').textContent = w.hostname + '.local';
}

function showOta(o) {
  const { kb } = App.fmt;
  const msg = $('otamsg'), bar = $('bar'), install = $('install');
  const busy = o.state === 'checking' || o.state === 'downloading' || o.state === 'rebooting';
  $('check').disabled = busy;
  install.disabled = busy;
  install.hidden = !o.checked || busy;
  bar.style.display = o.state === 'downloading' || o.state === 'rebooting' ? 'block' : 'none';
  if (o.total_bytes) bar.firstElementChild.style.width = (100 * o.bytes_read / o.total_bytes) + '%';

  if (o.state === 'error') {
    msg.className = 'msg err'; msg.textContent = o.error;
  } else if (o.state === 'checking') {
    msg.className = 'msg'; msg.textContent = 'Checking GitHub releases…';
  } else if (o.state === 'downloading') {
    msg.className = 'msg';
    msg.textContent = o.total_bytes
      ? `Downloading ${o.latest}: ${kb(o.bytes_read)} of ${kb(o.total_bytes)}`
      : `Connecting to download ${o.latest}…`;
  } else if (o.state === 'rebooting') {
    msg.className = 'msg ok'; msg.textContent = `Installed ${o.latest}. Rebooting…`;
  } else if (o.checked) {
    if (o.update_available) {
      msg.className = 'msg warn';
      msg.textContent = `Version ${o.latest} is available (running ${o.running}).` + (o.notes ? ' ' + o.notes : '');
      install.textContent = `Install ${o.latest}`;
      install.className = 'primary';
    } else {
      msg.className = 'msg ok';
      msg.textContent = `Up to date. Latest release is ${o.latest}.`;
      install.textContent = `Reinstall ${o.latest}`;
      install.className = '';
    }
  }
  return o.state;
}

async function pollOta() {
  let state;
  do {
    await new Promise(r => setTimeout(r, 1000));
    try { state = showOta(await App.api('/api/ota')); }
    catch (e) { state = 'rebooting'; }  // board is restarting
  } while (state === 'downloading');
  if (state === 'rebooting') waitForReboot();
}

async function waitForReboot() {
  $('otamsg').className = 'msg';
  $('otamsg').textContent = 'Waiting for the board to come back…';
  await new Promise(r => setTimeout(r, 4000));
  for (;;) {
    try {
      const r = await fetch('/api/info', { cache: 'no-store' });
      if (r.ok) { location.reload(); return; }
    } catch (e) { }
    await new Promise(r => setTimeout(r, 2000));
  }
}

export default {
  mount(el, app) {
    App = app;
    el.innerHTML = html;
    $ = k => el.querySelector(`[data-k="${k}"]`);

    $('check').onclick = async () => {
      showOta({ state: 'checking' });
      try { showOta(await App.api('/api/ota/check', { method: 'POST' })); }
      catch (e) { showOta({ state: 'error', error: 'Request failed' }); }
    };

    $('install').onclick = async () => {
      if (!confirm(`Install ${$('install').textContent.replace(/^\w+ /, '')} and reboot the board?`)) return;
      try { showOta(await App.api('/api/ota/update', { method: 'POST' })); }
      catch (e) { showOta({ state: 'error', error: e.message }); return; }
      pollOta();
    };

    $('reboot').onclick = async () => {
      if (!confirm('Reboot the board?')) return;
      try { await App.api('/api/reboot', { method: 'POST' }); } catch (e) { }
      waitForReboot();
    };

    App.api('/api/ota').then(o => { if (showOta(o) === 'downloading') pollOta(); }).catch(() => { });
  },

  // Poll only while the view is visible.
  show() {
    clearInterval(timer);
    refreshInfo();
    timer = setInterval(refreshInfo, 3000);
  },

  hide() {
    clearInterval(timer);
    timer = null;
  },
};
