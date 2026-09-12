'use strict';

// Selection is held locally until Apply, so a poll landing mid-edit never yanks the choice
// out from under the user.
let pending = { config: null };
let activeKind = null;          // which tab is being viewed
let lastConfigSignature = '';

// A tab per kind of input, derived from what each config declares. The mode is not a
// separate setting that could contradict a config -- it is which tab you are looking at,
// and choosing a config under it selects both at once.
const KINDS = [
  { id: 'controller',   label: 'Controller',
    hint: 'Driven by a controller plugged into the Pi.' },
  { id: 'network',      label: 'Network',
    hint: 'Driven over UDP by another machine. Capture is published back for alignment.' },
  { id: 'programmatic', label: 'Programmatic',
    hint: 'Driven locally over a Unix socket, for replaying a capture or scripting on the Pi.' },
  { id: 'other',        label: 'Other',
    hint: 'The source this config declares is not one the panel recognises.' },
];

const $ = (id) => document.getElementById(id);

// Config values are read from files on disk and interpolated into markup below. Escaping
// them is not optional: a device path or description containing markup would otherwise be
// injected straight into the page.
const esc = (v) => String(v ?? '').replace(/[&<>"']/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

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
  // Link health first. This is the claim a user actually cares about, and the one the page
  // used to get wrong: a USB link can report itself connected while the endpoint is dead
  // and every report fails, which looked identical to working.
  const link = s.link || { level: 'unknown', headline: '', detail: '' };
  const input = s.input || { level: 'unknown', headline: '', detail: '' };

  // One banner, showing whichever end is actually broken. Input problems are surfaced here
  // as loudly as USB ones because they fail identically from the user's side -- the service
  // healthy, the client apparently sending, and nothing happening.
  // Dropped capture samples are silent data loss, so they belong in the banner beside a
  // dead USB link rather than only on a card.
  const worst = link.level === 'bad' ? link
              : input.level === 'bad' ? input
              : (s.capture && s.capture.level === 'bad') ? s.capture
              : link.level !== 'ok' ? link : input;
  const banner = $('link-banner');
  banner.className = 'banner ' + worst.level;
  banner.hidden = worst.level === 'ok';
  $('link-head').textContent = worst.headline || '';
  $('link-detail').textContent = worst.detail || '';
  // Re-enumerating only helps the USB end; offering it for an input problem would be
  // pointing at the wrong half of the system.
  $('link-fix').hidden = !(worst === link && link.level === 'bad');

  // Only shown when something is actually capturing, so the common case stays uncluttered.
  const cap = s.capture || { level: 'off' };
  $('card-capture').hidden = cap.level === 'off';
  $('capture-state').textContent = cap.headline || '—';
  $('capture-state').className = 'state ' + (cap.level === 'ok' ? 'ok'
                                          : cap.level === 'bad' ? 'bad' : 'warn');
  $('capture-detail').textContent = cap.detail || '';

  const st0 = s.stats || {};
  $('input-state').textContent = input.headline || '—';
  $('input-state').className = 'state ' + (input.level === 'ok' ? 'ok'
                                        : input.level === 'bad' ? 'bad' : 'warn');
  const sc = st0.source_counters || {};
  $('input-meta').textContent = [st0.source, s.active && s.active.source]
      .filter((v, i, a) => v && a.indexOf(v) === i).join('  ·  ') || ' ';
  $('input-detail').textContent = input.detail || '';

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
    // "connected" now means reports are landing, not merely that the link enumerated.
    const attached = u.state === 'configured';
    const flowing = (s.link || {}).level === 'ok';
    $('usb-state').textContent = !attached ? u.state : (flowing ? 'sending' : 'not sending');
    $('usb-state').className = 'state ' + (flowing ? 'ok' : attached ? 'bad' : 'warn');
    const st = s.stats || {};
    $('usb-meta').textContent = [u.name, u.speed,
      st.submits !== undefined ? `${st.submits} reports` : null].filter(Boolean).join('  ·  ');
  }

  // Config list. Only re-render when the set of files actually changes, so polling does not
  // fight the user's cursor.
  const signature = s.configs.map((c) => c.path + ':' + (c.kind || '')).join('|');
  // Default to the tab holding whatever is running, so the page opens where the user is.
  const activeCfg = s.configs.find((c) => c.path === s.active.config);
  if (activeKind === null) activeKind = (activeCfg && activeCfg.kind) || 'controller';

  renderTabs(s);

  if (signature !== lastConfigSignature || renderTabs.dirty) {
    lastConfigSignature = signature;
    renderTabs.dirty = false;
    const list = $('config-list');
    list.innerHTML = '';
    const shown = s.configs.filter((c) => (c.kind || 'other') === activeKind);
    if (!shown.length) {
      list.innerHTML = '<p class="hint">No configs of this kind yet. The mapping wizard '
                     + 'writes controller configs; network ones are written by hand.</p>';
    }
    for (const c of shown) {
      const el = document.createElement('label');
      el.className = 'option';
      el.setAttribute('role', 'radio');
      const facts = [c.sink && 'sink: ' + c.sink,
                     c.heartbeat && 'heartbeat: ' + c.heartbeat + ' Hz',
                     c.name].filter(Boolean).join('  ·  ');
      el.innerHTML = `
        <input type="radio" name="config" value="${esc(c.path)}">
        <span>
          <span class="name">${esc(c.title || c.name)}</span><span class="badge" hidden>active</span>
          ${c.description ? `<span class="desc">${esc(c.description)}</span>` : ''}
          <span class="detail">${esc(facts)}</span>
        </span>`;
      el.querySelector('input').addEventListener('change', () => {
        pending.config = c.path;
        markSelection(s.active.config);
      });
      // Deleting is offered per row, but the server refuses to remove the config that is
      // currently selected -- losing the file driving a live console should take a
      // deliberate switch first, not one stray click.
      const del = document.createElement('button');
      del.className = 'btn btn-quiet btn-danger';
      del.textContent = 'Delete';
      del.style.marginLeft = 'auto';
      del.addEventListener('click', async (ev) => {
        ev.preventDefault(); ev.stopPropagation();
        if (!confirm(`Delete ${c.name}?\n\nThis cannot be undone.`)) return;
        try {
          const r = await api('/api/config/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ config: c.path }),
          });
          toast(r.message, !r.ok);
          if (r.ok) { lastConfigSignature = ''; refresh(); }
        } catch (err) { toast(String(err), true); }
      });
      el.appendChild(del);
      list.appendChild(el);
    }
  }
  markSelection(s.active.config);
  $('apply-note').textContent = isDirty(s) ? 'unsaved changes' : '';
}

function renderTabs(s) {
  const counts = {};
  for (const c of s.configs) {
    const k = c.kind || 'other';
    counts[k] = (counts[k] || 0) + 1;
  }
  const activeCfg = s.configs.find((c) => c.path === s.active.config);
  const runningKind = activeCfg && activeCfg.kind;

  // Only offer tabs that hold something, except the one being viewed -- removing the tab
  // under the user's feet because a file was deleted elsewhere would be jarring.
  const kinds = KINDS.filter((k) => counts[k.id] || k.id === activeKind);
  const sig = kinds.map((k) => `${k.id}:${counts[k.id] || 0}:${k.id === activeKind}:${k.id === runningKind}`).join('|');
  if (renderTabs.sig === sig) return;
  renderTabs.sig = sig;
  renderTabs.dirty = true;

  const bar = $('kind-tabs');
  bar.innerHTML = '';
  for (const k of kinds) {
    const b = document.createElement('button');
    b.className = 'tab';
    b.type = 'button';
    b.setAttribute('role', 'tab');
    b.setAttribute('aria-selected', String(k.id === activeKind));
    // A dot marks the tab whose config is actually running, so "what is live" stays visible
    // from whichever tab you happen to be on.
    b.innerHTML = `${esc(k.label)}<span class="count">${counts[k.id] || 0}</span>`
                + (k.id === runningKind ? '<span class="live-dot" title="running"></span>' : '');
    b.addEventListener('click', () => {
      activeKind = k.id;
      pending.config = null;      // a selection does not carry across tabs
      renderTabs.sig = null;
      lastConfigSignature = '';
      if (latest) renderStatus(latest);
    });
    bar.appendChild(b);
  }
  const hint = KINDS.find((k) => k.id === activeKind);
  $('kind-hint').textContent = hint ? hint.hint : '';
}

function markSelection(activePath) {
  const chosen = pending.config ?? activePath;
  // Scoped to the config list on purpose. Reaching across the whole document for `.option`
  // also caught the wizard's device and target rows -- so the two-second status poll quietly
  // unchecked whatever the user had just selected in the modal, then threw on their missing
  // `.badge` and abandoned the rest of the render. The intermittency was the poll interval.
  const list = $('config-list');
  if (!list) return;
  list.querySelectorAll('.option').forEach((el) => {
    const input = el.querySelector('input');
    if (!input) return;
    const isChosen = input.value === chosen;
    input.checked = isChosen;
    el.setAttribute('aria-checked', String(isChosen));
    const badge = el.querySelector('.badge');
    if (badge) badge.hidden = input.value !== activePath;
  });
}

function isDirty(s) {
  return Boolean(pending.config) && pending.config !== s.active.config;
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
          restart: true,
        }),
      });
      toast(r.message || 'applied', !r.ok);
      if (r.ok) pending = { config: null };
    } catch (err) { toast(String(err), true); }
    e.target.disabled = false;
    refresh(); refreshLogs();
  });

  $('link-fix').addEventListener('click', () => $('gadget-restart').click());
  $('hostline').textContent = location.host;
}

wire();
refresh();
refreshLogs();
setInterval(refresh, 2000);
setInterval(refreshLogs, 3000);
