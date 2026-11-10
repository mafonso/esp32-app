// Example feature card (template for new ones): live board stats pushed by
// main/features/sysinfo.c on WebSocket topic 'sysinfo'.
export default function (App) {
  App.card({
    id: 'sysinfo',
    title: 'System',
    order: 10,
    mount(el, App) {
      const { kb, mb, duration } = App.fmt;
      el.innerHTML = `
        <dl>
          <dt>Uptime</dt><dd data-k="uptime">–</dd>
          <dt>Heap free</dt><dd data-k="heap">–</dd>
          <dt>PSRAM free</dt><dd data-k="psram">–</dd>
          <dt>Temperature</dt><dd data-k="temp">–</dd>
        </dl>`;
      const $ = k => el.querySelector(`[data-k="${k}"]`);

      App.on('sysinfo', b => {
        $('uptime').textContent = duration(b.uptime_s);
        $('heap').textContent = kb(b.heap_free);
        $('psram').textContent = b.psram_bytes ? mb(b.psram_free) : 'none';
        $('temp').textContent = b.temp_c !== undefined ? b.temp_c.toFixed(1) + ' °C' : 'n/a';
      });

      // Ask for a sample now, and again after every reconnect.
      App.on('$open', () => App.send('sysinfo'));
      App.send('sysinfo');
    },
  });
}
