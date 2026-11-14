export function frameTimes(durationMs, fps) {
  if (!(durationMs > 0)) throw new Error('durationMs must be > 0');
  if (!(fps > 0)) throw new Error('fps must be > 0');
  const intervals = Math.max(1, Math.round((durationMs / 1000) * fps));
  const times = [];
  for (let k = 0; k <= intervals; k += 1) {
    times.push(Math.min(durationMs, (k * 1000) / fps));
  }
  return times;
}
