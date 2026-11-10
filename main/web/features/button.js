// BOOT button card, for main/features/button.c: topic 'button' carries
// {pressed, count, long_press} on every press, long press and release.
const CSS = `
.btn-live { display: flex; align-items: center; gap: 14px; }
.btn-dot { width: 44px; height: 44px; border-radius: 50%; flex: none; border: 2px solid var(--line);
  background: var(--bg); transition: background-color .08s, border-color .08s, transform .08s; }
.btn-dot.on { background: var(--accent); border-color: var(--accent); transform: scale(.92); }
.btn-state { font-size: 18px; font-weight: 600; }
.btn-long { margin-left: auto; padding: 4px 10px; border-radius: 999px; font-size: 13px; font-weight: 600;
  color: var(--warn); border: 1px solid currentColor; opacity: 0; transition: opacity .3s; }
.btn-long.show { opacity: 1; transition: none; }`;

export default function (App) {
  if (!document.getElementById('button-css')) {
    const s = document.createElement('style');
    s.id = 'button-css';
    s.textContent = CSS;
    document.head.appendChild(s);
  }

  App.card({
    id: 'button',
    title: 'BOOT button',
    order: 25,
    mount(el, App) {
      el.innerHTML = `
        <div class="btn-live">
          <div class="btn-dot" data-k="dot"></div>
          <div>
            <div class="btn-state" data-k="state">–</div>
            <div class="sub" data-k="count">–</div>
          </div>
          <span class="btn-long" data-k="long">long press!</span>
        </div>
        <p class="msg">Press the BOOT button on the board. A short press cycles the LED mode.</p>`;
      const $ = k => el.querySelector(`[data-k="${k}"]`);
      let longTimer;

      App.on('button', b => {
        if (!b) return;
        $('dot').classList.toggle('on', b.pressed);
        $('state').textContent = b.pressed ? 'Pressed' : 'Released';
        $('count').textContent = `${b.count} press${b.count === 1 ? '' : 'es'}`;
        if (b.pressed && b.long_press) {
          $('long').classList.add('show');
          clearTimeout(longTimer);
          longTimer = setTimeout(() => $('long').classList.remove('show'), 1500);
        }
      });

      App.on('$open', () => App.send('button'));
      App.send('button');
    },
  });
}
