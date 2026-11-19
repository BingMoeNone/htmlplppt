#!/usr/bin/env python3
"""Build the demo deck and stage the live page for the mode-B2 walkthrough.

Run from anywhere:

    python demo/make_deck.py

It writes demo/deck.pptx (5 slides, 16:9) and copies templates/live-page.html
to demo/live-page.html. Both files are generated, so they are git-ignored; the
committed pieces are this script, watcher.json, and README.md.

The vendored python-pptx is used directly, so no PYTHONPATH is required.
"""
from __future__ import annotations

import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEMO = ROOT / "demo"
sys.path.insert(0, str(ROOT / "vendor"))

from pptx import Presentation  # noqa: E402  (needs sys.path first)
from pptx.util import Inches  # noqa: E402

TITLES = ["Cover", "Agenda", "Live demo", "Detail", "Thanks"]


def build_deck() -> None:
    presentation = Presentation()
    presentation.slide_width = Inches(13.333)
    presentation.slide_height = Inches(7.5)
    for title in TITLES:
        slide = presentation.slides.add_slide(presentation.slide_layouts[0])
        slide.shapes.title.text = title
    presentation.save(str(DEMO / "deck.pptx"))
    print(f"wrote {DEMO / 'deck.pptx'} ({len(presentation.slides)} slides)")


def stage_page() -> None:
    source = ROOT / "templates" / "live-page.html"
    target = DEMO / "live-page.html"
    shutil.copyfile(source, target)
    print(f"copied {source} -> {target}")


if __name__ == "__main__":
    DEMO.mkdir(exist_ok=True)
    build_deck()
    stage_page()
    print()
    print("Next:")
    print("  native\\build\\htmlppt-watchdog.exe --supervisor "
          "--config demo\\watcher.json --state-dir demo\\state --run-id demo")
