#!/usr/bin/env bash
set -euo pipefail

SKILL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail() { echo "FAIL: $*" >&2; exit 1; }

if ! command -v google-chrome >/dev/null 2>&1 \
  && ! command -v chromium >/dev/null 2>&1 \
  && ! command -v chromium-browser >/dev/null 2>&1 \
  && [ -z "${CHROME_BIN:-}" ]; then
  fail "Chrome not found (install Chrome/Chromium or set CHROME_BIN)"
fi
command -v node >/dev/null 2>&1 || fail "node not found"
command -v python3 >/dev/null 2>&1 || fail "python3 not found"
if ! command -v ffmpeg >/dev/null 2>&1 && [ -z "${FFMPEG_BIN:-}" ]; then
  fail "ffmpeg not found (install ffmpeg or set FFMPEG_BIN)"
fi
if ! command -v ffprobe >/dev/null 2>&1 && [ -z "${FFPROBE_BIN:-}" ]; then
  fail "ffprobe not found (install ffmpeg or set FFPROBE_BIN)"
fi

FONT="${REQUIRED_FONT:-Microsoft YaHei}"
MATCH="$(fc-match "$FONT")"
echo "font: $MATCH"
if ! printf '%s' "$MATCH" | grep -qiF "$FONT" \
  && ! printf '%s' "$MATCH" | grep -qiE "msyh|deng|noto sans cjk|source han"; then
  fail "font preflight failed: $FONT -> $MATCH (install the font and run fc-cache -f)"
fi

PYTHONPATH="$SKILL_DIR/vendor" python3 -c 'import pptx; print("python-pptx", pptx.__version__)' \
  || fail "python-pptx missing; run scripts/setup.sh"
python3 -c 'from PIL import Image; print("Pillow", Image.__version__)' \
  || fail "Pillow missing"

echo "preflight OK"
