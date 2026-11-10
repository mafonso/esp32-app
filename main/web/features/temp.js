// Chip temperature card: current value, min/max and a 10-minute line chart.
// History from GET /api/temp/history, live samples on WebSocket topic 'temp'
// (main/features/temp.c).
const CSS = `
.temp-now { font-size: 34px; font-weight: 650; line-height: 1.1; font-variant-numeric: tabular-nums; }
.temp-now small { font-size: 16px; font-weight: 500; color: var(--muted); margin-left: 2px; }
.temp-mm { color: var(--muted); font-variant-numeric: tabular-nums; margin: 2px 0 12px; }
.temp-chart { position: relative; }
.temp-chart svg { display: block; width: 100%; height: 120px; overflow: visible; }
.temp-chart .gl { stroke: var(--line); stroke-width: 1; }
.temp-chart .line { fill: none; stroke: var(--accent); stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
.temp-chart .area { fill: var(--accent); opacity: .12; stroke: none; }
.temp-y { position: absolute; left: 0; font-size: 11px; color: var(--muted); background: var(--card);
  padding: 0 4px 0 0; font-variant-numeric: tabular-nums; line-height: 1; }
.temp-y.top { top: 0; transform: translateY(-50%); } .temp-y.bot { bottom: 0; transform: translateY(50%); }
.temp-x { display: flex; justify-content: space-between; font-size: 11px; color: var(--muted); margin-top: 8px; }
.temp-empty { position: absolute; inset: 0; display: grid; place-items: center; color: var(--muted); font-size: 13px; }
`;

const W = 300, H = 120;            // SVG user units; stretched to the card width
const MAX_SAMPLES = 300;           // 10 minutes at 2 s, matching temp.c

function injectStyle() {
  if (document.getElementById('temp-css')) return;
  const s = document.createElement('style');
  s.id = 'temp-css';
  s.textContent = CSS;
  document.head.appendChild(s);
}

export default function (App) {
  App.card({
    id: 'temp',
    title: 'Temperature',
    order: 30,
    mount(el, App) {
      injectStyle();
      el.innerHTML = `
        <div class="temp-now"><span data-k="now">–</span><small>°C</small></div>
        <div class="temp-mm" data-k="mm">&nbsp;</div>
        <div class="temp-chart">
          <svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" aria-label="Temperature, last 10 minutes">
            <line class="gl" x1="0" y1="0.5" x2="${W}" y2="0.5" vector-effect="non-scaling-stroke"/>
            <line class="gl" x1="0" y1="${H / 2}" x2="${W}" y2="${H / 2}" vector-effect="non-scaling-stroke"/>
            <line class="gl" x1="0" y1="${H - 0.5}" x2="${W}" y2="${H - 0.5}" vector-effect="non-scaling-stroke"/>
            <path class="area" data-k="area"/>
            <path class="line" data-k="line" vector-effect="non-scaling-stroke"/>
          </svg>
          <span class="temp-y top" data-k="ytop"></span>
          <span class="temp-y bot" data-k="ybot"></span>
          <div class="temp-empty" data-k="empty">Collecting samples…</div>
        </div>
        <div class="temp-x"><span>-10m</span><span>-5m</span><span>now</span></div>`;
      const $ = k => el.querySelector(`[data-k="${k}"]`);
      let samples = [];
      let interval = 2;

      function draw() {
        const n = samples.length;
        $('empty').hidden = n >= 2;
        if (!n) return;
        const cur = samples[n - 1];
        let lo = Infinity, hi = -Infinity;
        for (const c of samples) { if (c < lo) lo = c; if (c > hi) hi = c; }
        $('now').textContent = cur.toFixed(1);
        $('mm').textContent = `min ${lo.toFixed(1)} · max ${hi.toFixed(1)} °C`;

        // Y range with ~15% padding, and at least 1 °C so noise doesn't look like a cliff.
        const pad = Math.max((hi - lo) * 0.15, (1 - (hi - lo)) / 2, 0.1);
        const y0 = lo - pad, y1 = hi + pad;
        $('ytop').textContent = y1.toFixed(1);
        $('ybot').textContent = y0.toFixed(1);
        if (n < 2) { $('line').setAttribute('d', ''); $('area').setAttribute('d', ''); return; }

        // Newest sample at the right edge; the window is always 10 minutes wide.
        const step = W / (MAX_SAMPLES - 1);
        const x = i => (W - (n - 1 - i) * step).toFixed(1);
        const y = c => (H - ((c - y0) / (y1 - y0)) * H).toFixed(1);
        let d = '';
        for (let i = 0; i < n; i++) d += (i ? 'L' : 'M') + x(i) + ' ' + y(samples[i]);
        $('line').setAttribute('d', d);
        $('area').setAttribute('d', `${d}L${x(n - 1)} ${H}L${x(0)} ${H}Z`);
      }

      async function load() {
        try {
          const h = await App.api('/api/temp/history');
          interval = h.interval_s || interval;
          samples = (h.samples || []).slice(-MAX_SAMPLES);
          draw();
        } catch (e) {
          $('empty').textContent = 'History unavailable';
        }
      }

      App.on('temp', d => {
        if (typeof d?.c !== 'number') return;
        samples.push(d.c);
        if (samples.length > MAX_SAMPLES) samples.splice(0, samples.length - MAX_SAMPLES);
        draw();
      });
      // Reload after every (re)connect: samples kept coming while we were away.
      App.on('$open', load);
      load();
    },
  });
}
