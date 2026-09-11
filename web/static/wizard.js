'use strict';

// The mapping wizard asks the question the other way round from the terminal version.
//
// The terminal wizard says "press the bottom face button" and infers intent from physical
// position. Here we show the device being emulated and ask which of YOUR controls should be
// its A button. The user states intent directly, so nothing has to be inferred -- which is
// what makes evdev's misleading BTN_NORTH/BTN_WEST aliases irrelevant rather than a trap.

const W = {
  device: null,
  target: null,
  steps: [],          // flattened capture steps
  index: 0,
  mappings: [],       // {control, target, kind, code, invert}
  running: false,
  gen: 0,             // invalidates an in-flight capture when the user moves on
  abort: null,        // AbortController for the capture currently being awaited
};

const wq = (id) => document.getElementById(id);

function svgEl(name, attrs) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', name);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
  return el;
}

function drawTarget(target) {
  const svg = wq('wiz-svg');
  svg.innerHTML = '';
  svg.setAttribute('viewBox', `0 0 ${target.view[0]} ${target.view[1]}`);
  svg.appendChild(svgEl('path', { d: target.body, class: 'pad-body' }));

  for (const c of target.controls) {
    if (!c.r) continue;                       // stick clicks highlight their stick instead
    const g = svgEl('g', { 'data-control': c.id });
    if (c.shape === 'bumper') {
      g.appendChild(svgEl('rect', {
        x: c.x - c.r * 1.7, y: c.y - c.r * 0.6, width: c.r * 3.4, height: c.r * 1.2,
        rx: c.r * 0.6, class: 'ctl', 'data-shape': c.id,
      }));
    } else {
      g.appendChild(svgEl('circle', { cx: c.x, cy: c.y, r: c.r, class: 'ctl' }));
    }
    const label = svgEl('text', { x: c.x, y: c.y, class: 'ctl-label' });
    label.textContent = c.label.length > 4 ? '' : c.label;
    g.appendChild(label);
    svg.appendChild(g);
  }
}

function markDiagram() {
  const step = W.steps[W.index];
  const activeId = step ? (step.control.highlight || step.control.id) : null;
  const doneIds = new Set(W.mappings.map((m) => m.highlight || m.control));
  for (const g of wq('wiz-svg').querySelectorAll('[data-control]')) {
    const id = g.dataset.control;
    const shape = g.querySelector('.ctl');
    shape.classList.toggle('active', id === activeId);
    shape.classList.toggle('done', id !== activeId && doneIds.has(id));
  }
}

// A control may need several captures (a stick needs one per axis), so the flat step list is
// what drives progress rather than the control list.
function buildSteps(target) {
  const steps = [];
  for (const c of target.controls) {
    if (Array.isArray(c.steps)) {
      for (const st of c.steps) steps.push({ control: c, target: st.target, prompt: st.prompt });
    } else {
      steps.push({ control: c, target: c.target, prompt: c.prompt });
    }
  }
  return steps;
}

// Feedback belongs inside the modal, next to the thing it is about. A toast sits outside
// the overlay and reads as an alert regardless of what it says.
function note(message, kind) {
  const el = wq('wiz-note');
  if (!message) { el.hidden = true; el.textContent = ''; return; }
  el.textContent = message;
  el.classList.toggle('error', kind === 'error');
  el.hidden = false;
}

function show(step) {
  for (const id of ['wiz-setup', 'wiz-capture', 'wiz-save']) wq(id).hidden = id !== step;
}

async function loadChoices() {
  const [{ devices }, { targets }] = await Promise.all([
    api('/api/devices'), api('/api/targets'),
  ]);

  const dbox = wq('wiz-devices');
  dbox.innerHTML = devices.length ? '' :
    '<p class="hint">No controllers found. Plug one into the Pi and reopen this.</p>';
  devices.forEach((d, i) => {
    const el = document.createElement('label');
    el.className = 'option';
    el.innerHTML = `<input type="radio" name="wizdev" ${i === 0 ? 'checked' : ''}>
      <span><span class="name">${esc(d.name)}</span>
      <span class="detail">${esc(d.path)}</span></span>`;
    el.querySelector('input').addEventListener('change', () => { W.device = d.path; mark(dbox, el); });
    dbox.appendChild(el);
    if (i === 0) { W.device = d.path; el.setAttribute('aria-checked', 'true'); }
  });

  const tbox = wq('wiz-targets');
  tbox.innerHTML = '';
  targets.forEach((t, i) => {
    const el = document.createElement('label');
    el.className = 'option';
    el.innerHTML = `<input type="radio" name="wiztgt" ${i === 0 ? 'checked' : ''}>
      <span><span class="name">${esc(t.name)}</span>
      <span class="desc">${esc(t.summary || '')}</span></span>`;
    el.querySelector('input').addEventListener('change', () => { W.target = t; mark(tbox, el); });
    tbox.appendChild(el);
    if (i === 0) { W.target = t; el.setAttribute('aria-checked', 'true'); }
  });
}

function mark(box, chosen) {
  for (const el of box.querySelectorAll('.option')) {
    el.setAttribute('aria-checked', String(el === chosen));
  }
}

// Each step runs under a generation number. Skipping or cancelling bumps it and aborts the
// request being awaited, so a capture that completes afterwards recognises that it has been
// superseded and does nothing.
//
// Without that, Skip could only set the index -- the in-flight capture kept running, so the
// button looked dead until the full timeout elapsed, and the late response then advanced the
// index a second time and silently skipped an extra control.
async function nextStep(keepNote) {
  if (!W.running) return;
  if (W.index >= W.steps.length) return toSave();
  if (!keepNote) note('');

  const step = W.steps[W.index];
  const gen = ++W.gen;
  wq('wiz-prompt').textContent = step.prompt;
  wq('wiz-progress').textContent = `${W.index + 1} of ${W.steps.length}  ·  ${step.control.label}`;
  markDiagram();

  // Exclude codes already bound, so a control that answered an earlier prompt cannot answer
  // this one too -- the same rule the terminal wizard enforces.
  const exclude = W.mappings.map((m) => m.code);
  W.abort = new AbortController();
  let res;
  try {
    res = await api('/api/wizard/capture', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      signal: W.abort.signal,
      body: JSON.stringify({
        device: W.device, kind: step.control.capture || 'any',
        timeout_ms: 8000, exclude,
      }),
    });
  } catch (e) {
    if (gen !== W.gen) return;          // aborted deliberately; a newer step owns the flow
    note('Could not read the controller: ' + e, 'error');
    W.running = false;
    return;
  }
  if (gen !== W.gen || !W.running) return;   // superseded while we were waiting

  if (!res.ok && res.error === 'already_bound') {
    // The user pressed something real; it is just spoken for. Retry the SAME step rather
    // than advancing: they almost certainly want to map this target, not skip it. And this
    // is information, not an error -- some pads genuinely have no spare control.
    const owner = W.mappings.find((m) => m.code === res.code);
    note(`${res.code} is already mapped to ${owner ? owner.label : 'another control'}. `
       + `Try a different control, or Skip if this pad has none to spare.`);
    return nextStep(true);
  }

  if (res.ok) {
    const hat = /^ABS_HAT0[XY]$/.test(res.code);
    let targetName = step.target;
    let invert = res.invert;

    if (res.kind === 'axis') {
      if (hat) {
        // Many pads report the d-pad as a hat. It binds as an AXIS pair, not as four
        // button targets: writing `axis.ABS_HAT0Y = dup` produces a binding EvdevSource
        // has no axis target for, so it is silently dropped and the d-pad simply does not
        // exist. Direction comes from the hat's own sign, so no invert either.
        targetName = res.code.endsWith('X') ? 'hatx' : 'haty';
        invert = false;
      } else if (step.control.target_axis) {
        targetName = step.control.target_axis;   // e.g. an analog trigger answering ZL
      } else {
        // An axis answered a prompt with no axis equivalent. Recording it would emit a
        // binding that cannot work, so decline it and retry the same step.
        note(`${step.control.label} needs a button, but that was an axis (${res.code}). `
           + `Try a button, or Skip.`);
        return nextStep(true);
      }
    }

    W.mappings.push({
      control: step.control.id, highlight: step.control.highlight,
      label: step.control.label, target: targetName,
      kind: res.kind, code: res.code, invert,
    });
  }
  W.index += 1;
  nextStep();
}

function abortInFlight() {
  W.gen += 1;
  if (W.abort) { W.abort.abort(); W.abort = null; }
}

function skipStep() {
  if (!W.running) return;
  abortInFlight();
  W.index += 1;
  nextStep();
}

function toSave() {
  show('wiz-save');
  wq('wiz-sub').textContent = `${W.mappings.length} of ${W.steps.length} captured`;
  wq('wiz-summary').textContent = W.mappings.length
    ? W.mappings.map((m) => `${m.label.padEnd(12)} ${m.kind === 'axis' ? (m.invert ? '-' : '') : ''}${m.code}`).join('\n')
    : '(nothing captured)';
  const stamp = new Date().toISOString().slice(0, 10).replace(/-/g, '');
  wq('wiz-file').value = `mapping_${stamp}.ini`;
  wq('wiz-name').value = wq('wiz-name').value || `Mapping to ${W.target.name}`;
}

async function finish(message) {
  W.running = false;
  abortInFlight();
  try { await api('/api/wizard/finish', { method: 'POST' }); } catch (e) { /* reported below */ }
  wq('wizard').hidden = true;
  if (message) toast(message);
  refresh(); refreshLogs();
}

function wireWizard() {
  wq('wizard-open').addEventListener('click', async () => {
    W.index = 0; W.mappings = []; W.running = false;
    wq('wizard').hidden = false;
    wq('wiz-sub').textContent = 'Choose what to map, and what to emulate.';
    show('wiz-setup');
    try { await loadChoices(); } catch (e) { toast('could not load choices: ' + e, true); }
  });

  wq('wiz-begin').addEventListener('click', async () => {
    if (!W.device || !W.target) return toast('pick a controller and a target', true);
    try { await api('/api/wizard/begin', { method: 'POST' }); }
    catch (e) { return toast('could not stop the bridge: ' + e, true); }
    W.steps = buildSteps(W.target);
    W.index = 0; W.mappings = []; W.running = true;
    wq('wiz-sub').textContent = 'Press the control on YOUR pad that should act as the highlighted one.';
    drawTarget(W.target);
    show('wiz-capture');
    nextStep();
  });

  // Skipping advances without recording, so a pad missing a control does not strand the run.
  wq('wiz-skip').addEventListener('click', () => { note(''); skipStep(); });

  wq('wiz-abort').addEventListener('click', () => finish('wizard cancelled; bridge restarted'));
  wq('wiz-abort2').addEventListener('click', () => finish('discarded; bridge restarted'));
  wq('wiz-close').addEventListener('click', () => {
    if (W.running || !wq('wiz-capture').hidden || !wq('wiz-save').hidden) {
      finish('wizard closed; bridge restarted');
    } else {
      wq('wizard').hidden = true;
    }
  });

  wq('wiz-write').addEventListener('click', async (e) => {
    e.target.disabled = true;
    try {
      const r = await api('/api/wizard/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          target: W.target.id, device: W.device,
          name: wq('wiz-name').value, description: wq('wiz-desc').value,
          filename: wq('wiz-file').value,
          mappings: W.mappings,
        }),
      });
      if (!r.ok) { toast(r.message, true); e.target.disabled = false; return; }
      lastConfigSignature = '';           // force the config list to re-render with the new file
      await finish(r.message);
    } catch (err) {
      toast(String(err), true);
      e.target.disabled = false;
    }
  });
}

wireWizard();
