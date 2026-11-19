import test from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { readFileSync } from 'node:fs';

const root = dirname(dirname(fileURLToPath(import.meta.url)));

test('stepwise demo satisfies the HTML contract', () => {
  const out = execFileSync(
    process.execPath,
    [join(root, 'scripts/check-contract.mjs'), '--html', join(root, 'templates/stepwise-demo.html')],
    { cwd: root, encoding: 'utf8' },
  );
  assert.match(out, /OK: 2 steps/);
});

test('live browser scripts stay ASCII for Windows Script Host', () => {
  const files = [
    'scripts/watch-live-content.vbs',
    'scripts/stop-live.vbs',
    'scripts/black-fade.ps1',
    'scripts/focus-agent.ps1',
    'scripts/focus-lib.ps1',
    'scripts/smoke-watchdog.ps1',
  ];
  for (const rel of files) {
    const text = readFileSync(join(root, rel), 'utf8');
    assert.ok(!/[^\x00-\x7F]/.test(text), `${rel} contains non-ASCII characters`);
  }
});

test('vendored python-pptx imports', () => {
  const python = process.platform === 'win32' ? 'python' : 'python3';
  const out = execFileSync(python, ['-c', 'import pptx; print(pptx.__version__)'], {
    cwd: root,
    encoding: 'utf8',
    env: { ...process.env, PYTHONPATH: join(root, 'vendor') },
  });
  assert.match(out.trim(), /^1\.0\.2/);
});

test('vendored python-pptx can create a blank presentation', () => {
  // Presentation() loads the bundled default.pptx template. A vendor copy
  // installed with --no-deps can miss it, which breaks every from-scratch
  // deck without failing the plain `import pptx` check above.
  const python = process.platform === 'win32' ? 'python' : 'python3';
  const out = execFileSync(
    python,
    ['-c', 'from pptx import Presentation; print(len(Presentation().slides))'],
    {
      cwd: root,
      encoding: 'utf8',
      env: { ...process.env, PYTHONPATH: join(root, 'vendor') },
    },
  );
  assert.match(out.trim(), /^\d+$/);
});
