import test from 'node:test';
import assert from 'node:assert/strict';
import { matchWindowsFont } from '../scripts/lib/fonts.mjs';

const fontsDir = 'C:\\Windows\\Fonts';

test('font preflight matches the default Microsoft YaHei file', () => {
  const hit = matchWindowsFont('Microsoft YaHei', { fontsDir, files: ['msyh.ttc'] });
  assert.ok(hit.endsWith('msyh.ttc'));
});

test('font preflight accepts a Noto Serif CJK family', () => {
  const hit = matchWindowsFont('Noto Serif CJK SC', {
    fontsDir,
    files: ['NotoSerifCJKsc-Regular.otf'],
  });
  assert.ok(hit.endsWith('NotoSerifCJKsc-Regular.otf'));
});

test('font preflight fails closed for an unknown family', () => {
  assert.throws(
    () => matchWindowsFont('Comic Sans MS', { fontsDir, files: ['msyh.ttc'] }),
    /unsupported REQUIRED_FONT/
  );
});

test('font preflight reports a known family whose files are missing', () => {
  assert.throws(
    () => matchWindowsFont('SimSun', { fontsDir, files: ['msyh.ttc'] }),
    /not found under/
  );
});
