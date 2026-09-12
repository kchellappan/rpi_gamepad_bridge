'use strict';

// Guided loopback latency measurement.
//
// The number this produces is the whole chain minus the controller itself: socket -> bridge
// -> encode -> gadget -> USB -> host -> evdev. It cannot include the controller, because
// nothing here can press a physical button; that would need a GPIO across a button's
// contacts. Saying so in the results matters more than the figure being flattering.

const L = { device: null, detectTimer: null, running: false, pollTimer: null, duration: 60000 };
const lq = (id) => document.getElementById(id);

function latShow(step) {
  for (const id of ['lat-setup', 'lat-running', 'lat-results']) lq(id).hidden = id !== step;
}

async function detectLoopback() {
  try {
    const r = await api('/api/latency/detect', { method: 'POST' });
    L.device = r.found ? r.event : null;
    lq('lat-detect').textContent = r.found
      ? `Loopback found at ${r.event}. Ready to measure.`
      : 'Looking for the loopback… plug the data leg into a USB-A port on the Pi.';
    lq('lat-detect').classList.toggle('error', !r.found);
    lq('lat-run').disabled = !r.found;
  } catch (e) {
    lq('lat-detect').textContent = 'Could not check for the loopback: ' + e;
    lq('lat-detect').classList.add('error');
    lq('lat-run').disabled = true;
  }
}

// A histogram says more than a mean here: USB polling quantises the result, so the shape is
// the evidence that the interval dominates rather than something in software. Which only
// works if the reader can tell what the bars are worth, so it gets real axes.
function drawHistogram(samples, id) {
  const svg = lq(id || 'lat-chart');
  svg.innerHTML = '';
  if (!samples || !samples.length) return;

  const W = 560, H = 160;
  const padL = 38, padR = 12, padT = 12, padB = 30;
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;

  const lo = Math.min(...samples), hi = Math.max(...samples);
  // Use the real range. Padding it out to some minimum would push a tight cluster into a
  // corner and misrepresent exactly the case this is most often looking at.
  const span = Math.max(hi - lo, 1e-3);
  const bins = Math.min(28, Math.max(8, Math.round(Math.sqrt(samples.length) * 2)));
  const counts = new Array(bins).fill(0);
  for (const v of samples) {
    counts[Math.min(bins - 1, Math.floor(((v - lo) / span) * bins))] += 1;
  }
  const peak = Math.max(...counts, 1);
  const bw = plotW / bins;

  const el = (name, attrs, text) => {
    const n = document.createElementNS('http://www.w3.org/2000/svg', name);
    for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, String(v));
    if (text !== undefined) n.textContent = text;
    svg.appendChild(n);
    return n;
  };

  // Horizontal guides first, so bars sit on top of them.
  const yTicks = peak <= 4 ? [0, peak] : [0, Math.round(peak / 2), peak];
  for (const t of yTicks) {
    const y = padT + plotH - (t / peak) * plotH;
    el('line', { class: 'grid', x1: padL, x2: W - padR, y1: y, y2: y });
    el('text', { class: 'tick', x: padL - 6, y: y + 3, 'text-anchor': 'end' }, String(t));
  }

  counts.forEach((c, i) => {
    if (!c) return;
    const h = (c / peak) * plotH;
    el('rect', {
      class: 'bar', x: padL + i * bw + 0.5, y: padT + plotH - h,
      width: Math.max(bw - 1, 1), height: h, rx: 1.5,
    });
  });

  el('line', { class: 'axis', x1: padL, x2: W - padR, y1: padT + plotH, y2: padT + plotH });

  // Five x labels across the real range, at whatever precision distinguishes them. A
  // distribution this tight is unreadable at two decimals.
  const stepDigits = span / 4 < 0.02 ? 3 : 2;
  for (let i = 0; i <= 4; i++) {
    const frac = i / 4;
    const x = padL + frac * plotW;
    const value = lo + frac * span;
    el('line', { class: 'axis', x1: x, x2: x, y1: padT + plotH, y2: padT + plotH + 4 });
    el('text', {
      class: 'tick', x, y: padT + plotH + 15,
      'text-anchor': i === 0 ? 'start' : i === 4 ? 'end' : 'middle',
    }, value.toFixed(stepDigits));
  }
  el('text', { class: 'tick axis-label', x: padL + plotW / 2, y: H - 2,
               'text-anchor': 'middle' }, 'milliseconds');
  el('text', { class: 'tick axis-label', x: 10, y: padT + plotH / 2,
               'text-anchor': 'middle',
               transform: `rotate(-90 10 ${padT + plotH / 2})` }, 'samples');
}

function interpret(r) {
  // The gadget's endpoint is polled once per millisecond at high speed, so roughly a
  // millisecond is the floor and there is nothing in userspace left to win. Saying that
  // plainly is more useful than a number with no yardstick.
  if (r.p50_ms <= 2.0) {
    return 'This is the USB polling interval, not software overhead — the endpoint is polled '
         + 'once per millisecond, so around a millisecond is the floor. There is nothing left '
         + 'to optimise on this side.';
  }
  if (r.p50_ms <= 5.0) {
    return 'Higher than the 1 ms polling floor but still well inside one frame at 60 Hz. '
         + 'Worth checking the link negotiated high speed rather than full speed.';
  }
  return 'Well above the polling floor. Check that the gadget enumerated at high speed, and '
       + 'whether anything else on the Pi is competing for CPU.';
}

function wireLatency() {
  lq('lat-open').addEventListener('click', async () => {
    lq('latency').hidden = false;
    lq('lat-sub').textContent = 'Set up the loopback, then measure.';
    latShow('lat-setup');
    await detectLoopback();
    // Poll while the dialog is open so plugging the cable in is noticed without a click.
    clearInterval(L.detectTimer);
    L.detectTimer = setInterval(detectLoopback, 2000);
  });

  const close = async () => {
    clearInterval(L.detectTimer);
    clearInterval(L.pollTimer);
    L.detectTimer = null;
    // Abandoning a run would leave the bridge stopped with nothing on screen explaining it.
    if (L.running) { L.running = false; try { await api('/api/latency/stop', { method: 'POST' }); } catch (e) {} }
    lq('latency').hidden = true;
    refresh();
  };
  lq('lat-close').addEventListener('click', close);
  lq('lat-done').addEventListener('click', close);

  function renderLive(p) {
    lq('run-n').textContent = p.n || 0;
    lq('run-p50').textContent = p.p50_ms ? p.p50_ms.toFixed(2) : '—';
    lq('run-max').textContent = p.max_ms ? p.max_ms.toFixed(2) : '—';
    const left = Math.max(0, Math.ceil((L.duration - (p.elapsed_ms || 0)) / 1000));
    lq('run-left').textContent = String(left);
    drawHistogram(p.samples, 'run-chart');
  }

  function showResults(p) {
    const src = p.summary && p.summary.ok ? p.summary : p;
    if (!src || !src.n) {
      latShow('lat-setup');
      lq('lat-detect').textContent = 'No samples were collected.';
      lq('lat-detect').classList.add('error');
      L.detectTimer = setInterval(detectLoopback, 2000);
      return;
    }
    lq('lat-p50').textContent = src.p50_ms.toFixed(2);
    lq('lat-min').textContent = src.min_ms.toFixed(2);
    lq('lat-p95').textContent = src.p95_ms.toFixed(2);
    lq('lat-max').textContent = src.max_ms.toFixed(2);
    drawHistogram(p.samples, 'lat-chart');
    lq('lat-detail').textContent =
      `${src.n} samples over ${Math.round((p.elapsed_ms || 0) / 1000)}s, ${src.lost ?? 0} lost. `
      + `Measured from writing a report to the gadget to the host observing it: USB and evdev. `
      + `Neither the bridge's own processing (microseconds) nor the controller's latency is `
      + `included; the latter cannot be measured this way at all.`;
    // Buttons and axes share one report, so they should be indistinguishable. Showing both
    // is what turns that from an assumption into a result.
    const bt = src.button, ax = src.axis;
    if (bt && ax && bt.n && ax.n) {
      const gap = Math.abs(bt.p50_ms - ax.p50_ms);
      lq('lat-split').textContent =
        `Buttons ${bt.p50_ms.toFixed(2)} ms (n=${bt.n})  ·  Axes ${ax.p50_ms.toFixed(2)} ms `
        + `(n=${ax.n})` + (gap < 0.15
            ? ' — the same within measurement error, as expected: both ride the same report.'
            : ' — a real difference, which is worth investigating.');
      lq('lat-split').hidden = false;
    } else {
      lq('lat-split').hidden = true;
    }
    lq('lat-interp').textContent = interpret(src);
    latShow('lat-results');
    lq('lat-sub').textContent = 'Bridge restarted.';
  }

  async function poll() {
    let p;
    try {
      p = await api('/api/latency/progress', { method: 'POST' });
    } catch (e) {
      return;   // a transient failure should not abandon a run in progress
    }
    if (!L.running) return;
    if (p.running) {
      renderLive(p);
    } else {
      // The child hit its own time limit rather than being stopped from here.
      clearInterval(L.pollTimer);
      L.running = false;
      showResults(p);
      refresh(); refreshLogs();
    }
  }

  const run = async () => {
    if (L.running) return;
    clearInterval(L.detectTimer);
    latShow('lat-running');
    lq('lat-sub').textContent = 'Running. The bridge is stopped until this finishes.';
    renderLive({ samples: [] });
    try {
      const r = await api('/api/latency/start', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ duration_ms: L.duration }),
      });
      if (!r.ok) {
        latShow('lat-setup');
        lq('lat-detect').textContent = r.error || 'could not start';
        lq('lat-detect').classList.add('error');
        L.detectTimer = setInterval(detectLoopback, 2000);
        return;
      }
    } catch (e) {
      latShow('lat-setup');
      lq('lat-detect').textContent = 'Could not start: ' + e;
      lq('lat-detect').classList.add('error');
      return;
    }
    L.running = true;
    clearInterval(L.pollTimer);
    L.pollTimer = setInterval(poll, 700);
  };

  const stop = async () => {
    if (!L.running) return;
    clearInterval(L.pollTimer);
    L.running = false;
    try {
      const p = await api('/api/latency/stop', { method: 'POST' });
      showResults(p);
    } catch (e) {
      toast('could not stop cleanly: ' + e, true);
    }
    refresh(); refreshLogs();
  };

  lq('lat-stop').addEventListener('click', stop);

  lq('lat-run').addEventListener('click', run);
  lq('lat-again').addEventListener('click', run);
}

wireLatency();
