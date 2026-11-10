// Touch pins card: one bar per touch GPIO, length from the reading's rise over
// its benchmark, highlighted while touched. Live on WebSocket topic 'touch'
// (main/features/touch.c), which sends {pins:[{gpio, raw, delta, touched}], thresh_pct}.
const CSS = `
.touch-hint { color: var(--muted); margin: 0 0 12px; }
.touch-pins { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 8px 24px; }
.touch-pin { display: grid; grid-template-columns: 62px 1fr 48px; align-items: center; gap: 10px; }
.touch-pin .lbl { color: var(--muted); font-size: 13px; white-space: nowrap; }
.touch-pin .val { text-align: right; font-size: 12px; color: var(--muted); font-variant-numeric: tabular-nums; }
.touch-pin .trk { position: relative; height: 10px; background: var(--line); border-radius: 5px; overflow: hidden; }
.touch-pin .fill { position: absolute; inset: 0 auto 0 0; width: 0; background: var(--muted); opacity: .55;
  border-radius: 5px; transition: width .15s linear; }
.touch-pin .thr { position: absolute; top: 0; bottom: 0; width: 2px; background: var(--card); opacity: .9; }
.touch-pin.on .lbl { color: var(--text); font-weight: 600; }
.touch-pin.on .val { color: var(--accent); }
.touch-pin.on .fill { background: var(--accent); opacity: 1; }
`;

// Bar full scale, as a multiple of the touch threshold.
const FULL_SCALE = 4;

function injectStyle() {
  if (document.getElementById('touch-css')) return;
  const s = document.createElement('style');
  s.id = 'touch-css';
  s.textContent = CSS;
  document.head.appendChild(s);
}

export default function (App) {
  App.card({
    id: 'touch',
    title: 'Touch pins',
    order: 35,
    wide: true,
    mount(el, App) {
      injectStyle();
      el.innerHTML = `
        <p class="touch-hint">Touch a header pin (GPIO1–14) with your finger.</p>
        <div class="touch-pins" data-k="pins"><div class="sub">Waiting for data…</div></div>`;
      const list = el.querySelector('[data-k="pins"]');
      let rows = new Map();   // gpio -> {row, fill, val}

      function build(pins, thresh) {
        list.innerHTML = '';
        rows = new Map();
        for (const p of pins) {
          const row = document.createElement('div');
          row.className = 'touch-pin';
          row.innerHTML = `<span class="lbl">GPIO ${p.gpio}</span>
            <div class="trk"><div class="fill"></div><div class="thr"></div></div>
            <span class="val"></span>`;
          row.querySelector('.thr').style.left = `${100 / FULL_SCALE}%`;
          row.title = `Touched above +${thresh}% over the idle level`;
          list.appendChild(row);
          rows.set(p.gpio, { row, fill: row.querySelector('.fill'), val: row.querySelector('.val') });
        }
      }

      App.on('touch', d => {
        const pins = d?.pins || [];
        const thresh = d?.thresh_pct || 3;
        if (pins.length !== rows.size || pins.some(p => !rows.has(p.gpio))) build(pins, thresh);
        for (const p of pins) {
          const r = rows.get(p.gpio);
          const bench = p.raw - p.delta;
          const pct = bench > 0 ? (100 * p.delta) / bench : 0;
          const frac = Math.min(Math.max(pct / (thresh * FULL_SCALE), 0), 1);
          r.fill.style.width = `${(frac * 100).toFixed(1)}%`;
          // Round first so tiny negatives show as 0.0%, not -0.0%.
          const shown = Math.round(pct * 10) / 10 || 0;
          r.val.textContent = `${shown > 0 ? '+' : ''}${shown.toFixed(1)}%`;
          r.row.classList.toggle('on', !!p.touched);
        }
      });
      App.on('$open', () => App.send('touch'));
      App.send('touch');
    },
  });
}
