'use strict';

// Guided loopback latency measurement.
//
// The number this produces is the whole chain minus the controller itself: socket -> bridge
// -> encode -> gadget -> USB -> host -> evdev. It cannot include the controller, because
// nothing here can press a physical button; that would need a GPIO across a button's
// contacts. Saying so in the results matters more than the figure being flattering.

const L = { device: null, detectTimer: null, running: false };
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
// the evidence that the interval dominates rather than something in software.
function drawHistogram(samples) {
  const svg = lq('lat-chart');
  svg.innerHTML = '';
  if (!samples || !samples.length) return;
  const W = 520, H = 120, pad = 18;
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);

  const lo = Math.min(...samples), hi = Math.max(...samples);
  const span = Math.max(hi - lo, 0.2);
  const bins = 24;
  const counts = new Array(bins).fill(0);
  for (const v of samples) {
    counts[Math.min(bins - 1, Math.floor(((v - lo) / span) * bins))] += 1;
  }
  const peak = Math.max(...counts, 1);
  const bw = (W - pad * 2) / bins;

  counts.forEach((c, i) => {
    const h = (c / peak) * (H - pad * 2);
    const r = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    r.setAttribute('class', 'bar');
    r.setAttribute('x', String(pad + i * bw + 1));
    r.setAttribute('y', String(H - pad - h));
    r.setAttribute('width', String(Math.max(bw - 2, 1)));
    r.setAttribute('height', String(h));
    r.setAttribute('rx', '2');
    svg.appendChild(r);
  });

  const axis = document.createElementNS('http://www.w3.org/2000/svg', 'line');
  axis.setAttribute('class', 'axis');
  axis.setAttribute('x1', String(pad)); axis.setAttribute('x2', String(W - pad));
  axis.setAttribute('y1', String(H - pad)); axis.setAttribute('y2', String(H - pad));
  svg.appendChild(axis);

  for (const [frac, val] of [[0, lo], [1, hi]]) {
    const t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    t.setAttribute('class', 'tick');
    t.setAttribute('x', String(pad + frac * (W - pad * 2)));
    t.setAttribute('y', String(H - 4));
    t.setAttribute('text-anchor', frac ? 'end' : 'start');
    t.textContent = `${val.toFixed(2)} ms`;
    svg.appendChild(t);
  }
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

  const close = () => {
    clearInterval(L.detectTimer);
    L.detectTimer = null;
    lq('latency').hidden = true;
    refresh();
  };
  lq('lat-close').addEventListener('click', () => { if (!L.running) close(); });
  lq('lat-done').addEventListener('click', close);

  const run = async () => {
    if (L.running) return;
    L.running = true;
    clearInterval(L.detectTimer);
    latShow('lat-running');
    lq('lat-sub').textContent = 'Running. The bridge is in socket mode until this finishes.';
    const samples = parseInt(lq('lat-samples').value, 10) || 60;
    try {
      const r = await api('/api/latency/run', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ samples }),
      });
      if (!r.ok) {
        latShow('lat-setup');
        lq('lat-detect').textContent = r.error || 'measurement failed';
        lq('lat-detect').classList.add('error');
        L.detectTimer = setInterval(detectLoopback, 2000);
      } else {
        lq('lat-p50').textContent = r.p50_ms.toFixed(2);
        lq('lat-min').textContent = r.min_ms.toFixed(2);
        lq('lat-p95').textContent = r.p95_ms.toFixed(2);
        lq('lat-max').textContent = r.max_ms.toFixed(2);
        drawHistogram(r.samples);
        lq('lat-detail').textContent =
          `${r.n} samples, ${r.lost} lost. Measured from injecting an input to the host `
          + `observing it: bridge, encoding, USB and evdev. The controller's own latency is `
          + `not included and cannot be measured this way.`;
        lq('lat-interp').textContent = interpret(r);
        latShow('lat-results');
        lq('lat-sub').textContent = `Source restored to ${r.restored_source}.`;
      }
    } catch (e) {
      latShow('lat-setup');
      lq('lat-detect').textContent = 'Measurement failed: ' + e;
      lq('lat-detect').classList.add('error');
    }
    L.running = false;
    refresh(); refreshLogs();
  };

  lq('lat-run').addEventListener('click', run);
  lq('lat-again').addEventListener('click', run);
}

wireLatency();
