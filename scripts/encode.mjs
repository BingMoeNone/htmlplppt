#!/usr/bin/env node
import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { parseArgs } from 'node:util';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { ffmpegEnv, resolveFfmpeg } from './lib/ffmpeg.mjs';

const execFileAsync = promisify(execFile);
const { values } = parseArgs({
  options: {
    manifest: { type: 'string' },
    out: { type: 'string' },
    crf: { type: 'string', default: '14' },
    preset: { type: 'string', default: 'slow' }
  }
});

if (!values.manifest || !values.out) {
  console.error('usage: node scripts/encode.mjs --manifest <capture-manifest.json> --out <clips-dir> [--crf 14]');
  process.exit(2);
}

const manifestPath = path.resolve(values.manifest);
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
const outDir = path.resolve(values.out);
await mkdir(outDir, { recursive: true });
const ffmpeg = resolveFfmpeg();
const env = ffmpegEnv(ffmpeg);
const clips = [];

for (const step of manifest.steps) {
  const framesDir = path.resolve(path.dirname(manifestPath), step.frameDir);
  const name = `step-${String(step.index + 1).padStart(2, '0')}`;
  const outFile = path.join(outDir, `${name}.mp4`);
  const poster = path.join(outDir, `${name}.png`);
  await execFileAsync(ffmpeg, [
    '-y',
    '-framerate', String(manifest.fps),
    '-i', path.join(framesDir, '%05d.png'),
    '-c:v', 'libx264',
    '-preset', values.preset,
    '-crf', values.crf,
    '-pix_fmt', 'yuv420p',
    '-movflags', '+faststart',
    '-an',
    outFile
  ], { env, maxBuffer: 20 * 1024 * 1024 });
  await copyFile(path.join(framesDir, '00000.png'), poster);
  clips.push({ index: step.index, file: `${name}.mp4`, poster: `${name}.png`, durationMs: step.durationMs });
  console.log(`encoded ${name}.mp4`);
}

const outManifest = { fps: manifest.fps, width: manifest.width, height: manifest.height, clips };
await writeFile(path.join(outDir, 'manifest.json'), `${JSON.stringify(outManifest, null, 2)}\n`);
console.log(`wrote ${path.join(outDir, 'manifest.json')}`);
