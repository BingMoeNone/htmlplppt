#!/usr/bin/env python3
"""Add a static 'live animation starts automatically' slide to a deck.

Use with watch-slides.vbs: the watcher opens the deck, starts the show, exits
the show at the target slide, launches Edge, and resumes at the target slide.
No button or hyperlink is needed for this mode.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Emu, Pt


def add_text(slide, prs, left_f, top_f, width_f, height_f, text, size, color, bold=False, font='Microsoft YaHei'):
    box = slide.shapes.add_textbox(
        Emu(int(prs.slide_width * left_f)),
        Emu(int(prs.slide_height * top_f)),
        Emu(int(prs.slide_width * width_f)),
        Emu(int(prs.slide_height * height_f)),
    )
    frame = box.text_frame
    frame.word_wrap = True
    frame.vertical_anchor = MSO_ANCHOR.MIDDLE
    paragraph = frame.paragraphs[0]
    paragraph.alignment = PP_ALIGN.CENTER
    run = paragraph.add_run()
    run.text = text
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.name = font
    run.font.color.rgb = color
    return box


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--after', type=int, required=True, help='1-based slide index after which to insert')
    parser.add_argument('--title', default='Live HTML Animation')
    parser.add_argument('--subtitle', default='The browser animation starts automatically on this slide.')
    parser.add_argument('--note', default='If it does not start, run watch-slides.vbs in this folder.')
    parser.add_argument('--eyebrow', default='HTML / LIVE DEMO')
    parser.add_argument('--font', default='Microsoft YaHei')
    args = parser.parse_args()

    source = Path(args.source).resolve()
    output = Path(args.out).resolve()
    if source == output:
        raise SystemExit('--out must not equal --source')

    prs = Presentation(str(source))
    template = prs.slides[args.after - 1]
    slide = prs.slides.add_slide(template.slide_layout)
    for shape in list(slide.shapes):
        shape._element.getparent().remove(shape._element)

    background = slide.background.fill
    background.solid()
    background.fore_color.rgb = RGBColor(0x03, 0x0b, 0x08)

    add_text(slide, prs, 0.08, 0.16, 0.84, 0.08, args.eyebrow, 14, RGBColor(0x78, 0xcb, 0xb4), font=args.font)
    add_text(slide, prs, 0.08, 0.27, 0.84, 0.13, args.title, 40, RGBColor(0xf0, 0xec, 0xdf), True, args.font)
    add_text(slide, prs, 0.08, 0.44, 0.84, 0.10, args.subtitle, 20, RGBColor(0x9d, 0xb3, 0xa7), font=args.font)
    add_text(slide, prs, 0.08, 0.62, 0.84, 0.10, args.note, 15, RGBColor(0x6f, 0x89, 0x7c), font=args.font)

    slide_ids = prs.slides._sldIdLst
    new_id = slide_ids[-1]
    slide_ids.remove(new_id)
    slide_ids.insert(args.after, new_id)

    output.parent.mkdir(parents=True, exist_ok=True)
    prs.save(str(output))
    print(f'wrote {output} ({len(prs.slides)} slides)')


if __name__ == '__main__':
    main()
