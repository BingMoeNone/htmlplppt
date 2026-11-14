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
    max: { type: 'string', default: '0.9' },
    scale: { type: 'string' },
    crf: { type: 'string', default: '20' }
  }
});

if (!values.manifest || !values.out) {
  console.error('usage: node scripts/compress-clips.mjs --manifest <clips-manifest.json> --out <dir> [--max 0.9 | --scale 0.25] [--crf 20]');
  process.exit(2);
}

const manifestPath = path.resolve(values.manifest);
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
const inDir = path.dirname(manifestPath);
const outDir = path.resolve(values.out);
await mkdir(outDir, { recursive: true });
const ffmpeg = resolveFfmpeg();
const env = ffmpegEnv(ffmpeg);
const maxSeconds = Number(values.max);
const uniformScale = values.scale ? Number(values.scale) : null;
const clips = [];

for (const clip of manifest.clips) {
  const input = path.join(inDir, clip.file);
  const durationSec = clip.durationMs / 1000;
  const scale = uniformScale ?? Math.min(1, maxSeconds / durationSec);
  const name = `${path.basename(clip.file, '.mp4')}-x${scale.toFixed(3)}.mp4`;
  const output = path.join(outDir, name);

  if (scale >= 0.999) {
    await copyFile(input, output);
  } else {
    await execFileAsync(ffmpeg, [
      '-y',
      '-loglevel', 'error',
      '-itsscale', String(scale),
      '-i', input,
      '-c:v', 'libx264',
      '-preset', 'veryfast',
      '-crf', values.crf,
      '-pix_fmt', 'yuv420p',
      '-an',
      '-movflags', '+faststart',
      output
    ], { env, maxBuffer: 20 * 1024 * 1024 });
  }

  const posterName = path.basename(clip.poster);
  await copyFile(path.join(inDir, clip.poster), path.join(outDir, posterName));
  clips.push({ ...clip, file: name, poster: posterName, durationMs: Math.round(clip.durationMs * scale) });
  console.log(`${clip.file} -> ${name} (scale ${scale.toFixed(3)})`);
}

const outManifest = { ...manifest, clips };
await writeFile(path.join(outDir, 'manifest.json'), `${JSON.stringify(outManifest, null, 2)}\n`);
console.log(`wrote ${path.join(outDir, 'manifest.json')}`);
