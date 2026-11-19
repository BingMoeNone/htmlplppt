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
# The Windows App Execution Alias for python3 is often a Store stub that exits
# 9009, so accept PYTHON, then the first candidate that actually runs.
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 \
      && "$candidate" -c 'import sys' >/dev/null 2>&1; then
      PY="$candidate"
      break
    fi
  done
fi
[ -n "$PY" ] || fail "no working Python found (install Python 3.12+ or set PYTHON)"
"$PY" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 12) else 1)' \
  || fail "Python 3.12+ required, found: $("$PY" --version 2>&1)"
if ! command -v ffmpeg >/dev/null 2>&1 && [ -z "${FFMPEG_BIN:-}" ]; then
  fail "ffmpeg not found (install ffmpeg or set FFMPEG_BIN)"
fi
if ! command -v ffprobe >/dev/null 2>&1 && [ -z "${FFPROBE_BIN:-}" ]; then
  fail "ffprobe not found (install ffmpeg or set FFPROBE_BIN)"
fi

FONT="${REQUIRED_FONT:-Microsoft YaHei}"
if command -v fc-match >/dev/null 2>&1; then
  MATCH="$(fc-match "$FONT")"
  echo "font: $MATCH"
  if ! printf '%s' "$MATCH" | grep -qiF "$FONT" \
    && ! printf '%s' "$MATCH" | grep -qiE "msyh|deng|noto sans cjk|source han"; then
    fail "font preflight failed: $FONT -> $MATCH (install the font and run fc-cache -f)"
  fi
elif [ -d /c/Windows/Fonts ]; then
  # Windows has no fontconfig; check the installed font files directly. `grep`
  # exits 1 when nothing matches and `set -e` would abort the script before the
  # friendly failure below could run, so a missing match is an empty one here.
  font_file() {
    ls /c/Windows/Fonts 2>/dev/null | grep -iE "$1" | head -n 1 || true
  }
  case "$(printf '%s' "$FONT" | tr '[:upper:]' '[:lower:]')" in
    *yahei*)    MATCH="$(font_file '^msyh')" ;;
    *dengxian*) MATCH="$(font_file '^deng')" ;;
    *simhei*)   MATCH="$(font_file '^simhei')" ;;
    *simsun*)   MATCH="$(font_file '^simsun')" ;;
    # Fail closed on an unknown family: accepting any CJK font here would pass
    # preflight while scripts/lib/fonts.mjs rejects the same family later.
    *) fail "font preflight failed: unknown REQUIRED_FONT '$FONT' (expected Microsoft YaHei, DengXian, SimHei or SimSun)" ;;
  esac
  if [ -z "$MATCH" ]; then
    fail "font preflight failed: no CJK font in C:\\Windows\\Fonts (install $FONT or set REQUIRED_FONT)"
  fi
  echo "font: $FONT (/c/Windows/Fonts/$MATCH)"
else
  fail "font preflight failed: fc-match unavailable (install fontconfig or the font)"
fi

# Native Windows Python cannot read an MSYS path such as /d/Project/..., so
# convert the vendor directory when running under Git Bash / MSYS.
VENDOR="$SKILL_DIR/vendor"
if command -v cygpath >/dev/null 2>&1; then
  VENDOR="$(cygpath -w "$VENDOR")"
fi
PYTHONPATH="$VENDOR" "$PY" -c 'import pptx; print("python-pptx", pptx.__version__)' \
  || fail "python-pptx missing; run scripts/setup.sh"
"$PY" -c 'from PIL import Image; print("Pillow", Image.__version__)' \
  || fail "Pillow missing"

echo "preflight OK"
