// Settings → System → Factory reset: POST /api/factory-reset (main/features/factory_reset.c).
// The board comes back without a network, so this page can't follow it: it says
// where to find the setup hotspot instead.
export default function (App) {
  App.settings({
    id: 'factory_reset', title: 'Factory reset', category: 'system', order: 50,
    mount(el) {
      el.innerHTML = `
        <p class="hint" style="margin-top:0">Erases the Wi-Fi network, hostname, admin password and every
          feature setting, then reboots. The firmware stays. Also: hold BOOT for 10 s, or
          <code>factory-reset yes</code> on the console.</p>
        <div class="row"><button data-k="go">Factory reset…</button></div>
        <div class="msg"></div>`;
      const go = el.querySelector('[data-k="go"]'), msg = el.querySelector('.msg');
      go.onclick = async () => {
        if (!confirm('Erase all settings, including the Wi-Fi network, and reboot the board?')) return;
        let def = '';
        try { def = (await App.api('/api/settings/name')).default; } catch (e) { }
        try {
          await App.api('/api/factory-reset', { method: 'POST' });
        } catch (e) {
          if (e.status === 401) return;  // sign-in cancelled
          msg.className = 'msg err';
          msg.textContent = 'Reset failed: ' + e.message;
          return;
        }
        go.disabled = true;
        msg.className = 'msg ok';
        msg.textContent = `Settings erased, rebooting. To set it up again, join the Wi-Fi hotspot `
          + `${def ? `'${def}'` : 'named after the board'} and open http://192.168.4.1/.`;
      };
    },
  });
}
