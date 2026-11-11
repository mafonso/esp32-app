// Small live line chart for cards: a fixed window of samples with the newest at the
// right edge, a shaded area, two gridlines labelled with the Y range, and X labels.
//
//   import { lineChart } from '/chart.js';
//   const chart = lineChart(el, { points: 300, minSpan: 4, xLabels: ['-10m', '-5m', 'now'] });
//   chart.set(samples);   // replace all samples (oldest first)
//   chart.push(value);    // append one; drops the oldest beyond `points`
//   chart.message(text);  // shown over the chart while it has fewer than 2 samples
const CSS = `
.chart { position: relative; }
.chart svg { display: block; width: 100%; overflow: visible; }
.chart .gl { stroke: var(--line); stroke-width: 1; }
.chart .line { fill: none; stroke: var(--accent); stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
.chart .area { fill: var(--accent); opacity: .12; stroke: none; }
.chart-y { position: absolute; left: 0; font-size: 11px; color: var(--muted); background: var(--card);
  padding: 0 4px 0 0; font-variant-numeric: tabular-nums; line-height: 1; }
.chart-y.top { top: 0; transform: translateY(-50%); } .chart-y.bot { bottom: 0; transform: translateY(50%); }
.chart-x { display: flex; justify-content: space-between; font-size: 11px; color: var(--muted); margin-top: 8px; }
.chart-msg { position: absolute; inset: 0; display: grid; place-items: center; color: var(--muted); font-size: 13px; }
`;

const W = 300;  // SVG user units; stretched to the element's width

export function lineChart(el, { points, minSpan = 1, height = 120, xLabels = [], label = '', decimals = 1 }) {
  if (!document.getElementById('chart-css')) {
    const s = document.createElement('style');
    s.id = 'chart-css';
    s.textContent = CSS;
    document.head.appendChild(s);
  }
  const H = height;
  el.innerHTML = `
    <div class="chart">
      <svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" style="height:${H}px">
        <line class="gl" x1="0" y1="0.5" x2="${W}" y2="0.5" vector-effect="non-scaling-stroke"/>
        <line class="gl" x1="0" y1="${H / 2}" x2="${W}" y2="${H / 2}" vector-effect="non-scaling-stroke"/>
        <line class="gl" x1="0" y1="${H - 0.5}" x2="${W}" y2="${H - 0.5}" vector-effect="non-scaling-stroke"/>
        <path class="area"/>
        <path class="line" vector-effect="non-scaling-stroke"/>
      </svg>
      <span class="chart-y top"></span><span class="chart-y bot"></span>
      <div class="chart-msg">Collecting samples…</div>
    </div>
    <div class="chart-x"></div>`;
  const q = s => el.querySelector(s);
  q('svg').setAttribute('aria-label', label);
  for (const t of xLabels) q('.chart-x').appendChild(document.createElement('span')).textContent = t;
  let samples = [];

  function draw() {
    const n = samples.length;
    q('.chart-msg').hidden = n >= 2;
    if (n < 2) {
      q('.line').setAttribute('d', '');
      q('.area').setAttribute('d', '');
      return;
    }
    let lo = Infinity, hi = -Infinity;
    for (const v of samples) { if (v < lo) lo = v; if (v > hi) hi = v; }
    // Y range: data plus ~15% padding, but at least minSpan centred on the data,
    // so small noise doesn't fill the whole height.
    const mid = (lo + hi) / 2;
    const half = Math.max(((hi - lo) * 1.3) / 2, minSpan / 2);
    const y0 = mid - half, y1 = mid + half;
    q('.chart-y.top').textContent = y1.toFixed(decimals);
    q('.chart-y.bot').textContent = y0.toFixed(decimals);
    const step = W / (points - 1);
    const x = i => (W - (n - 1 - i) * step).toFixed(1);
    const y = v => (H - ((v - y0) / (y1 - y0)) * H).toFixed(1);
    let d = '';
    for (let i = 0; i < n; i++) d += (i ? 'L' : 'M') + x(i) + ' ' + y(samples[i]);
    q('.line').setAttribute('d', d);
    q('.area').setAttribute('d', `${d}L${x(n - 1)} ${H}L${x(0)} ${H}Z`);
  }

  return {
    set(values) { samples = values.slice(-points); draw(); },
    push(v) {
      samples.push(v);
      if (samples.length > points) samples.splice(0, samples.length - points);
      draw();
    },
    get samples() { return samples; },
    message(text) { q('.chart-msg').textContent = text; },
  };
}
