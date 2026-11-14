#!/usr/bin/env bash
set -euo pipefail

SKILL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 -m pip install --target "$SKILL_DIR/vendor" --no-deps python-pptx
PYTHONPATH="$SKILL_DIR/vendor" python3 -c 'import pptx; print("python-pptx", pptx.__version__)'

echo "python-pptx installed under $SKILL_DIR/vendor"
echo "Run Python tools with: PYTHONPATH=\"$SKILL_DIR/vendor\" python3 <script>"
