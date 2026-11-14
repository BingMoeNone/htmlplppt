#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from lxml import etree
from PIL import Image
from pptx import Presentation
from pptx.util import Emu
from pptx.oxml.ns import qn


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_transparent_poster(path: Path) -> None:
    Image.new('RGBA', (1920, 1080), (0, 0, 0, 0)).save(path)


def set_first_media_auto(slide, shape_id: int) -> None:
    timing = slide._element.find(qn('p:timing'))
    if timing is None:
        raise RuntimeError('python-pptx did not create p:timing for the movie')
    for video in timing.iter(qn('p:video')):
        target = video.find('.//' + qn('p:spTgt'))
        if target is not None and int(target.get('spid')) == shape_id:
            condition = video.find('.//' + qn('p:cond'))
            condition.set('delay', '0')
            return
    raise RuntimeError(f'media node for shape {shape_id} not found')


def _sub(parent, tag, **attrs):
    element = etree.SubElement(parent, qn(tag))
    for key, value in attrs.items():
        element.set(key, str(value))
    return element


def build_main_seq(shape_ids, first_auto: bool):
    ids = iter(range(100, 100000))
    seq = etree.Element(qn('p:seq'))
    seq.set('concurrent', '1')
    seq.set('nextAc', 'seek')
    ctn = _sub(seq, 'p:cTn', id=next(ids), dur='indefinite', nodeType='mainSeq')
    child = _sub(ctn, 'p:childTnLst')

    for index, spid in enumerate(shape_ids):
        auto = first_auto and index == 0
        click = _sub(child, 'p:par')
        click_ctn = _sub(click, 'p:cTn', id=next(ids), fill='hold', nodeType='afterEffect' if auto else 'clickEffect')
        start = _sub(click_ctn, 'p:stCondLst')
        _sub(start, 'p:cond', delay='0' if auto else 'indefinite')
        click_child = _sub(click_ctn, 'p:childTnLst')
        group = _sub(click_child, 'p:par')
        group_ctn = _sub(group, 'p:cTn', id=next(ids), fill='hold')
        group_start = _sub(group_ctn, 'p:stCondLst')
        _sub(group_start, 'p:cond', delay='0')
        group_child = _sub(group_ctn, 'p:childTnLst')

        appear = _sub(group_child, 'p:par')
        appear_ctn = _sub(appear, 'p:cTn', id=next(ids), presetID='1', presetClass='entr', presetSubtype='0', fill='hold', nodeType='withEffect')
        appear_start = _sub(appear_ctn, 'p:stCondLst')
        _sub(appear_start, 'p:cond', delay='0')
        appear_child = _sub(appear_ctn, 'p:childTnLst')
        set_element = _sub(appear_child, 'p:set')
        behavior = _sub(set_element, 'p:cBhvr')
        set_ctn = _sub(behavior, 'p:cTn', id=next(ids), dur='1', fill='hold')
        set_start = _sub(set_ctn, 'p:stCondLst')
        _sub(set_start, 'p:cond', delay='0')
        target = _sub(behavior, 'p:tgtEl')
        _sub(target, 'p:spTgt', spid=spid)
        names = _sub(behavior, 'p:attrNameLst')
        attr_name = _sub(names, 'p:attrName')
        attr_name.text = 'style.visibility'
        to = _sub(set_element, 'p:to')
        _sub(to, 'p:strVal', val='visible')

        play = _sub(group_child, 'p:par')
        play_ctn = _sub(play, 'p:cTn', id=next(ids), presetID='1', presetClass='mediacall', fill='hold', nodeType='withEffect')
        play_start = _sub(play_ctn, 'p:stCondLst')
        _sub(play_start, 'p:cond', delay='0')
        play_child = _sub(play_ctn, 'p:childTnLst')
        command = _sub(play_child, 'p:cmd', type='call', cmd='playFrom(0.0)')
        command_behavior = _sub(command, 'p:cBhvr')
        _sub(command_behavior, 'p:cTn', id=next(ids), dur='1', fill='hold')
        command_target = _sub(command_behavior, 'p:tgtEl')
        _sub(command_target, 'p:spTgt', spid=spid)

    previous = _sub(seq, 'p:prevCondLst')
    previous_condition = _sub(previous, 'p:cond', evt='onPrev', delay='0')
    previous_target = _sub(previous_condition, 'p:tgtEl')
    _sub(previous_target, 'p:sldTgt')
    following = _sub(seq, 'p:nextCondLst')
    following_condition = _sub(following, 'p:cond', evt='onNext', delay='0')
    following_target = _sub(following_condition, 'p:tgtEl')
    _sub(following_target, 'p:sldTgt')
    return seq


def inject_appear_timing(slide, shape_ids, first_auto: bool) -> None:
    timing = slide._element.find(qn('p:timing'))
    if timing is None:
        raise RuntimeError('p:timing missing')
    root_ctn = timing.find('.//' + qn('p:cTn'))
    root_child = root_ctn.find(qn('p:childTnLst'))
    if root_child is None:
        raise RuntimeError('p:childTnLst missing')
    root_child.append(build_main_seq(shape_ids, first_auto))
    build_list = etree.SubElement(timing, qn('p:bldLst'))
    for spid in shape_ids:
        etree.SubElement(build_list, qn('p:bldP'), spid=str(spid), grpId='0', animBg='1')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True)
    parser.add_argument('--clips', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--after', type=int, required=True, help='1-based slide index after which to insert')
    parser.add_argument('--mode', choices=['native', 'appear'], default='native')
    parser.add_argument('--no-first-auto', action='store_true')
    args = parser.parse_args()

    source = Path(args.source).resolve()
    output = Path(args.out).resolve()
    if output == source:
        raise SystemExit('--out must not be the same file as --source (refusing to overwrite the source deck)')
    clips_manifest_path = Path(args.clips).resolve()
    clips_dir = clips_manifest_path.parent
    if not clips_manifest_path.exists():
        raise SystemExit(f'clips manifest not found: {clips_manifest_path}')
    try:
        manifest = json.loads(clips_manifest_path.read_text())
    except json.JSONDecodeError as error:
        raise SystemExit(f'invalid clips manifest JSON {clips_manifest_path}: {error}')
    if not isinstance(manifest, dict) or 'clips' not in manifest:
        raise SystemExit(f'clips manifest missing "clips": {clips_manifest_path}')
    clips = manifest['clips']
    if not isinstance(clips, list):
        raise SystemExit(f'clips manifest "clips" must be a list: {clips_manifest_path}')
    if len(clips) < 2:
        raise SystemExit('need at least 2 clips')

    if 'width' not in manifest or 'height' not in manifest:
        raise SystemExit(f'clips manifest missing width/height: {clips_manifest_path}')

    source_hash = sha256(source)
    presentation = Presentation(str(source))
    if not 1 <= args.after <= len(presentation.slides):
        raise SystemExit('--after out of range')

    # Use the deck's real page size; a 4:3 deck must not receive a 16:9
    # full-bleed video, and a mismatched capture would be stretched.
    slide_width = presentation.slide_width
    slide_height = presentation.slide_height
    slide_ratio = slide_width / slide_height
    clip_ratio = manifest['width'] / manifest['height']
    if abs(slide_ratio - clip_ratio) > 0.01:
        raise SystemExit(
            f'clip aspect ratio {clip_ratio:.3f} does not match slide {slide_ratio:.3f}; '
            're-capture with width/height matching the deck'
        )

    template = presentation.slides[args.after - 1]
    slide = presentation.slides.add_slide(template.slide_layout)
    for shape in list(slide.shapes):
        shape._element.getparent().remove(shape._element)

    transparent = clips_dir / '_transparent-poster.png'
    if args.mode == 'native':
        make_transparent_poster(transparent)

    shape_ids = []
    for index, clip in enumerate(clips):
        if not isinstance(clip, dict) or 'file' not in clip:
            raise SystemExit(f'clip {index} missing "file": {clips_manifest_path}')
        if 'poster' not in clip:
            raise SystemExit(f'clip {index} missing "poster": {clips_manifest_path}')
        video = clips_dir / clip['file']
        poster = clips_dir / clip['poster']
        if not video.exists():
            raise SystemExit(f'missing clip: {video}')
        if not poster.exists():
            raise SystemExit(f'missing poster: {poster}')
        use_poster = poster if index == 0 or args.mode == 'appear' else transparent
        picture = slide.shapes.add_movie(
            str(video), Emu(0), Emu(0), Emu(slide_width), Emu(slide_height),
            poster_frame_image=str(use_poster), mime_type='video/mp4'
        )
        shape_ids.append(picture.shape_id)

    first_auto = not args.no_first_auto
    if args.mode == 'native':
        if first_auto:
            set_first_media_auto(slide, shape_ids[0])
    else:
        inject_appear_timing(slide, shape_ids, first_auto)

    slide_id_list = presentation.slides._sldIdLst
    new_slide_id = slide_id_list[-1]
    slide_id_list.remove(new_slide_id)
    slide_id_list.insert(args.after, new_slide_id)

    output.parent.mkdir(parents=True, exist_ok=True)
    presentation.save(str(output))

    if sha256(source) != source_hash:
        raise SystemExit('source deck was modified; aborting')
    print(f'wrote {output} ({len(presentation.slides)} slides, {len(clips)} steps, mode={args.mode})')


if __name__ == '__main__':
    main()
