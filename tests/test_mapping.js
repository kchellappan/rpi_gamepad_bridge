'use strict';

// Unit tests for the wizard's target resolution. This is the part of the UI that is real
// logic rather than rendering, and it has been wrong twice on real hardware -- both times
// invisible until someone ran the wizard on an actual controller.

const assert = require('assert');
const { resolveTarget } = require('../web/static/mapping.js');

const lstick = { id: 'lstick', label: 'Left stick', capture: 'axis' };
const dup = { id: 'dup', label: 'D-pad up', capture: 'button', target: 'dup' };
const zl = { id: 'zl', label: 'ZL', capture: 'any', target: 'l2', target_axis: 'lt' };
const face = { id: 'a', label: 'A', capture: 'button', target: 'east' };

const cases = [
  ['a button answering a button prompt',
    { control: face, target: 'east' }, { kind: 'button', code: 'BTN_EAST' },
    { target: 'east' }],

  // Regression: a stick's own axis was rejected as "needs a button", because the control
  // has no target_axis and the axis branch fell through to the error case.
  ['a stick axis answering its own prompt',
    { control: lstick, target: 'ly' }, { kind: 'axis', code: 'ABS_Y', invert: false },
    { target: 'ly' }],

  ['an inverted stick axis keeps its inversion',
    { control: lstick, target: 'lx' }, { kind: 'axis', code: 'ABS_X', invert: true },
    { target: 'lx', invert: true }],

  // Regression: a hat was recorded against the button target name, producing
  // "axis.ABS_HAT0Y = dup", which EvdevSource has no axis target for and silently drops.
  ['a hat answering a d-pad prompt binds as the hat axis, never the button name',
    { control: dup, target: 'dup' }, { kind: 'axis', code: 'ABS_HAT0Y', invert: true },
    { target: 'haty', invert: false }],

  ['an analog trigger answering ZL binds as the trigger axis',
    { control: zl, target: 'l2' }, { kind: 'axis', code: 'ABS_BRAKE', invert: false },
    { target: 'lt' }],

  ['a digital shoulder answering ZL binds as the button',
    { control: zl, target: 'l2' }, { kind: 'button', code: 'BTN_TL2' },
    { target: 'l2' }],
];

let failures = 0;
for (const [name, step, res, expected] of cases) {
  const got = resolveTarget(step, res);
  try {
    assert.strictEqual(got.target, expected.target);
    if ('invert' in expected) assert.strictEqual(got.invert, expected.invert);
    assert.ok(!got.error, `unexpected error: ${got.error}`);
    console.log(`  PASS  ${name}`);
  } catch (e) {
    console.log(`  FAIL  ${name}`);
    console.log(`        expected ${JSON.stringify(expected)}, got ${JSON.stringify(got)}`);
    failures += 1;
  }
}

// An axis cannot satisfy a face button, and saying so is the point: recording it would emit
// a binding that cannot work.
const bad = resolveTarget({ control: face, target: 'east' },
                          { kind: 'axis', code: 'ABS_X', invert: false });
if (bad.error) {
  console.log('  PASS  an axis answering a face button is refused');
} else {
  console.log(`  FAIL  an axis answering a face button should be refused, got ${JSON.stringify(bad)}`);
  failures += 1;
}

process.exit(failures ? 1 : 0);
