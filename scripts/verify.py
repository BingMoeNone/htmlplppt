#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops


def resolve_ffprobe() -> str:
    return os.environ.get('FFPROBE_BIN') or shutil.which('ffprobe') or 'ffprobe'


FFPROBE = resolve_ffprobe()


def probe(path: Path) -> tuple[dict, float]:
    try:
        result = subprocess.run([
            FFPROBE, '-v', 'error', '-select_streams', 'v:0',
            '-show_entries', 'stream=codec_name,width,height,pix_fmt,duration:format=duration',
            '-of', 'json', str(path)
        ], capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit(f'ffprobe not found: {FFPROBE}; set FFPROBE_BIN or install ffmpeg')
    if result.returncode != 0:
        raise SystemExit(f'ffprobe failed for {path}: {result.stderr.strip()}')
    data = json.loads(result.stdout)
    stream = data['streams'][0]
    duration = float(stream.get('duration') or data.get('format', {}).get('duration', 0))
    return stream, duration


def assert_same_pixels(a: Path, b: Path) -> None:
    if not a.exists():
        raise AssertionError(f'missing frame: {a}')
    if not b.exists():
        raise AssertionError(f'missing frame: {b}')
    image_a = Image.open(a).convert('RGB')
    image_b = Image.open(b).convert('RGB')
    if image_a.size != image_b.size:
        raise AssertionError(f'size mismatch: {a} {image_a.size} vs {b} {image_b.size}')
    bbox = ImageChops.difference(image_a, image_b).getbbox()
    if bbox is not None:
        raise AssertionError(f'boundary frames differ: {a.name} vs {b.name} bbox={bbox}')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--capture', required=True)
    parser.add_argument('--clips', required=True)
    args = parser.parse_args()

    capture_path = Path(args.capture).resolve()
    clips_path = Path(args.clips).resolve()
    capture = json.loads(capture_path.read_text())
    clips = json.loads(clips_path.read_text())
    capture_dir = capture_path.parent
    clips_dir = clips_path.parent

    failures: list[str] = []
    if len(capture['steps']) != len(clips['clips']):
        failures.append(f"step count mismatch: {len(capture['steps'])} vs {len(clips['clips'])}")

    for step in capture['steps']:
        frames_dir = capture_dir / step['frameDir']
        actual = len(list(frames_dir.glob('*.png')))
        if actual != step['frameCount']:
            failures.append(f"step {step['index']}: {actual} frames, expected {step['frameCount']}")

    for index in range(len(capture['steps']) - 1):
        current = capture['steps'][index]
        following = capture['steps'][index + 1]
        last = capture_dir / current['frameDir'] / f"{current['frameCount'] - 1:05d}.png"
        first = capture_dir / following['frameDir'] / '00000.png'
        try:
            assert_same_pixels(last, first)
        except AssertionError as error:
            failures.append(str(error))

    for clip in clips['clips']:
        path = clips_dir / clip['file']
        if not path.exists():
            failures.append(f'missing clip: {path}')
            continue
        stream, duration = probe(path)
        if stream.get('codec_name') != 'h264':
            failures.append(f"{path.name}: codec {stream.get('codec_name')}, expected h264")
        if stream.get('pix_fmt') != 'yuv420p':
            failures.append(f"{path.name}: pix_fmt {stream.get('pix_fmt')}, expected yuv420p")
        if int(stream.get('width', 0)) != int(capture['width']) or int(stream.get('height', 0)) != int(capture['height']):
            failures.append(f"{path.name}: {stream.get('width')}x{stream.get('height')}, expected {capture['width']}x{capture['height']}")
        expected = (clip['durationMs'] + 1000 / capture['fps']) / 1000
        if abs(duration - expected) > 1.5 / capture['fps']:
            failures.append(f"{path.name}: duration {duration:.3f}s, expected ~{expected:.3f}s")

    if failures:
        print('FAIL:')
        for failure in failures:
            print('  -', failure)
        return 1
    print(f"OK: {len(capture['steps'])} steps, boundary frames identical, {len(clips['clips'])} clips valid")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
