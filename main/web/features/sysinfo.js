// Example feature card (template for new ones): live board stats pushed by
// main/features/sysinfo.c on WebSocket topic 'sysinfo', and the chip temperature
// from main/features/temp.c: history from GET /api/temp/history, live samples on
// topic 'temp'. Without the temp feature the card shows the stats only.
import { lineChart } from '/chart.js';

const CSS = `
.sys { display: grid; grid-template-columns: minmax(200px, 1fr) 2fr; gap: 16px 32px; align-items: start; }
.sys-now { font-size: 34px; font-weight: 650; line-height: 1.1; font-variant-numeric: tabular-nums; }
.sys-now small { font-size: 16px; font-weight: 500; color: var(--muted); margin-left: 2px; }
.sys-mm { color: var(--muted); font-variant-numeric: tabular-nums; margin: 2px 0 14px; font-size: 13px; }
.sys-chart { padding-top: 8px; }
.sys.no-temp { grid-template-columns: 1fr; }
.sys.no-temp .sys-t, .sys.no-temp .sys-chart { display: none; }
@media (max-width: 720px) { .sys { grid-template-columns: 1fr; } }
`;

const POINTS = 300;  // 10 minutes at 2 s, matching temp.c

export default function (App) {
  if (!document.getElementById('sysinfo-css')) {
    const s = document.createElement('style');
    s.id = 'sysinfo-css';
    s.textContent = CSS;
    document.head.appendChild(s);
  }
  App.card({
    id: 'sysinfo',
    title: 'System',
    order: 10,
    span: 3,
    mount(el, App) {
      const { kb, mb, duration } = App.fmt;
      el.innerHTML = `
        <div class="sys">
          <div>
            <div class="sys-t">
              <div class="sys-now"><span data-k="now">–</span><small>°C</small></div>
              <div class="sys-mm" data-k="mm">&nbsp;</div>
            </div>
            <dl>
              <dt>Uptime</dt><dd data-k="uptime">–</dd>
              <dt>Heap free</dt><dd data-k="heap">–</dd>
              <dt>PSRAM free</dt><dd data-k="psram">–</dd>
            </dl>
          </div>
          <div class="sys-chart" data-k="chart"></div>
        </div>`;
      const $ = k => el.querySelector(`[data-k="${k}"]`);
      const chart = lineChart($('chart'), {
        points: POINTS, minSpan: 4, height: 130, label: 'Chip temperature, last 10 minutes',
        xLabels: ['-10m', '-5m', 'now'],
      });

      App.on('sysinfo', b => {
        $('uptime').textContent = duration(b.uptime_s);
        $('heap').textContent = kb(b.heap_free);
        $('psram').textContent = b.psram_bytes ? mb(b.psram_free) : 'none';
      });

      function showTemp() {
        const s = chart.samples;
        if (!s.length) return;
        $('now').textContent = s[s.length - 1].toFixed(1);
        $('mm').textContent = `min ${Math.min(...s).toFixed(1)} · max ${Math.max(...s).toFixed(1)} °C, last 10 min`;
      }
      async function loadTemp() {
        try {
          chart.set((await App.api('/api/temp/history')).samples || []);
          el.firstElementChild.classList.remove('no-temp');
          showTemp();
        } catch (e) {
          if (e.status === 404) el.firstElementChild.classList.add('no-temp');  // no temp feature
          else chart.message('History unavailable');
        }
      }
      App.on('temp', d => {
        if (typeof d?.c !== 'number') return;
        chart.push(d.c);
        showTemp();
      });

      // Ask for a sample now, and again after every reconnect (the temperature
      // history kept growing while we were away).
      App.on('$open', () => { App.send('sysinfo'); loadTemp(); });
      App.send('sysinfo');
      loadTemp();
    },
  });
}
