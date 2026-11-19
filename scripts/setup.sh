#!/usr/bin/env bash
set -euo pipefail

SKILL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-python3}"

# Pin the version the smoke test asserts. `--no-deps` keeps the vendor tree
# reproducible, so python-pptx's runtime dependencies (Pillow, lxml,
# XlsxWriter, typing-extensions) come from requirements.txt and must be
# installed in the interpreter, not under vendor/.
"$PY" -m pip install --target "$SKILL_DIR/vendor" --no-deps --upgrade "python-pptx==1.0.2"

# Instantiating Presentation() loads the bundled default.pptx template, so this
# also proves the vendored copy is complete rather than just importable.
VENDOR="$SKILL_DIR/vendor"
if command -v cygpath >/dev/null 2>&1; then
  VENDOR="$(cygpath -w "$VENDOR")"
fi
PYTHONPATH="$VENDOR" "$PY" -c '
import pptx
from pptx import Presentation
Presentation()
print("python-pptx", pptx.__version__, "(vendor complete)")
'

echo "python-pptx installed under $SKILL_DIR/vendor"
echo "Install runtime deps with: $PY -m pip install -r requirements.txt"
echo "Run Python tools with: PYTHONPATH=\"$SKILL_DIR/vendor\" $PY <script>"
