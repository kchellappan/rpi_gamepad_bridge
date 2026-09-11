'use strict';

// Selection is held locally until Apply, so a poll landing mid-edit never yanks the choice
// out from under the user.
let pending = { config: null, source: null };
let lastConfigSignature = '';

const $ = (id) => document.getElementById(id);

async function api(path, opts) {
  const r = await fetch(path, opts);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.json();
}

function toast(message, bad) {
  const t = $('toast');
  t.textContent = message;
  t.classList.toggle('bad', !!bad);
  t.classList.add('show');
  clearTimeout(toast._t);
  toast._t = setTimeout(() => t.classList.remove('show'), 3200);
}

function stateClass(active) {
  if (active === 'active') return 'ok';
  if (active === 'activating' || active === 'deactivating') return 'warn';
  return 'bad';
}

function renderStatus(s) {
  // Bridge
  $('bridge-state').textContent = s.bridge.active;
  $('bridge-state').className = 'state ' + stateClass(s.bridge.active);
  const since = s.bridge.since ? s.bridge.since.replace(/^[A-Za-z]{3} /, '') : '';
  $('bridge-meta').textContent =
    [s.bridge.enabled, since && 'since ' + since,
     s.bridge.restarts && s.bridge.restarts !== '0' && s.bridge.restarts + ' restarts']
    .filter(Boolean).join('  ·  ') || ' ';

  // USB. This reads sysfs rather than a service state: "configured" means a host is really
  // there, which is the only thing that proves the far end is listening.
  const u = s.udc;
  if (!u.present) {
    $('usb-state').textContent = 'no UDC';
    $('usb-state').className = 'state bad';
    $('usb-meta').textContent = 'peripheral mode is not enabled';
  } else {
    const attached = u.state === 'configured';
    $('usb-state').textContent = attached ? 'connected' : u.state;
    $('usb-state').className = 'state ' + (attached ? 'ok' : 'warn');
    $('usb-meta').textContent = `${u.name}  ·  ${u.speed}`;
  }

  // Mode
  const source = pending.source ?? s.active.source;
  document.querySelectorAll('.seg').forEach((b) => {
    b.setAttribute('aria-pressed', String(b.dataset.source === source));
  });
  $('source-hint').textContent = source === 'socket'
    ? 'Input comes from an application over the Unix socket. The controller is ignored.'
    : 'Input comes from the controller named in the selected config.';

  // Config list. Only re-render when the set of files actually changes, so polling does not
  // fight the user's cursor.
  const signature = s.configs.map((c) => c.path).join('|');
  if (signature !== lastConfigSignature) {
    lastConfigSignature = signature;
    const list = $('config-list');
    list.innerHTML = '';
    if (!s.configs.length) {
      list.innerHTML = '<p class="hint">No .ini files found in config/.</p>';
    }
    for (const c of s.configs) {
      const el = document.createElement('label');
      el.className = 'option';
      el.setAttribute('role', 'radio');
      el.innerHTML = `
        <input type="radio" name="config" value="${c.path}">
        <span>
          <span class="name">${c.name}</span><span class="badge" hidden>active</span>
          <span class="detail">${[c.sink && 'sink: ' + c.sink,
                                  c.heartbeat && 'heartbeat: ' + c.heartbeat + ' Hz',
                                  c.device].filter(Boolean).join('  ·  ')}</span>
        </span>`;
      el.querySelector('input').addEventListener('change', () => {
        pending.config = c.path;
        markSelection(s.active.config);
      });
      list.appendChild(el);
    }
  }
  markSelection(s.active.config);
  $('apply-note').textContent = isDirty(s) ? 'unsaved changes' : '';
}

function markSelection(activePath) {
  const chosen = pending.config ?? activePath;
  document.querySelectorAll('.option').forEach((el) => {
    const input = el.querySelector('input');
    const isChosen = input.value === chosen;
    input.checked = isChosen;
    el.setAttribute('aria-checked', String(isChosen));
    el.querySelector('.badge').hidden = input.value !== activePath;
  });
}

function isDirty(s) {
  return (pending.config && pending.config !== s.active.config) ||
         (pending.source && pending.source !== s.active.source);
}

let latest = null;
async function refresh() {
  try {
    latest = await api('/api/status');
    renderStatus(latest);
    $('pulse').className = 'dot on';
    $('updated').textContent = new Date().toLocaleTimeString();
  } catch (e) {
    $('pulse').className = 'dot err';
    $('updated').textContent = 'disconnected';
  }
}

async function refreshLogs() {
  if (!$('follow').checked) return;
  try {
    const { lines } = await api('/api/logs?n=250');
    const box = $('logs');
    const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 40;
    box.textContent = lines.join('\n') || '(no log entries)';
    if (atBottom) box.scrollTop = box.scrollHeight;
  } catch (e) { /* the status pulse already reports connectivity */ }
}

function wire() {
  document.querySelectorAll('[data-action]').forEach((b) => {
    b.addEventListener('click', async () => {
      b.disabled = true;
      try {
        const r = await api('/api/service', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ action: b.dataset.action }),
        });
        toast(r.message || 'done', !r.ok);
      } catch (e) { toast(String(e), true); }
      b.disabled = false;
      refresh(); refreshLogs();
    });
  });

  document.querySelectorAll('.seg').forEach((b) => {
    b.addEventListener('click', () => {
      pending.source = b.dataset.source;
      if (latest) renderStatus(latest);
    });
  });

  $('gadget-restart').addEventListener('click', async (e) => {
    if (!confirm('Re-enumerate the gadget?\n\nThe console will see the controller disconnect '
               + 'and reconnect. Only needed if something has wedged.')) return;
    e.target.disabled = true;
    try {
      const r = await api('/api/gadget', { method: 'POST' });
      toast(r.message || 'done', !r.ok);
    } catch (err) { toast(String(err), true); }
    e.target.disabled = false;
    refresh();
  });

  $('apply').addEventListener('click', async (e) => {
    if (!latest) return;
    e.target.disabled = true;
    try {
      const r = await api('/api/select', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          config: pending.config ?? latest.active.config,
          source: pending.source ?? latest.active.source,
          restart: true,
        }),
      });
      toast(r.message || 'applied', !r.ok);
      if (r.ok) pending = { config: null, source: null };
    } catch (err) { toast(String(err), true); }
    e.target.disabled = false;
    refresh(); refreshLogs();
  });

  $('hostline').textContent = location.host;
}

wire();
refresh();
refreshLogs();
setInterval(refresh, 2000);
setInterval(refreshLogs, 3000);
