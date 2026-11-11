// RGB LED card and settings section, for main/features/led.c.
// Topic 'led' carries the full state {mode, color, brightness, speed, gpio}
// both ways: every browser gets each change, and sends partial changes.
const MODES = ['off', 'solid', 'breathe', 'blink', 'rainbow'];

const CSS = `
.led-top { display: flex; align-items: center; gap: 12px; margin-bottom: 14px; }
.led-dot { width: 28px; height: 28px; border-radius: 50%; flex: none; border: 1px solid var(--line);
  background: var(--line); transition: background-color .2s; }
.led-dot.breathe { animation: led-breathe var(--period) ease-in-out infinite; }
.led-dot.blink { animation: led-blink var(--period) step-end infinite; }
.led-dot.rainbow { background: #ff0000 !important; animation: led-rainbow var(--period) linear infinite; }
@keyframes led-breathe { 0%, 100% { opacity: .1; } 50% { opacity: 1; } }
@keyframes led-blink { 0% { opacity: 1; } 50% { opacity: .1; } }
@keyframes led-rainbow { to { filter: hue-rotate(360deg); } }
.led-seg { display: grid; grid-template-columns: repeat(auto-fit, minmax(68px, 1fr)); gap: 4px; padding: 4px;
  border: 1px solid var(--line); border-radius: 10px; background: var(--bg); }
.led-seg button { border: 0; background: none; color: var(--muted); padding: 6px 4px; border-radius: 7px; font-size: 14px; }
.led-seg button:hover { color: var(--text); }
.led-seg button[aria-pressed="true"] { background: var(--accent); color: var(--accent-text); }
.led-f { display: grid; grid-template-columns: 82px minmax(0, 1fr) 60px; align-items: center; gap: 10px; margin-top: 14px; }
.led-f > span:first-child { color: var(--muted); }
.led-f output, .led-f .led-hex { text-align: right; font-variant-numeric: tabular-nums; color: var(--muted); font-size: 13px; }
.led-f input[type=range] { width: 100%; margin: 0; accent-color: var(--accent); }
.led-f input[type=color] { width: 100%; max-width: 72px; height: 32px; padding: 2px; border: 1px solid var(--line);
  border-radius: 8px; background: var(--card); cursor: pointer; }
.led-f.na { opacity: .45; }
.led-set label { display: grid; gap: 6px; }
.led-set label > span { color: var(--muted); }
.led-set select { font: inherit; max-width: 320px; padding: 8px 10px; border: 1px solid var(--line); border-radius: 8px;
  background: var(--card); color: var(--text); }`;

function addStyle() {
  if (document.getElementById('led-css')) return;
  const s = document.createElement('style');
  s.id = 'led-css';
  s.textContent = CSS;
  document.head.appendChild(s);
}

// Animation periods, matching frame() in led.c.
const period = (mode, speed) =>
  mode === 'breathe' ? 5500 - 500 * speed : mode === 'blink' ? 2200 - 200 * speed : 11000 - 1000 * speed;

function mountCard(el, App) {
  el.innerHTML = `
    <div class="led-top">
      <span class="led-dot" data-k="dot"></span>
      <span class="sub" data-k="summary">–</span>
    </div>
    <div class="led-seg" role="group" aria-label="Mode">
      ${MODES.map(m => `<button type="button" data-mode="${m}" aria-pressed="false">${m[0].toUpperCase() + m.slice(1)}</button>`).join('')}
    </div>
    <label class="led-f" data-f="color"><span>Colour</span><input type="color" data-k="color" value="#2080ff"><span class="led-hex" data-k="hex"></span></label>
    <label class="led-f" data-f="brightness"><span>Brightness</span><input type="range" min="0" max="100" data-k="brightness"><output data-k="bv"></output></label>
    <label class="led-f" data-f="speed"><span>Speed</span><input type="range" min="1" max="10" data-k="speed"><output data-k="sv"></output></label>`;
  const $ = k => el.querySelector(`[data-k="${k}"]`);
  const field = f => el.querySelector(`[data-f="${f}"]`);

  // Local view of the state; the board's broadcasts are the source of truth.
  let st = { mode: 'off', color: '#2080ff', brightness: 20, speed: 5 };
  const editedAt = {};   // control -> time of the user's last input

  function render(from) {
    el.querySelectorAll('[data-mode]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.mode === st.mode)));
    // Leave a control alone while the user is working it, so echoes don't make it jump.
    const busy = k => from === 'remote' && Date.now() - (editedAt[k] || 0) < 800;
    if (!busy('color')) $('color').value = st.color;
    if (!busy('brightness')) $('brightness').value = st.brightness;
    if (!busy('speed')) $('speed').value = st.speed;
    $('hex').textContent = st.color;
    $('bv').textContent = st.brightness + '%';
    $('sv').textContent = st.speed;
    field('color').classList.toggle('na', st.mode === 'off' || st.mode === 'rainbow');
    field('speed').classList.toggle('na', st.mode === 'off' || st.mode === 'solid');

    const dot = $('dot');
    dot.className = 'led-dot' + (['breathe', 'blink', 'rainbow'].includes(st.mode) ? ' ' + st.mode : '');
    dot.style.setProperty('--period', period(st.mode, st.speed) + 'ms');
    dot.style.backgroundColor = st.mode === 'off' ? '' : st.color;
    // Dim the preview with brightness, but keep it visible at low settings.
    dot.style.opacity = st.mode === 'off' || st.mode === 'breathe' || st.mode === 'blink' ? '' : String(0.35 + 0.65 * st.brightness / 100);
    $('summary').textContent = st.gpio !== undefined ? `${st.mode} · GPIO ${st.gpio}` : st.mode;
  }

  // Coalesce slider drags into at most ~10 messages per second.
  let pending = null, timer = null, lastSent = 0;
  function flush() {
    timer = null;
    if (!pending) return;
    App.send('led', pending);
    pending = null;
    lastSent = Date.now();
  }
  function send(patch) {
    pending = { ...pending, ...patch };
    const wait = 100 - (Date.now() - lastSent);
    if (wait <= 0) flush();
    else if (!timer) timer = setTimeout(flush, wait);
  }

  function edit(k, v) {
    editedAt[k] = Date.now();
    st = { ...st, [k]: v };
    render('local');
    send({ [k]: v });
  }

  el.querySelectorAll('[data-mode]').forEach(b => { b.onclick = () => edit('mode', b.dataset.mode); });
  $('color').oninput = e => edit('color', e.target.value);
  $('brightness').oninput = e => edit('brightness', Number(e.target.value));
  $('speed').oninput = e => edit('speed', Number(e.target.value));

  App.on('led', d => {
    if (!d) return;
    st = { ...st, ...d };
    render('remote');
  });
  App.on('$open', () => App.send('led'));
  App.send('led');
  render('local');
}

export default function (App) {
  addStyle();

  App.card({ id: 'led', title: 'RGB LED', order: 20, span: 2, mount: mountCard });

  let stored = 48, sel;
  App.settings({
    id: 'led', title: 'RGB LED', order: 50,
    mount(el, ctx) {
      el.className = 'led-set';
      el.innerHTML = `
        <label><span>LED data pin</span>
          <select data-k="gpio">
            <option value="48">GPIO 48 (DevKitC-1 v1.0)</option>
            <option value="38">GPIO 38 (DevKitC-1 v1.1)</option>
          </select>
        </label>
        <p class="msg">LED not lighting up? Your board may use GPIO 38.</p>`;
      sel = el.querySelector('select');
      sel.onchange = () => ctx.dirty(Number(sel.value) !== stored);
      // Follow changes made elsewhere, unless the user is editing this section.
      App.on('led', d => {
        if (!d || d.gpio === undefined) return;
        const untouched = Number(sel.value) === stored;
        stored = d.gpio;
        if (untouched) sel.value = String(stored);
      });
      App.api('/api/led').then(d => { stored = d.gpio; sel.value = String(stored); }).catch(() => { });
    },
    async save() {
      const d = await App.api('/api/led', { method: 'POST', body: { gpio: Number(sel.value) } });
      stored = d.gpio;
    },
    reset() { sel.value = String(stored); },
  });
}
