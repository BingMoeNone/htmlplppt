#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import zipfile
from pathlib import Path

from lxml import etree
from pptx import Presentation

NS = 'http://schemas.openxmlformats.org/presentationml/2006/main'


def p(tag: str) -> str:
    return f'{{{NS}}}{tag}'


def c14n_hash(archive: zipfile.ZipFile, name: str) -> str | None:
    if name not in archive.namelist():
        return None
    root = etree.fromstring(archive.read(name))
    return hashlib.sha256(etree.tostring(root, method='c14n')).hexdigest()


def _direct_condition_delay(element, *path: str) -> str | None:
    """Return @delay of the direct-child p:cond at the end of a direct-child tag path."""
    cursor = element
    for tag in path[:-1]:
        cursor = cursor.find(p(tag))
        if cursor is None:
            return None
    condition = cursor.find(p(path[-1]))
    if condition is None:
        return None
    return condition.get('delay')


def check_native_timing(slide_element, expected_videos: int, first_auto: bool,
                        failures: list[str]) -> None:
    timing = slide_element.find(p('timing'))
    delays: list[str | None] = []
    if timing is not None:
        for video in timing.iter(p('video')):
            delays.append(
                _direct_condition_delay(video, 'cMediaNode', 'cTn', 'stCondLst', 'cond')
            )
    if len(delays) != expected_videos:
        failures.append(f'native mode: media nodes={len(delays)}, expected {expected_videos}')
        return
    if first_auto:
        expected = ['0'] + ['indefinite'] * (expected_videos - 1)
    else:
        expected = ['indefinite'] * expected_videos
    for index, (delay, want) in enumerate(zip(delays, expected)):
        if delay != want:
            failures.append(
                f'native mode: media node {index} delay={delay!r}, expected {want!r}'
            )


def count_media_nodes(slide_element) -> int:
    """Return the number of `p:video` media nodes in the slide timing."""
    timing = slide_element.find(p('timing'))
    if timing is None:
        return 0
    return len(list(timing.iter(p('video'))))


def find_main_seq(slide_element):
    timing = slide_element.find(p('timing'))
    if timing is None:
        return None
    for seq in timing.iter(p('seq')):
        ctn = seq.find(p('cTn'))
        if ctn is not None and ctn.get('nodeType') == 'mainSeq':
            return seq
    return None


def check_appear_timing(slide_element, expected_videos: int, first_auto: bool,
                        failures: list[str]) -> None:
    seq = find_main_seq(slide_element)
    if seq is None:
        failures.append('appear mode: mainSeq p:seq not found')
        return
    sequence_ctn = seq.find(p('cTn'))
    child_list = sequence_ctn.find(p('childTnLst')) if sequence_ctn is not None else None
    if child_list is None:
        failures.append('appear mode: mainSeq p:childTnLst not found')
        return
    groups = [child for child in child_list if child.tag == p('par')]
    if len(groups) != expected_videos:
        failures.append(f'appear mode: click groups={len(groups)}, expected {expected_videos}')
        return
    for index, group in enumerate(groups):
        ctn = group.find(p('cTn'))
        conditions = ctn.find(p('stCondLst')) if ctn is not None else None
        condition = conditions.find(p('cond')) if conditions is not None else None
        if ctn is None or condition is None:
            failures.append(f'appear mode: click group {index} missing cTn/stCondLst/cond')
            continue
        auto = first_auto and index == 0
        want_node = 'afterEffect' if auto else 'clickEffect'
        want_delay = '0' if auto else 'indefinite'
        if ctn.get('nodeType') != want_node:
            failures.append(
                f'appear mode: click group {index} nodeType={ctn.get("nodeType")!r}, '
                f'expected {want_node!r}'
            )
        if condition.get('delay') != want_delay:
            failures.append(
                f'appear mode: click group {index} delay={condition.get("delay")!r}, '
                f'expected {want_delay!r}'
            )


def main() -> int:
    parser = argparse.ArgumentParser(description='Verify an assembled animated-slide deck.')
    parser.add_argument('--source', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--new-index', type=int, required=True, help='1-based index of the animated slide')
    parser.add_argument('--expected-slides', type=int, required=True)
    parser.add_argument('--expected-videos', type=int, required=True)
    parser.add_argument('--mode', choices=['native', 'appear'], default='native')
    parser.add_argument('--no-first-auto', action='store_true',
                        help='expect no auto-starting first clip: every media node / click group '
                             'waits for a click')
    args = parser.parse_args()

    source = Path(args.source).resolve()
    output = Path(args.out).resolve()
    failures: list[str] = []

    with zipfile.ZipFile(source) as source_zip, zipfile.ZipFile(output) as output_zip:
        source_slides = sorted(
            name for name in source_zip.namelist()
            if name.startswith('ppt/slides/slide') and name.endswith('.xml')
        )
        identical = 0
        for name in source_slides:
            if c14n_hash(source_zip, name) == c14n_hash(output_zip, name):
                identical += 1
            else:
                failures.append(f'original slide changed: {name}')
        if identical != len(source_slides):
            failures.append(f'original slides identical: {identical}/{len(source_slides)}')

        presentation = Presentation(str(output))
        slide_count = len(presentation.slides)
        if slide_count != args.expected_slides:
            failures.append(f'slides={slide_count}, expected {args.expected_slides}')

        target = None
        if not 1 <= args.new_index <= slide_count:
            failures.append(f'--new-index {args.new_index} out of range 1..{slide_count}')
        else:
            target = presentation.slides[args.new_index - 1]

        if target is not None:
            slide_xml = etree.tostring(target._element, encoding='unicode')
            videos = slide_xml.count('videoFile')
            if videos != args.expected_videos:
                failures.append(f'videos on target slide={videos}, expected {args.expected_videos}')

            first_auto = not args.no_first_auto
            if args.mode == 'native':
                if find_main_seq(target._element) is not None:
                    failures.append(
                        'native mode: an injected mainSeq is present; native mode must '
                        'step through the media nodes only'
                    )
                check_native_timing(target._element, args.expected_videos, first_auto, failures)
            else:
                if slide_xml.count('playFrom(0.0)') != args.expected_videos:
                    failures.append('appear mode: playFrom count mismatch')
                if slide_xml.count('style.visibility') != args.expected_videos:
                    failures.append('appear mode: appear count mismatch')
                check_appear_timing(target._element, args.expected_videos, first_auto, failures)
                remaining = count_media_nodes(target._element)
                if remaining != 0:
                    failures.append(
                        f'appear mode: {remaining} p:video media nodes remain; each one is '
                        'an extra click stop on top of the injected click groups'
                    )

    if failures:
        print('FAIL:')
        for failure in failures:
            print('  -', failure)
        return 1
    print(f'OK: {args.expected_slides} slides, {args.expected_videos} videos, original slides preserved')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
