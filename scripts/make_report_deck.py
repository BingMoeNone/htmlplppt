"""Build the competition-report deck for htmlppt-watchdog.

All visuals are native python-pptx shapes and text, so the deck stays editable
and does not depend on external images. Run:

    python scripts/make_report_deck.py out/htmlppt-watchdog-技术汇报.pptx
"""

from __future__ import annotations

import sys

from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Inches, Pt

# ---------------------------------------------------------------- design tokens

BG = RGBColor(0x0A, 0x10, 0x15)
PANEL = RGBColor(0x12, 0x1B, 0x22)
PANEL2 = RGBColor(0x17, 0x24, 0x2E)
LINE = RGBColor(0x22, 0x30, 0x3A)
TEXT = RGBColor(0xEA, 0xF1, 0xF4)
DIM = RGBColor(0x93, 0xA6, 0xB3)
FAINT = RGBColor(0x5F, 0x72, 0x80)
ACCENT = RGBColor(0x38, 0xC6, 0xD9)
ACCENT_SOFT = RGBColor(0x1B, 0x3A, 0x42)
WARN = RGBColor(0xE9, 0xA2, 0x3B)
OK = RGBColor(0x58, 0xC0, 0x8A)
NEG = RGBColor(0xE0, 0x6C, 0x75)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)

FONT = "微软雅黑"
MONO = "Consolas"

W, H = 13.333, 7.5
MARGIN = 0.78
BODY_TOP = 1.72
BODY_BOTTOM = 6.62


# -------------------------------------------------------------------- primitives


def rgb(color: RGBColor) -> RGBColor:
    return color


def rect(slide, x, y, w, h, fill=None, line=None, radius=None, line_w=0.75):
    shape_type = MSO_SHAPE.ROUNDED_RECTANGLE if radius else MSO_SHAPE.RECTANGLE
    shape = slide.shapes.add_shape(
        shape_type, Inches(x), Inches(y), Inches(w), Inches(h)
    )
    if radius:
        shape.adjustments[0] = radius
    if fill is None:
        shape.fill.background()
    else:
        shape.fill.solid()
        shape.fill.fore_color.rgb = fill
    if line is None:
        shape.line.fill.background()
    else:
        shape.line.color.rgb = line
        shape.line.width = Pt(line_w)
    shape.shadow.inherit = False
    shape.text_frame.text = ""
    return shape


def text(
    slide,
    x,
    y,
    w,
    h,
    s,
    size=17,
    color=TEXT,
    bold=False,
    font=FONT,
    align=PP_ALIGN.LEFT,
    anchor=MSO_ANCHOR.TOP,
    spacing=1.22,
    margins=(0, 0, 0, 0),
):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.word_wrap = True
    tf.vertical_anchor = anchor
    tf.margin_left = Inches(margins[0])
    tf.margin_top = Inches(margins[1])
    tf.margin_right = Inches(margins[2])
    tf.margin_bottom = Inches(margins[3])
    p = tf.paragraphs[0]
    p.alignment = align
    p.line_spacing = spacing
    run = p.add_run()
    run.text = s
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.name = font
    run.font.color.rgb = color
    return box


def para(
    box,
    s,
    size=17,
    color=TEXT,
    bold=False,
    font=FONT,
    align=PP_ALIGN.LEFT,
    spacing=1.22,
    space_before=0,
    first=False,
):
    tf = box.text_frame
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = align
    p.line_spacing = spacing
    p.space_before = Pt(space_before)
    run = p.add_run()
    run.text = s
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.name = font
    run.font.color.rgb = color
    return p


def bullets(slide, x, y, w, h, items, size=17, gap=12, color=TEXT, marker_color=ACCENT):
    """Items: list of (lead, rest) or plain strings. Lead renders in accent."""
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.word_wrap = True
    tf.margin_left = 0
    tf.margin_right = 0
    tf.margin_top = 0
    tf.margin_bottom = 0
    for i, item in enumerate(items):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.line_spacing = 1.3
        p.space_before = Pt(0 if i == 0 else gap)
        if isinstance(item, tuple):
            lead, rest = item
        else:
            lead, rest = "", item
        if lead:
            r = p.add_run()
            r.text = lead + "  "
            r.font.size = Pt(size)
            r.font.bold = True
            r.font.name = FONT
            r.font.color.rgb = marker_color
        r2 = p.add_run()
        r2.text = rest
        r2.font.size = Pt(size)
        r2.font.name = FONT
        r2.font.color.rgb = color
    return box


def header(slide, kicker, title, title_size=27):
    rect(slide, MARGIN, 0.62, 0.055, 0.72, fill=ACCENT)
    text(slide, MARGIN + 0.22, 0.58, 11.5, 0.3, kicker, 10.5, ACCENT, bold=True, spacing=1.0)
    text(slide, MARGIN + 0.22, 0.88, 11.6, 0.62, title, title_size, TEXT, bold=True, spacing=1.12)


def footer(slide, page, total=15):
    rect(slide, MARGIN, 6.94, W - 2 * MARGIN, 0.012, fill=LINE)
    text(slide, MARGIN, 7.04, 7.0, 0.26, "htmlppt-watchdog  ·  HTML 动画 × PPT 结合技术研究", 9, FAINT)
    text(
        slide,
        W - MARGIN - 1.4,
        7.04,
        1.4,
        0.26,
        f"{page:02d} / {total}",
        9,
        FAINT,
        align=PP_ALIGN.RIGHT,
    )


def new_slide(prs):
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    rect(slide, 0, 0, W, H, fill=BG)
    return slide


def conclusion(slide, y, s, color=ACCENT, size=15):
    rect(slide, MARGIN, y, W - 2 * MARGIN, 0.66, fill=PANEL2, radius=0.16)
    rect(slide, MARGIN, y, 0.06, 0.66, fill=color)
    text(
        slide,
        MARGIN + 0.28,
        y,
        W - 2 * MARGIN - 0.5,
        0.66,
        s,
        size,
        TEXT,
        anchor=MSO_ANCHOR.MIDDLE,
        spacing=1.15,
    )


def chip(slide, x, y, w, h, s, size=12.5, fill=PANEL, color=TEXT, line=LINE, bold=False):
    rect(slide, x, y, w, h, fill=fill, line=line, radius=0.18)
    text(
        slide,
        x + 0.08,
        y,
        w - 0.16,
        h,
        s,
        size,
        color,
        bold=bold,
        align=PP_ALIGN.CENTER,
        anchor=MSO_ANCHOR.MIDDLE,
        spacing=1.05,
    )


def card(slide, x, y, w, h, title, body, accent=ACCENT, body_size=13, title_size=16):
    rect(slide, x, y, w, h, fill=PANEL, line=LINE, radius=0.05)
    rect(slide, x, y, w, 0.045, fill=accent)
    text(slide, x + 0.28, y + 0.3, w - 0.56, 0.4, title, title_size, TEXT, bold=True, spacing=1.1)
    text(slide, x + 0.28, y + 0.82, w - 0.56, h - 1.0, body, body_size, DIM, spacing=1.35)


def num_card(slide, x, y, w, h, value, unit, label, color=ACCENT):
    rect(slide, x, y, w, h, fill=PANEL, line=LINE, radius=0.08)
    box = slide.shapes.add_textbox(Inches(x + 0.2), Inches(y + 0.14), Inches(w - 0.26), Inches(0.5))
    tf = box.text_frame
    tf.margin_left = 0
    tf.margin_top = 0
    tf.margin_bottom = 0
    p = tf.paragraphs[0]
    p.line_spacing = 1.0
    r = p.add_run()
    r.text = value
    r.font.size = Pt(27 if len(value) <= 4 else 23)
    r.font.bold = True
    r.font.name = FONT
    r.font.color.rgb = color
    if unit:
        r2 = p.add_run()
        r2.text = " " + unit
        r2.font.size = Pt(12)
        r2.font.name = FONT
        r2.font.color.rgb = DIM
    text(slide, x + 0.2, y + 0.68, w - 0.26, 0.42, label, 9.5, DIM, spacing=1.2)


def arrow(slide, x, y, w, h=0.16, color=LINE):
    shape = slide.shapes.add_shape(
        MSO_SHAPE.RIGHT_ARROW, Inches(x), Inches(y), Inches(w), Inches(h)
    )
    shape.fill.solid()
    shape.fill.fore_color.rgb = color
    shape.line.fill.background()
    shape.shadow.inherit = False
    return shape


def logblock(slide, x, y, w, h, lines, size=11.5):
    rect(slide, x, y, w, h, fill=RGBColor(0x08, 0x0C, 0x10), line=LINE, radius=0.03)
    box = slide.shapes.add_textbox(
        Inches(x + 0.22), Inches(y + 0.16), Inches(w - 0.44), Inches(h - 0.28)
    )
    tf = box.text_frame
    tf.word_wrap = False
    tf.margin_left = 0
    tf.margin_top = 0
    tf.margin_bottom = 0
    for i, (line, color) in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.line_spacing = 1.32
        r = p.add_run()
        r.text = line
        r.font.size = Pt(size)
        r.font.name = MONO
        r.font.color.rgb = color


# ------------------------------------------------------------------------ slides


def slide_cover(prs):
    slide = new_slide(prs)
    rect(slide, 0, 0, 0.09, H, fill=ACCENT)

    text(slide, 0.95, 1.28, 7.4, 0.32, "比赛项目汇报  ·  2026.11", 11.5, ACCENT, bold=True)

    text(slide, 0.95, 1.78, 7.3, 0.9, "把 HTML 动画", 44, TEXT, bold=True, spacing=1.06)
    text(slide, 0.95, 2.62, 7.3, 0.9, "放进 PPT", 44, TEXT, bold=True, spacing=1.06)
    rect(slide, 0.98, 3.52, 1.1, 0.05, fill=ACCENT)
    text(
        slide,
        0.95,
        3.74,
        7.2,
        1.0,
        "一个不改 PPT 运行时的实时桥：\n按一次空格，WPS 与全屏动画互换，观众只看到一次黑屏。",
        17,
        DIM,
        spacing=1.5,
    )

    # right: the whole idea in three chips
    x0, cw, ch = 8.55, 3.85, 0.66
    chip(slide, x0, 1.62, cw, ch, "WPS 放映到目标页", 13.5, PANEL, TEXT)
    text(slide, x0, 2.33, cw, 0.3, "▼  按一次空格", 11, FAINT, align=PP_ALIGN.CENTER)
    chip(slide, x0, 2.66, cw, ch, "黑场 1.6s  ·  完成切换", 13.5, ACCENT_SOFT, ACCENT, line=ACCENT)
    text(slide, x0, 3.37, cw, 0.3, "▼  就绪后淡出", 11, FAINT, align=PP_ALIGN.CENTER)
    chip(slide, x0, 3.7, cw, ch, "Edge 全屏跑 HTML 动画", 13.5, PANEL, TEXT)
    text(slide, x0, 4.41, cw, 0.3, "▲  返回信号 → 恢复放映", 11, FAINT, align=PP_ALIGN.CENTER)
    chip(slide, x0, 4.74, cw, ch, "回到同一页，继续讲", 13.5, PANEL, TEXT)

    rect(slide, 0.95, 6.05, 7.3, 0.012, fill=LINE)
    text(
        slide,
        0.95,
        6.2,
        8.0,
        0.6,
        "htmlppt-watchdog  v0.1.0   ·   Windows + WPS 12.1 + Edge 实测",
        11,
        FAINT,
        spacing=1.4,
    )
    text(slide, 10.3, 6.2, 2.3, 0.6, "不装插件 · 不加页", 11, FAINT, align=PP_ALIGN.RIGHT, spacing=1.4)
    return slide


def slide_problem(prs):
    slide = new_slide(prs)
    header(slide, "01 / 现场问题", "PPT 放映态跑不了 HTML，但现场只给你一次机会")

    # left: two runtimes
    rect(slide, MARGIN, 1.86, 5.55, 3.42, fill=PANEL, line=LINE, radius=0.04)
    text(slide, MARGIN + 0.32, 2.08, 4.9, 0.32, "两种运行时，互不相认", 15, TEXT, bold=True)
    bullets(
        slide,
        MARGIN + 0.32,
        2.6,
        4.95,
        2.4,
        [
            ("HTML", "有动画、有交互、有实时状态 —— 只在浏览器里活着"),
            ("PPT 放映态", "只保证有限媒体能力，版本行为还不一致 —— 但它是现场唯一的入口"),
        ],
        size=14.5,
        gap=16,
    )

    # right: four constraints
    items = [
        ("01", "禁止宏、禁止联网插件"),
        ("02", "页数有上限，不能随便加页"),
        ("03", "客户机的 WPS 版本不可控"),
        ("04", "切换失败 = 全场冷场"),
    ]
    for i, (num, label) in enumerate(items):
        y = 1.86 + i * 0.86
        rect(slide, 6.72, y, 5.83, 0.74, fill=PANEL2, line=LINE, radius=0.06)
        text(slide, 6.95, y + 0.09, 0.5, 0.4, num, 13, ACCENT, bold=True)
        text(slide, 7.45, y + 0.12, 4.9, 0.5, label, 14, TEXT if i < 3 else WARN, bold=(i == 3))

    conclusion(
        slide,
        5.55,
        "所以真正的问题不是「怎么录屏」，而是「怎么把一段正在运行的动画，搬进一个不允许改动的运行时」。",
    )
    footer(slide, 2)
    return slide


def slide_routes(prs):
    slide = new_slide(prs)
    header(slide, "02 / 三条路线", "三条路都能到，代价完全不同")

    cards = [
        (
            "A  分步视频",
            "逐帧捕获成 N 段 MP4，叠在同一页，用 PPT 原生单击动画逐段播放。",
            "＋ 单文件、离线、页数只 +1",
            "− 没有交互；WPS 对单段时长敏感",
            ACCENT,
        ),
        (
            "B  实时浏览器",
            "退出 WPS 放映，Edge 全屏接管，关闭后回到 PPT。",
            "＋ 完整交互，页面不用改造成视频",
            "− 需要新增启动页；覆盖层必须先退放映",
            ACCENT,
        ),
        (
            "B2  空格触发 + 黑场",
            "在现有内容页按空格，黑场下切到 Edge，返回时恢复 WPS。",
            "＋ 0 页变化，完整交互",
            "− 切换必须藏在黑场里，返回比进入难",
            ACCENT,
        ),
    ]
    w = 3.86
    for i, (title, body, plus, minus, accent) in enumerate(cards):
        x = MARGIN + i * (w + 0.24)
        rect(slide, x, 1.86, w, 3.5, fill=PANEL, line=LINE, radius=0.04)
        text(slide, x + 0.28, 2.06, 1.0, 0.3, ["A", "B", "B2"][i], 11, accent, bold=True)
        text(slide, x + 0.28, 2.3, w - 0.5, 0.4, title, 16, TEXT, bold=True)
        text(slide, x + 0.28, 2.78, w - 0.56, 0.92, body, 12.5, DIM, spacing=1.42)
        rect(slide, x + 0.28, 3.8, w - 0.56, 0.012, fill=LINE)
        text(slide, x + 0.28, 3.9, w - 0.56, 0.34, plus, 11.5, OK, spacing=1.3)
        text(slide, x + 0.28, 4.32, w - 0.56, 0.72, minus, 11.5, WARN, spacing=1.3)

    conclusion(slide, 5.58, "我们的硬约束是「0 页变化」—— 这一条直接排除了 A 和 B。")
    footer(slide, 3)
    return slide


def slide_choice(prs):
    slide = new_slide(prs)
    header(slide, "03 / 取舍", "选 B2 不是因为它最强，是因为它唯一满足约束")

    rows = [
        ("页数变化", "+1 页", "+1 页", "0 页"),
        ("交互能力", "无（视频）", "完整", "完整"),
        ("HTML 改造", "分步契约", "关闭逻辑", "空格 / 返回信号"),
        ("现场依赖", "生成侧 Chrome + ffmpeg", "仅运行侧", "仅运行侧"),
        ("失败代价", "视频时长 / 编码", "焦点 / 覆盖层", "时机 / COM"),
    ]
    x0, y0 = MARGIN, 1.86
    widths = [2.3, 3.35, 3.35, 3.35]
    header_h = 0.52
    row_h = 0.56

    # header row
    heads = ["维度", "A  分步视频", "B  实时浏览器", "B2  黑场切换"]
    x = x0
    for i, head in enumerate(heads):
        fill = ACCENT_SOFT if i == 3 else PANEL2
        rect(slide, x, y0, widths[i] - 0.04, header_h, fill=fill, line=LINE)
        text(
            slide,
            x + 0.02,
            y0,
            widths[i] - 0.08,
            header_h,
            head,
            12,
            ACCENT if i == 3 else DIM,
            bold=True,
            align=PP_ALIGN.CENTER,
            anchor=MSO_ANCHOR.MIDDLE,
        )
        x += widths[i]

    for r, row in enumerate(rows):
        y = y0 + header_h + 0.04 + r * (row_h + 0.04)
        for c, cell in enumerate(row):
            x = x0 + sum(widths[:c])
            is_last = c == 3
            fill = RGBColor(0x10, 0x22, 0x28) if is_last else PANEL
            rect(slide, x, y, widths[c] - 0.04, row_h, fill=fill, line=LINE)
            color = TEXT if c == 0 else (ACCENT if is_last else DIM)
            text(
                slide,
                x + 0.02,
                y,
                widths[c] - 0.12,
                row_h,
                cell,
                11.5 if c else 12,
                color,
                bold=(c == 0 or is_last),
                align=PP_ALIGN.LEFT if c == 0 else PP_ALIGN.CENTER,
                anchor=MSO_ANCHOR.MIDDLE,
                margins=(0.2, 0, 0.1, 0),
            )

    text(
        slide,
        MARGIN,
        5.55,
        6.4,
        0.6,
        "代价被集中到一个点：观众必须接受一次约 1.6 秒的黑屏。",
        13,
        DIM,
        spacing=1.35,
    )
    conclusion(slide, 6.05, "那就把这 1.6 秒做成「看不见的切换」。")
    footer(slide, 4)
    return slide


def slide_timeline(prs):
    slide = new_slide(prs)
    header(slide, "04 / 一次真实运行", "观众只看到一次黑屏，其余都发生在黑场下面")

    nodes = [
        ("T0", "按空格", "WPS 翻到下一页"),
        ("≤20ms", "检测到离开", "轮询放映页码"),
        ("0.3s", "黑场渐入", "盖住整个屏幕"),
        ("全黑", "切换动作", "退放映 + 起 Edge"),
        ("就绪", "保持 ≥1.0s", "等 Edge 聚焦完成"),
        ("0.3s", "淡出", "HTML 已可交互"),
    ]
    line_y = 2.62
    rect(slide, 0.95, line_y, 11.4, 0.02, fill=LINE)
    slot = 11.4 / len(nodes)
    for i, (tag, name, note) in enumerate(nodes):
        cx = 0.95 + slot * (i + 0.5)
        color = ACCENT if i in (2, 3, 4) else LINE
        circle = slide.shapes.add_shape(
            MSO_SHAPE.OVAL, Inches(cx - 0.11), Inches(line_y - 0.1), Inches(0.22), Inches(0.22)
        )
        circle.fill.solid()
        circle.fill.fore_color.rgb = BG
        circle.line.color.rgb = color
        circle.line.width = Pt(2)
        circle.shadow.inherit = False
        text(slide, cx - 0.72, 2.02, 1.44, 0.26, tag, 10.5, ACCENT if i > 0 else DIM, align=PP_ALIGN.CENTER)
        text(slide, cx - 0.72, 3.0, 1.44, 0.3, name, 12.5, TEXT, bold=True, align=PP_ALIGN.CENTER)
        text(slide, cx - 0.72, 3.34, 1.44, 0.5, note, 10, DIM, align=PP_ALIGN.CENTER, spacing=1.25)

    logblock(
        slide,
        0.78,
        4.02,
        7.55,
        2.1,
        [
            ("fade in   21:52:17.213", DIM),
            ("hold      21:52:17.498", DIM),
            ("switch    21:52:17.616    全黑后 118ms", ACCENT),
            ("fade out  21:52:18.552    held=1039ms ready=True", DIM),
            ("done      21:52:18.862", DIM),
        ],
        size=11,
    )

    metrics = [("1.65s", "端到端"), ("285ms", "渐入"), ("1039ms", "保持"), ("310ms", "渐出")]
    for i, (value, label) in enumerate(metrics):
        x = 8.62 + (i % 2) * 2.02
        y = 4.02 + (i // 2) * 1.06
        rect(slide, x, y, 1.86, 0.94, fill=PANEL, line=LINE, radius=0.08)
        text(slide, x + 0.16, y + 0.1, 1.6, 0.4, value, 19, ACCENT, bold=True)
        text(slide, x + 0.16, y + 0.56, 1.6, 0.3, label, 10, DIM)

    text(
        slide,
        0.78,
        6.28,
        11.9,
        0.3,
        "黑屏期间实测全屏截图 mean=0 —— 观众看不到 WPS 退出，也看不到 Edge 启动。",
        11.5,
        DIM,
    )
    footer(slide, 5)
    return slide


def slide_signal(prs):
    slide = new_slide(prs)
    header(slide, "05 / 为什么行 ①", "把「按空格」换成「页码变了」")

    steps = [
        ("1", "每 20ms 读一次放映页", "WPS COM 的 CurrentShowPosition"),
        ("2", "页码离开目标页 = 触发", "目标页 3，看到 4 就说明用户按了空格"),
        ("3", "每个目标每场只触发一次", "done_slides 记账，返回后再按只翻页"),
    ]
    for i, (num, title, note) in enumerate(steps):
        y = 1.9 + i * 1.16
        rect(slide, MARGIN, y, 5.9, 1.0, fill=PANEL, line=LINE, radius=0.06)
        text(slide, MARGIN + 0.24, y + 0.2, 0.4, 0.4, num, 15, ACCENT, bold=True)
        text(slide, MARGIN + 0.7, y + 0.16, 4.9, 0.34, title, 14, TEXT, bold=True)
        text(slide, MARGIN + 0.7, y + 0.54, 4.9, 0.32, note, 11, DIM)

    rect(slide, 7.0, 1.9, 5.55, 3.32, fill=PANEL, line=LINE, radius=0.04)
    text(slide, 7.28, 2.1, 4.9, 0.34, "为什么不用键盘钩子", 15, TEXT, bold=True)
    bullets(
        slide,
        7.28,
        2.6,
        4.95,
        2.4,
        [
            "全局钩子要注入或驱动，权限与稳定性都不可控",
            "现场杀软、组策略经常直接拦截",
            "而「翻页」本来就是用户必须做的动作，天然可靠",
        ],
        size=13,
        gap=12,
    )

    conclusion(
        slide,
        5.55,
        "这条替换是整套自动化成立的前提：把不可观测的「按键」，换成可观测的「状态变化」。",
    )
    text(
        slide,
        MARGIN,
        6.32,
        11.9,
        0.3,
        "配置里两个触发模式：leave-forward（离开目标页） / arrive（到达即触发）。",
        10.5,
        FAINT,
    )
    footer(slide, 6)
    return slide


def slide_cover_timing(prs):
    slide = new_slide(prs)
    header(slide, "06 / 为什么行 ②", "黑场不是特效，是把「不可观测」盖住")

    boxes = [
        ("渐黑 0.3s", "让观众先失去画面"),
        ("全黑之后才动作", "ExitShow / 启动 Edge"),
        ("就绪 + 保持 ≥1.0s", "等 Edge 真正可交互"),
        ("淡出 0.3s", "露出 HTML 第一步"),
    ]
    bw, gap = 2.72, 0.32
    for i, (title, note) in enumerate(boxes):
        x = MARGIN + i * (bw + gap)
        rect(slide, x, 1.92, bw, 1.16, fill=PANEL, line=LINE, radius=0.06)
        text(slide, x + 0.2, 2.04, bw - 0.3, 0.32, title, 13, ACCENT if i in (1, 2) else TEXT, bold=True)
        text(slide, x + 0.2, 2.44, bw - 0.3, 0.5, note, 10.5, DIM, spacing=1.3)
        if i < 3:
            arrow(slide, x + bw + 0.06, 2.42, gap - 0.12, 0.14, LINE)

    rules = [
        ("顺序硬约束", "必须全黑之后才发切换动作；实测在 hold 之后 118ms 才执行。"),
        ("完成条件", "cover-ready（Edge 已就绪且被聚焦）才允许淡出，绝不能凭时间猜。"),
        ("上限保护", "maxHoldMs 到点按「切换失败」处理：收黑场、杀 Edge，再按策略自愈或进 SafeMode。"),
    ]
    for i, (title, note) in enumerate(rules):
        y = 3.4 + i * 0.78
        rect(slide, MARGIN, y, 11.77, 0.68, fill=PANEL2 if i % 2 == 0 else PANEL, line=LINE, radius=0.05)
        text(slide, MARGIN + 0.24, y + 0.17, 1.35, 0.34, title, 11.5, WARN if i == 0 else ACCENT, bold=True)
        text(slide, MARGIN + 1.66, y + 0.16, 9.9, 0.4, note, 12, TEXT, spacing=1.2)

    conclusion(
        slide,
        6.0,
        "观众视角：黑 → 动画。系统视角：退放映、起 Edge、等就绪、抢焦点 —— 全部藏在这个「画不出来」的窗口里。",
        size=13.5,
    )
    footer(slide, 7)
    return slide


def slide_return(prs):
    slide = new_slide(prs)
    header(slide, "07 / 为什么行 ③", "返回更难：结束发生在被遮住的那个进程里")

    # wrong path
    rect(slide, MARGIN, 1.9, 5.75, 1.62, fill=PANEL, line=RGBColor(0x4A, 0x2A, 0x2E), radius=0.05)
    text(slide, MARGIN + 0.26, 2.02, 5.2, 0.3, "✗  关窗之后才起黑场", 13, NEG, bold=True)
    text(
        slide,
        MARGIN + 0.26,
        2.42,
        5.25,
        1.0,
        "Edge 先关 → 桌面 / WPS 编辑器先露出 → 黑场才盖上来。\n现场看到的是「闪一下」。",
        11.5,
        DIM,
        spacing=1.38,
    )

    # right path
    rect(slide, 6.8, 1.9, 5.75, 1.62, fill=RGBColor(0x0F, 0x26, 0x20), line=RGBColor(0x2A, 0x4A, 0x3A), radius=0.05)
    text(slide, 7.06, 2.02, 5.2, 0.3, "✓  关窗之前先喊一声", 13, OK, bold=True)
    text(
        slide,
        7.06,
        2.42,
        5.25,
        1.0,
        "HTML 最后一步：title = TEF-LIVE-RETURN，页面内渐黑，延迟 2s 再 close。\n看门狗看到标题就先起黑场。",
        11.5,
        DIM,
        spacing=1.38,
    )

    steps = [
        ("1", "HTML 改标题", "TEF-LIVE-RETURN"),
        ("2", "看门狗起黑场", "标题轮询触发"),
        ("3", "恢复 WPS 放映", "Edge 仍开着"),
        ("4", "关窗后淡出", "露出目标页"),
    ]
    for i, (num, title, note) in enumerate(steps):
        x = MARGIN + i * 3.0
        rect(slide, x, 3.78, 2.72, 0.98, fill=PANEL2, line=LINE, radius=0.06)
        text(slide, x + 0.2, 3.88, 0.3, 0.3, num, 12, ACCENT, bold=True)
        text(slide, x + 0.5, 3.86, 2.1, 0.3, title, 11.5, TEXT, bold=True)
        text(slide, x + 0.2, 4.28, 2.4, 0.3, note, 10, DIM)

    logblock(
        slide,
        0.78,
        4.86,
        11.77,
        1.04,
        [
            ("return requested 28.890   fade in 29.038   hold 29.346   bring-wps 30.947", DIM),
            ("agent session end 30.999（Edge 关闭）   fade out 31.630   held=2433ms ready=True", ACCENT),
            ("黑场在 Edge 关闭前就已经起来，所以恢复过程不会露馅", FAINT),
        ],
        size=10.5,
    )
    conclusion(
        slide,
        6.14,
        "代价：返回侧约 2–3 秒黑屏。收益：全程不露出编辑器，目标页原样恢复。",
        size=13,
    )
    footer(slide, 8)
    return slide


def slide_focus(prs):
    slide = new_slide(prs)
    header(slide, "08 / 为什么行 ④", "抢不到焦点，后面全是白干")

    rect(slide, MARGIN, 1.9, 5.6, 3.36, fill=PANEL, line=LINE, radius=0.04)
    text(slide, MARGIN + 0.28, 2.1, 5.0, 0.32, "现象与原因", 15, TEXT, bold=True)
    bullets(
        slide,
        MARGIN + 0.28,
        2.6,
        5.05,
        2.5,
        [
            ("日志", "set pos=True fg=False —— 窗口已置顶，系统不给前台"),
            ("原因", "后台脚本（wscript / 隐藏 powershell）不满足前台激活条件"),
            ("后果", "键盘仍发往旧窗口，空格被吞，动画根本开始不了"),
        ],
        size=12.5,
        gap=13,
    )

    rect(slide, 6.66, 1.9, 5.9, 3.36, fill=PANEL, line=LINE, radius=0.04)
    text(slide, 6.94, 2.1, 5.3, 0.32, "手段（按顺序升级，直到校验通过）", 15, TEXT, bold=True)
    steps = [
        "SetForegroundWindow",
        "SwitchToThisWindow",
        "AttachThreadInput + BringWindowToTop + SetFocus",
        "模拟 Alt 按下/抬起，解锁前台锁",
        "校验 GetForegroundWindow 是否等于目标窗口",
    ]
    for i, s in enumerate(steps):
        y = 2.62 + i * 0.5
        text(slide, 6.94, y, 0.44, 0.3, str(i + 1), 11, ACCENT, bold=True)
        text(slide, 7.36, y, 4.95, 0.34, s, 11.5, TEXT if i < 3 else ACCENT, spacing=1.15)

    conclusion(
        slide,
        5.62,
        "自动化在现场失败，八成不是逻辑错，而是「Windows 不让你抢焦点」—— 所以要预检，且要自检到 PASS 才上场。",
        size=13,
    )
    logblock(
        slide,
        0.78,
        6.36,
        11.77,
        0.5,
        [("test-focus.ps1  →  orig=789718 test=2172088 force=True fg=2172088 match=True   PASS", OK)],
        size=10,
    )
    footer(slide, 9)
    return slide


def slide_state_machine(prs):
    slide = new_slide(prs)
    header(slide, "09 / 内部原理", "一个可测的状态机，而不是一堆脚本")

    row1 = ["Boot", "Warming", "Monitoring", "CoverIn", "EdgeStarting", "ShowExit"]
    row2 = ["Live", "Returning", "ReturnCoverOut", "Stopping", "Exited"]
    cw, ch, gap = 1.78, 0.6, 0.1

    def draw_row(states, y, highlight):
        x = MARGIN
        for s in states:
            fill = ACCENT_SOFT if s in highlight else PANEL2
            line = ACCENT if s in highlight else LINE
            rect(slide, x, y, cw, ch, fill=fill, line=line, radius=0.1)
            text(
                slide,
                x,
                y,
                cw,
                ch,
                s,
                11,
                ACCENT if s in highlight else TEXT,
                bold=s in highlight,
                align=PP_ALIGN.CENTER,
                anchor=MSO_ANCHOR.MIDDLE,
            )
            x += cw + gap

    draw_row(row1, 2.02, {"Monitoring"})
    draw_row(row2, 2.82, {"Live", "Returning"})

    # failure branch, kept visually separate so the happy path stays readable
    for i, name in enumerate(["Faulted", "SafeMode"]):
        x = 10.24 + i * 1.14
        rect(slide, x, 2.82, 1.06, 0.6, fill=PANEL, line=RGBColor(0x5A, 0x42, 0x24), radius=0.1)
        text(
            slide,
            x,
            2.82,
            1.06,
            0.6,
            name,
            9.5,
            WARN,
            align=PP_ALIGN.CENTER,
            anchor=MSO_ANCHOR.MIDDLE,
        )
    text(slide, 10.24, 3.5, 2.3, 0.24, "失败时进入旁支，不继续往下走", 9, WARN)

    invariants = [
        ("只有 Monitoring 响应翻页", "其它状态收到的事件一律忽略，避免误触发"),
        ("每个目标每场只触发一次", "done_slides 记账，返回后再按空格只翻页"),
        ("动作批次失败即中止", "ResumeShow 失败时，绝不能再淡出"),
    ]
    for i, (title, note) in enumerate(invariants):
        x = MARGIN + i * 3.95
        rect(slide, x, 4.02, 3.72, 1.42, fill=PANEL, line=LINE, radius=0.05)
        text(slide, x + 0.22, 4.16, 3.3, 0.34, title, 12, TEXT, bold=True)
        text(slide, x + 0.22, 4.6, 3.3, 0.7, note, 10.5, DIM, spacing=1.32)

    conclusion(
        slide,
        5.66,
        "它不碰 COM、不碰 Win32，只吃事件、只吐动作 —— 所以能在 Linux 上跑 284 个单元测试。",
        size=13,
    )
    text(
        slide,
        MARGIN,
        6.42,
        11.9,
        0.3,
        "13 个状态 · 15 种动作 · 16 类事件 · 6 个超时（覆盖保持 / Edge 就绪 20s / Edge 时长 600s / 返回恢复 …）",
        10.5,
        FAINT,
    )
    footer(slide, 10)
    return slide


def slide_supervisor(prs):
    slide = new_slide(prs)
    header(slide, "10 / 内部原理", "两层守护：worker 可以死，现场不能停")

    # supervisor box
    rect(slide, MARGIN, 1.92, 8.1, 1.72, fill=PANEL, line=ACCENT, radius=0.04)
    text(slide, MARGIN + 0.26, 2.02, 3.0, 0.3, "supervisor（守护）", 12, ACCENT, bold=True)
    sup_chips = ["Job Object", "命名管道 server", "共享黑盒（只读）", "重启预算 5 次 / 10 min"]
    for i, label in enumerate(sup_chips):
        cx = MARGIN + 0.26 + (i % 2) * 3.86
        cy = 2.34 + (i // 2) * 0.52
        chip(slide, cx, cy, 3.7, 0.44, label, 10.5, PANEL2, TEXT)
    text(
        slide,
        MARGIN + 0.26,
        3.34,
        7.5,
        0.3,
        "判定：心跳 2s 超时、循环停滞 12s、重启退避 200ms → 5s（带抖动）",
        10,
        DIM,
    )

    # worker box
    rect(slide, MARGIN, 3.86, 8.1, 1.72, fill=PANEL, line=LINE, radius=0.04)
    text(slide, MARGIN + 0.26, 3.96, 3.0, 0.3, "worker（执行）", 12, TEXT, bold=True)
    wk_chips = [("单线程 STA 持 COM", 2.1), ("黑场覆盖窗", 1.6), ("状态机循环", 1.6), ("心跳", 0.9)]
    cx = MARGIN + 0.26
    for label, w in wk_chips:
        chip(slide, cx, 4.4, w, 0.5, label, 10.5, PANEL2, TEXT)
        cx += w + 0.12
    text(
        slide,
        MARGIN + 0.26,
        5.0,
        7.5,
        0.4,
        "单线程是刻意的：COM、覆盖窗、状态机同属一个公寓，消息泵每片 ≤50ms",
        10.5,
        DIM,
    )

    # right: frame + resilience
    rect(slide, 9.2, 1.92, 3.35, 1.72, fill=PANEL2, line=LINE, radius=0.04)
    text(slide, 9.42, 2.02, 3.0, 0.3, "管道帧", 11.5, TEXT, bold=True)
    text(
        slide,
        9.42,
        2.4,
        2.95,
        1.1,
        "magic · version\nsequence · timestamp\ntype · length + CRC32\n上限 64 KiB",
        10,
        DIM,
        font=MONO,
        spacing=1.35,
    )

    rect(slide, 9.2, 3.86, 3.35, 1.72, fill=PANEL2, line=LINE, radius=0.04)
    text(slide, 9.42, 3.96, 3.0, 0.3, "黑盒日志", 11.5, TEXT, bold=True)
    text(
        slide,
        9.42,
        4.34,
        2.95,
        1.1,
        "96 字节定长记录\n共享内存环形缓冲\n崩溃后仍可读",
        10,
        DIM,
        spacing=1.35,
    )

    conclusion(
        slide,
        5.82,
        "worker 崩了 supervisor 重启它；supervisor 没了 worker 继续跑（Continue 策略）—— 两层都留了「继续演」的路。",
        size=12.5,
    )
    footer(slide, 11)
    return slide


def slide_com(prs):
    slide = new_slide(prs)
    header(slide, "11 / 内部原理", "WPS COM：能驱动，但会突然不理你")

    rect(slide, MARGIN, 1.9, 7.05, 2.36, fill=PANEL, line=LINE, radius=0.04)
    text(slide, MARGIN + 0.26, 2.02, 6.5, 0.3, "驱动链", 14, TEXT, bold=True)
    chain = ["打开 deck", "StartShow", "CurrentShowPosition", "ExitShow / ResumeShow", "GotoSlide"]
    cx = MARGIN + 0.26
    widths = [0.98, 0.92, 1.68, 1.62, 0.8]
    for label, w in zip(chain, widths):
        chip(slide, cx, 2.46, w, 0.5, label, 9.5, PANEL2, TEXT)
        cx += w + 0.08
    text(
        slide,
        MARGIN + 0.26,
        3.14,
        6.6,
        0.9,
        "WPS  ProgID：Kwpp.Application；放映页码只能靠轮询读取，\n每 20ms 一次（Monitoring 态），其它状态退回 100ms。",
        11,
        DIM,
        spacing=1.4,
    )

    rect(slide, 8.0, 1.9, 4.55, 2.36, fill=RGBColor(0x2A, 0x20, 0x12), line=RGBColor(0x5A, 0x42, 0x24), radius=0.04)
    text(slide, 8.24, 2.02, 4.1, 0.3, "实测：err=462 会突然出现", 12.5, WARN, bold=True)
    text(
        slide,
        8.24,
        2.42,
        4.05,
        1.6,
        "切换与恢复的瞬间最常见。\n旧版把它当「WPS 已关闭」直接退出 —— 这是闪退的主因。\n现在：2s 窗口内连续失败才判结构性故障。",
        10.5,
        DIM,
        spacing=1.4,
    )

    blocks = [
        ("为什么会抖", "WPS 在换放映态时要重建窗口与 COM 对象，期间调用会返回 busy。"),
        ("怎么处理", "瞬时失败重试（实测 20 次 ≈ 2s）；连续失败 → 清理并进 SafeMode。"),
        ("为什么不用事件", "探测过：WPS 只暴露一个连接点，翻页确实有事件；但 20ms 轮询已够，集成要加 STA 泵与 advise 生命周期，不值。"),
    ]
    for i, (title, note) in enumerate(blocks):
        y = 4.16 + i * 0.64
        rect(slide, MARGIN, y, 11.77, 0.56, fill=PANEL2 if i % 2 == 0 else PANEL, line=LINE, radius=0.05)
        text(slide, MARGIN + 0.24, y + 0.14, 1.6, 0.3, title, 10.5, ACCENT, bold=True)
        text(slide, MARGIN + 1.9, y + 0.12, 9.6, 0.36, note, 10, TEXT, spacing=1.15)

    conclusion(
        slide,
        6.16,
        "对 WPS 的策略不是「保证不出错」，而是「出错时能在 2 秒内判定并接管」。",
        size=12.5,
    )
    footer(slide, 12)
    return slide


def slide_metrics(prs):
    slide = new_slide(prs)
    header(slide, "12 / 实测", "这些数字都可以从日志里复算")

    metrics = [
        ("0.9s", "WPS 单段视频上限"),
        ("1.65s", "触发侧黑场端到端"),
        ("2.4s", "返回侧黑场"),
        ("20ms", "触发轮询间隔"),
        ("31ms", "步骤 p50（p99 140ms）"),
        ("345", "core 284 + win32 61 用例"),
    ]
    w, gap = 1.9, 0.12
    for i, (value, label) in enumerate(metrics):
        x = MARGIN + i * (w + gap)
        num_card(slide, x, 1.78, w, 1.12, value, "", label, ACCENT if i != 2 else WARN)

    rows = [
        ("E1  单段时长", "2×0.816s 正常；2×1.816s 空格被当翻页", "面向 WPS 每段压到 ≤0.9s"),
        ("E2  页数与保真", "9 页 → 9 页；原页 XML C14N 逐项一致", "只加外部看门狗，不改内容"),
        ("E5  全屏覆盖", "先 ExitShow 再起 Edge 有效；否则被盖住", "必须先退放映"),
        ("E6  前台锁", "pos=True 但 fg=False，输入仍去旧窗口", "AttachThreadInput + Alt 解锁"),
        ("E8  返回信号", "先起黑场再恢复：held=2433ms，未露编辑器", "返回侧必须有「关闭前信号」"),
        ("E10  COM 瞬时错误", "err=462 ×20 ≈2s 后自动清理", "重试 + 清理替代闪退"),
    ]
    x0, y0 = MARGIN, 3.0
    widths = [1.85, 6.15, 3.76]
    row_h = 0.47
    heads = ["实验", "观察到的现象", "得到的结论"]
    x = x0
    for i, head in enumerate(heads):
        rect(slide, x, y0, widths[i] - 0.03, 0.42, fill=PANEL2, line=LINE)
        text(slide, x + 0.18, y0, widths[i] - 0.3, 0.42, head, 10.5, DIM, bold=True, anchor=MSO_ANCHOR.MIDDLE)
        x += widths[i]
    for r, row in enumerate(rows):
        y = y0 + 0.45 + r * (row_h + 0.03)
        for c, cell in enumerate(row):
            x = x0 + sum(widths[:c])
            rect(slide, x, y, widths[c] - 0.03, row_h, fill=PANEL, line=LINE)
            text(
                slide,
                x + 0.18,
                y,
                widths[c] - 0.3,
                row_h,
                cell,
                9.5,
                ACCENT if c == 0 else (TEXT if c == 2 else DIM),
                bold=(c == 0),
                anchor=MSO_ANCHOR.MIDDLE,
                spacing=1.1,
            )

    text(slide, MARGIN, 6.56, 11.9, 0.28, "没有「感觉很流畅」，只有可复算的测量。", 10, FAINT)
    footer(slide, 13)
    return slide


def slide_limits(prs):
    slide = new_slide(prs)
    header(slide, "13 / 边界", "不承诺永不失败，承诺失败可观测、可恢复、可退出")

    rect(slide, MARGIN, 1.9, 5.5, 3.5, fill=PANEL, line=LINE, radius=0.04)
    text(slide, MARGIN + 0.28, 2.08, 5.0, 0.32, "已知边界", 14, TEXT, bold=True)
    bullets(
        slide,
        MARGIN + 0.28,
        2.56,
        4.95,
        2.7,
        [
            "WPS 版本敏感：12.1 实测，11.x / 13.x 待验",
            "PowerPoint 未验证，appear 是后备模式",
            "Edge 启动参数组不是稳定 API，仍需策略兜底",
            "多显示器、DPI、安全软件、组策略都会改变结果",
        ],
        size=12,
        gap=11,
    )

    rows = [
        ("瞬态失败", "超时、焦点、Edge 慢", "重试 + 退避", OK),
        ("切换失败", "退放映或起 Edge 失败", "收黑场、杀 Edge、恢复 WPS", ACCENT),
        ("返回失败", "恢复放映失败", "收黑场进 Faulted，Edge 留人工", WARN),
        ("结构性失败", "COM 连续 2s 不可用", "SafeMode / LegacyFallback 并清理", NEG),
    ]
    x0 = 6.6
    for i, (kind, when, action, color) in enumerate(rows):
        y = 1.9 + i * 0.9
        rect(slide, x0, y, 5.95, 0.8, fill=PANEL2 if i % 2 == 0 else PANEL, line=LINE, radius=0.05)
        rect(slide, x0, y, 0.05, 0.8, fill=color)
        text(slide, x0 + 0.24, y + 0.1, 1.5, 0.3, kind, 11.5, color, bold=True)
        text(slide, x0 + 1.8, y + 0.1, 2.2, 0.3, when, 10, DIM)
        text(slide, x0 + 1.8, y + 0.44, 4.0, 0.3, action, 10.5, TEXT)

    conclusion(
        slide,
        5.62,
        "每条失败路径都有对应动作和日志事件；--diagnostics 能把现场打包带走，异地复盘。",
        size=12.5,
    )
    footer(slide, 14)
    return slide


def slide_close(prs):
    slide = new_slide(prs)
    header(slide, "14 / 结论", "三句话")

    points = [
        ("01", "不改运行时也能搬动画", "前提是把「切换」放进一个观众看不见的黑场里。"),
        ("02", "自动化的关键不是更快，而是有信号", "按键 → 页码；结束 → 标题；就绪 → 覆盖窗状态。"),
        ("03", "现场稳定来自工程，不是运气", "状态机 + 两层守护 + 可复算的日志，才有可重复性。"),
    ]
    for i, (num, title, note) in enumerate(points):
        y = 1.78 + i * 1.16
        text(slide, MARGIN, y, 0.7, 0.5, num, 15, ACCENT, bold=True)
        text(slide, MARGIN + 0.75, y - 0.02, 7.4, 0.4, title, 17, TEXT, bold=True)
        text(slide, MARGIN + 0.75, y + 0.4, 7.6, 0.4, note, 12, DIM, spacing=1.3)

    rect(slide, 9.1, 1.7, 3.45, 3.5, fill=PANEL, line=LINE, radius=0.05)
    text(slide, 9.34, 1.86, 3.0, 0.3, "怎么复现", 11, ACCENT, bold=True)
    text(
        slide,
        9.34,
        2.24,
        3.0,
        2.7,
        "--validate\n--supervisor\n--status\n--stop\n--diagnostics",
        11,
        TEXT,
        font=MONO,
        spacing=1.62,
    )
    text(slide, 9.34, 4.62, 3.0, 0.4, "13 行 JSON 配置即可跑", 9.5, DIM)

    conclusion(
        slide,
        5.6,
        "wps 放映 + 一个黑场 + 一份 13 行配置 = 现场可以反复演的同一条路。",
        size=13,
    )
    text(slide, MARGIN, 6.46, 6.0, 0.4, "谢谢  ·  Q&A", 15, TEXT, bold=True)
    text(
        slide,
        8.2,
        6.46,
        4.35,
        0.4,
        "htmlppt-watchdog v0.1.0  ·  比赛项目汇报",
        9.5,
        FAINT,
        align=PP_ALIGN.RIGHT,
    )
    footer(slide, 15)
    return slide


# -------------------------------------------------------------------------- main


def build(path: str) -> None:
    prs = Presentation()
    prs.slide_width = Inches(W)
    prs.slide_height = Inches(H)

    slide_cover(prs)
    slide_problem(prs)
    slide_routes(prs)
    slide_choice(prs)
    slide_timeline(prs)
    slide_signal(prs)
    slide_cover_timing(prs)
    slide_return(prs)
    slide_focus(prs)
    slide_state_machine(prs)
    slide_supervisor(prs)
    slide_com(prs)
    slide_metrics(prs)
    slide_limits(prs)
    slide_close(prs)

    prs.save(path)
    print(f"saved {path} with {len(prs.slides.__iter__.__self__._sldIdLst)} slides")


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "out/htmlppt-watchdog-技术汇报.pptx")
