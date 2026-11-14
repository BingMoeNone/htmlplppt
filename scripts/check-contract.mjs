#!/usr/bin/env node
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { launchChrome, openPage } from './lib/cdp.mjs';

// Runtime.evaluate reports page-side throws in `exceptionDetails` instead of
// rejecting the CDP command, so surface them here rather than reporting a
// confusing "cannot read properties of undefined".
async function evaluate(cdp, expression) {
  const result = await cdp.command('Runtime.evaluate', { expression, returnByValue: true });
  if (result.exceptionDetails) {
    const details = result.exceptionDetails;
    const message = details.exception?.description || details.text || 'unknown page error';
    throw new Error(`page evaluation failed: ${message}`);
  }
  return result.result?.value;
}

const { values } = parseArgs({
  options: {
    html: { type: 'string' },
    chrome: { type: 'string', default: process.env.CHROME_BIN || 'google-chrome' },
    port: { type: 'string', default: '9334' }
  }
});

if (!values.html) {
  console.error('usage: node scripts/check-contract.mjs --html <file>');
  process.exit(2);
}

const htmlPath = path.resolve(values.html);
const userDataDir = await mkdtemp(path.join(tmpdir(), 'html2ppt-check-'));
let chrome;
let cdp;
let exitCode = 0;
try {
  chrome = await launchChrome({ chromeBin: values.chrome, port: Number(values.port), userDataDir });
  const url = pathToFileURL(htmlPath);
  url.searchParams.set('capture', '1');
  // Open about:blank so the only navigation to the capture URL is the one
  // below, whose load event listener is registered first.
  cdp = await openPage(Number(values.port), 'about:blank');
  await cdp.command('Page.enable');
  await cdp.command('Runtime.enable');
  const loaded = cdp.once('Page.loadEventFired');
  await cdp.command('Page.navigate', { url: url.href });
  await loaded;

  const value = await evaluate(
    cdp,
    `(() => {
      const s = window.__tefSteps;
      if (!s || typeof s !== 'object') {
        return { ok: false, reason: 'missing window.__tefSteps { total, durations, seek }' };
      }
      if (!Number.isInteger(s.total)) {
        return { ok: false, reason: 'total must be an integer' };
      }
      if (s.total < 2) {
        return { ok: false, reason: 'total must be >= 2' };
      }
      if (!Array.isArray(s.durations)) {
        return { ok: false, reason: 'durations must be an array' };
      }
      if (s.durations.length !== s.total) {
        return { ok: false, reason: 'durations.length (' + s.durations.length + ') !== total (' + s.total + ')' };
      }
      if (typeof s.seek !== 'function') {
        return { ok: false, reason: 'seek must be a function' };
      }
      for (let i = 0; i < s.durations.length; i += 1) {
        const d = s.durations[i];
        if (typeof d !== 'number' || !Number.isFinite(d) || d <= 0) {
          return { ok: false, reason: 'durations[' + i + '] must be a positive finite number (got ' + String(d) + ')' };
        }
      }
      for (let i = 0; i < s.total; i += 1) {
        try {
          s.seek(i, 0);
        } catch (err) {
          return { ok: false, reason: 'seek(' + i + ', 0) threw: ' + (err && err.message ? err.message : String(err)) };
        }
        try {
          s.seek(i, s.durations[i]);
        } catch (err) {
          return { ok: false, reason: 'seek(' + i + ', ' + s.durations[i] + ') threw: ' + (err && err.message ? err.message : String(err)) };
        }
      }
      return { ok: true, total: s.total, durations: s.durations.slice() };
    })()`
  );

  if (!value || value.ok !== true) {
    console.error(`FAIL: ${value?.reason ?? 'contract check returned no result'}`);
    exitCode = 1;
  } else {
    console.log(`OK: ${value.total} steps, durations=${value.durations.join(',')}ms`);
  }
} catch (err) {
  console.error(`FAIL: ${err?.message ?? err}`);
  exitCode = 1;
} finally {
  cdp?.close();
  chrome?.proc.kill('SIGTERM');
  await new Promise((resolve) => setTimeout(resolve, 300));
  chrome?.proc.kill('SIGKILL');
  await rm(userDataDir, { recursive: true, force: true });
}
process.exitCode = exitCode;
