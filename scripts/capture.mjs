#!/usr/bin/env node
import { execFileSync } from 'node:child_process';
import { readdirSync } from 'node:fs';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { launchChrome, openPage } from './lib/cdp.mjs';
import { frameTimes } from './lib/schedule.mjs';

// Runtime.evaluate reports page-side throws in `exceptionDetails` instead of
// rejecting the CDP command, so surface them here rather than capturing stale
// frames and exiting successfully.
async function evaluate(cdp, expression, { awaitPromise = false } = {}) {
  const result = await cdp.command('Runtime.evaluate', { expression, returnByValue: true, awaitPromise });
  if (result.exceptionDetails) {
    const details = result.exceptionDetails;
    const message = details.exception?.description || details.text || 'unknown page error';
    throw new Error(`page evaluation failed: ${message} (expression: ${expression})`);
  }
  return result.result?.value;
}

const REQUIRED_FONT = process.env.REQUIRED_FONT || 'Microsoft YaHei';

// The rendered video bakes the font into pixels. Capturing before the intended
// font is installed silently produces a deck with the wrong typeface, so fail
// before launching Chrome instead of after rendering hundreds of frames.
// Windows ships no fontconfig, so fc-match is unavailable there. Map the
// requested family straight to the font file the renderer will pick; the
// default "Microsoft YaHei" is msyh.ttc on every zh-CN install.
const WINDOWS_FONT_FAMILIES = [
  ['microsoft yahei', [/^msyh(?:bd)?\.ttc$/i]],
  ['dengxian', [/^deng(?:xian|b|i)?\.ttf$/i]],
  ['simhei', [/^simhei\.ttf$/i]],
  ['simsun', [/^simsun\.ttc$/i]],
  ['noto sans cjk', [/^notosanscjk/i]],
  ['source han', [/^sourcehan/i]]
];

function matchWindowsFont(required) {
  const fontsDir = path.join(process.env.WINDIR ?? 'C:\\Windows', 'Fonts');
  let files;
  try {
    files = readdirSync(fontsDir);
  } catch (error) {
    throw new Error(`font preflight failed: cannot read ${fontsDir} (${error.message})`);
  }
  const wanted = required.toLowerCase();
  const family = WINDOWS_FONT_FAMILIES.find(([name]) => name === wanted);
  const groups = family ? [family] : WINDOWS_FONT_FAMILIES;
  for (const [, patterns] of groups) {
    for (const pattern of patterns) {
      const hit = files.find((file) => pattern.test(file));
      if (hit) return path.join(fontsDir, hit);
    }
  }
  throw new Error(
    `font preflight failed: "${required}" not found under ${fontsDir}. ` +
      'Install the family or set REQUIRED_FONT to one of: ' +
      WINDOWS_FONT_FAMILIES.map(([name]) => name).join(', ')
  );
}

function assertFontAvailable() {
  let match;
  if (process.platform === 'win32') {
    match = `${REQUIRED_FONT}: ${matchWindowsFont(REQUIRED_FONT)}`;
  } else {
    try {
      match = execFileSync('fc-match', [REQUIRED_FONT], { encoding: 'utf8' }).trim();
    } catch (error) {
      throw new Error(`font preflight failed: fc-match unavailable (${error.message})`);
    }
    const normalized = match.toLowerCase();
    if (!normalized.includes(REQUIRED_FONT.toLowerCase()) && !/msyh|deng|noto sans cjk|source han/.test(normalized)) {
      throw new Error(
        `font preflight failed: "${REQUIRED_FONT}" resolved to "${match}". ` +
          'Install the font under ~/.local/share/fonts and run fc-cache -f, or set REQUIRED_FONT.'
      );
    }
  }
  console.log(`font: ${match}`);
}

const { values } = parseArgs({
  options: {
    html: { type: 'string' },
    out: { type: 'string' },
    width: { type: 'string', default: '1920' },
    height: { type: 'string', default: '1080' },
    fps: { type: 'string', default: '60' },
    chrome: { type: 'string', default: process.env.CHROME_BIN || 'google-chrome' },
    port: { type: 'string', default: '9333' }
  }
});

if (!values.html || !values.out) {
  console.error('usage: node scripts/capture.mjs --html <file> --out <dir> [--width 1920] [--height 1080] [--fps 60]');
  process.exit(2);
}

const htmlPath = path.resolve(values.html);
const outDir = path.resolve(values.out);
const width = Number(values.width);
const height = Number(values.height);
const fps = Number(values.fps);
assertFontAvailable();
const userDataDir = await mkdtemp(path.join(tmpdir(), 'html2ppt-chrome-'));

let chrome;
let cdp;
try {
  chrome = await launchChrome({ chromeBin: values.chrome, port: Number(values.port), userDataDir });
  const url = pathToFileURL(htmlPath);
  url.searchParams.set('capture', '1');
  // Open about:blank so the only navigation to the capture URL is the one
  // below, whose load event listener is registered first.
  cdp = await openPage(Number(values.port), 'about:blank');
  await cdp.command('Page.enable');
  await cdp.command('Runtime.enable');
  await cdp.command('Emulation.setDeviceMetricsOverride', {
    width, height, deviceScaleFactor: 1, mobile: false
  });

  const loaded = cdp.once('Page.loadEventFired');
  await cdp.command('Page.navigate', { url: url.href });
  await loaded;
  await evaluate(cdp, 'document.fonts.ready.then(() => true)', { awaitPromise: true });

  const metaJson = await evaluate(
    cdp,
    'JSON.stringify({ total: window.__tefSteps?.total, durations: window.__tefSteps?.durations, seek: typeof window.__tefSteps?.seek })'
  );
  const meta = typeof metaJson === 'string' ? JSON.parse(metaJson) : undefined;
  if (
    !meta ||
    !Number.isInteger(meta.total) || meta.total < 1 ||
    meta.seek !== 'function' ||
    !Array.isArray(meta.durations) || meta.durations.length !== meta.total
  ) {
    throw new Error(
      'window.__tefSteps contract missing or invalid: need integer total >= 1, durations array of length total, and a seek function; see design spec section 5'
    );
  }

  const steps = [];
  for (let i = 0; i < meta.total; i += 1) {
    const durationMs = Number(meta.durations[i]);
    const frameDir = path.join(outDir, 'frames', `step-${String(i + 1).padStart(2, '0')}`);
    // Clear prior runs so a rerun with lower fps or fewer steps cannot leave
    // stale PNGs behind.
    await rm(frameDir, { recursive: true, force: true });
    await mkdir(frameDir, { recursive: true });
    const times = frameTimes(durationMs, fps);
    for (let k = 0; k < times.length; k += 1) {
      await evaluate(cdp, `window.__tefSteps.seek(${i}, ${times[k]})`);
      await evaluate(
        cdp,
        'new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)))',
        { awaitPromise: true }
      );
      const shot = await cdp.command('Page.captureScreenshot', {
        format: 'png', fromSurface: true, captureBeyondViewport: false
      });
      await writeFile(path.join(frameDir, `${String(k).padStart(5, '0')}.png`), Buffer.from(shot.data, 'base64'));
    }
    steps.push({
      index: i,
      durationMs,
      frameCount: times.length,
      frameDir: path.relative(outDir, frameDir).split(path.sep).join('/')
    });
    console.log(`step ${i + 1}/${meta.total}: ${times.length} frames`);
  }

  const manifest = { html: htmlPath, captureUrl: url.href, width, height, fps, steps };
  await writeFile(path.join(outDir, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  console.log(`wrote ${path.join(outDir, 'manifest.json')}`);
} finally {
  cdp?.close();
  chrome?.proc.kill('SIGTERM');
  await new Promise((resolve) => setTimeout(resolve, 300));
  chrome?.proc.kill('SIGKILL');
  await rm(userDataDir, { recursive: true, force: true });
}
