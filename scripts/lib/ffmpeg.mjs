import { existsSync } from 'node:fs';
import path from 'node:path';

export function repoRoot(start = process.cwd()) {
  let dir = path.resolve(start);
  while (dir !== path.dirname(dir)) {
    if (existsSync(path.join(dir, 'package.json')) && existsSync(path.join(dir, 'docs'))) return dir;
    dir = path.dirname(dir);
  }
  throw new Error('repo root not found');
}

export function resolveFfmpeg() {
  if (process.env.FFMPEG_BIN) return process.env.FFMPEG_BIN;
  try {
    const local = path.join(
      repoRoot(),
      'docs/competition/animation/node_modules/@remotion/compositor-linux-x64-gnu/ffmpeg'
    );
    if (existsSync(local)) return local;
  } catch {
    // Generic project: fall back to PATH.
  }
  return 'ffmpeg';
}

export function ffmpegEnv(ffmpegPath) {
  const directory = path.dirname(ffmpegPath);
  if (directory === '.') return { ...process.env };
  return {
    ...process.env,
    LD_LIBRARY_PATH: `${directory}:${process.env.LD_LIBRARY_PATH ?? ''}`
  };
}
