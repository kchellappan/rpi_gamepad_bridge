'use strict';

// Deciding which config target a captured control maps to.
//
// Pulled out of the wizard because it is pure logic with no DOM in it, and because getting
// it wrong is invisible until someone runs the wizard on real hardware. It has been wrong
// twice: once writing a hat's axis code against a button target name (producing a binding
// EvdevSource silently dropped), and once rejecting a stick's own axis as "needs a button".
// See tests/test_mapping.js.
//
// A step describes what the target device wants. A capture describes what the user's pad
// actually produced. The two do not always agree, and the interesting cases are the
// mismatches:
//
//   hat answering a d-pad prompt  -> binds as the hatx/haty axis pair, not four buttons
//   analog trigger answering ZL   -> binds as the lt/rt axis, via target_axis
//   axis answering a face button  -> cannot work; reject and let the user retry
function resolveTarget(step, res) {
  const control = step.control || {};
  const kindWanted = control.capture || 'any';

  if (res.kind !== 'axis') {
    return { target: step.target, invert: false };
  }

  if (/^ABS_HAT0[XY]$/.test(res.code)) {
    // Direction comes from the hat's own sign, so it is never inverted here.
    return { target: res.code.endsWith('X') ? 'hatx' : 'haty', invert: false };
  }

  if (kindWanted === 'axis') {
    // The step asked for an axis and got one; step.target is already an axis name.
    return { target: step.target, invert: res.invert };
  }

  if (control.target_axis) {
    // e.g. an analog trigger satisfying ZL, which is a button on the target device.
    return { target: control.target_axis, invert: res.invert };
  }

  return { error: `${control.label} needs a button, but that was an axis (${res.code}).` };
}

if (typeof module !== 'undefined' && module.exports) module.exports = { resolveTarget };
