#!/usr/bin/env python3
"""Build the beginner-friendly Proteus32 user guide PDF from its Markdown source.

Usage:
    python tools/md2pdf_guide.py

Input : docs/РУКОВОДСТВО_ПОЛЬЗОВАТЕЛЯ.md
Output: docs/Руководство_пользователя_Proteus32.pdf

Supported Markdown subset (deliberately small, so the source stays hand-editable):
    # / ## / ###        headings (a second '#' starts a new page)
    paragraph            plain text
    - item               bullet list
    1. item              numbered list
    > text               callout box (left accent bar)
    | a | b |            table
    <!-- widths: 30,70 -->  column width ratios for the next table
    ---                  thin separator
    *text*               final small-print paragraph
Inline: **bold**, *italic*, `code`, {{red:текст}} (красный цвет).
"""

from __future__ import annotations

import html
import re
import sys
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import cm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate,
    Frame,
    HRFlowable,
    KeepTogether,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "РУКОВОДСТВО_ПОЛЬЗОВАТЕЛЯ.md"
DST = ROOT / "docs" / "Руководство_пользователя_Proteus32.pdf"
PAGE_FOOTER = "Фонарь с драйвером Proteus 32 — инструкция пользователя"

FONTS = {
    "Body": "C:/Windows/Fonts/segoeui.ttf",
    "Body-Bold": "C:/Windows/Fonts/segoeuib.ttf",
    "Body-Italic": "C:/Windows/Fonts/segoeuii.ttf",
    "Body-BoldItalic": "C:/Windows/Fonts/segoeuiz.ttf",
    "Mono": "C:/Windows/Fonts/consola.ttf",
    "Mono-Bold": "C:/Windows/Fonts/consolab.ttf",
}

INK = colors.HexColor("#1b1b1b")
MUTED = colors.HexColor("#5a5a5a")
ACCENT = colors.HexColor("#0f5c8c")
ACCENT_SOFT = colors.HexColor("#eaf2f8")
RULE = colors.HexColor("#c8c8c8")
HEAD_BG = colors.HexColor("#e4e8ec")
ZEBRA = colors.HexColor("#f6f7f8")
NOTE_BG = colors.HexColor("#f2f7fb")
WARN_BG = colors.HexColor("#fdf4ec")
WARN_BAR = colors.HexColor("#c46a10")
NOTE_BAR = ACCENT
RED_HEX = "#c00000"


# --------------------------------------------------------------------------- fonts
def register_fonts() -> str:
    for name, path in FONTS.items():
        if not Path(path).exists():
            sys.exit(f"font not found: {path}")
        pdfmetrics.registerFont(TTFont(name, path))
    pdfmetrics.registerFontFamily(
        "Body",
        normal="Body",
        bold="Body-Bold",
        italic="Body-Italic",
        boldItalic="Body-BoldItalic",
    )
    return "Body"


def build_styles() -> dict[str, ParagraphStyle]:
    base = dict(fontName="Body", fontSize=10.2, leading=15, textColor=INK)
    st: dict[str, ParagraphStyle] = {}
    st["title"] = ParagraphStyle(
        "title", fontName="Body-Bold", fontSize=30, leading=36, textColor=ACCENT,
        spaceAfter=6,
    )
    st["subtitle"] = ParagraphStyle(
        "subtitle", fontName="Body", fontSize=15, leading=21, textColor=MUTED,
        spaceAfter=14,
    )
    st["h1"] = ParagraphStyle(
        "h1", fontName="Body-Bold", fontSize=19, leading=24, textColor=ACCENT,
        spaceBefore=0, spaceAfter=10,
    )
    st["h2"] = ParagraphStyle(
        "h2", fontName="Body-Bold", fontSize=13.6, leading=18, textColor=ACCENT,
        spaceBefore=16, spaceAfter=6,
    )
    st["h3"] = ParagraphStyle(
        "h3", fontName="Body-Bold", fontSize=11.2, leading=15, textColor=INK,
        spaceBefore=12, spaceAfter=4,
    )
    st["body"] = ParagraphStyle("body", alignment=TA_JUSTIFY, spaceAfter=7, **base)
    st["li"] = ParagraphStyle(
        "li", alignment=TA_LEFT, leftIndent=14, bulletIndent=3, spaceAfter=5, **base
    )
    st["note"] = ParagraphStyle(
        "note", fontName="Body", fontSize=9.6, leading=14, textColor=INK, spaceAfter=0
    )
    st["cell"] = ParagraphStyle(
        "cell", fontName="Body", fontSize=9.2, leading=12.6, textColor=INK,
        splitLongWords=1, spaceAfter=0,
    )
    st["cell_head"] = ParagraphStyle(
        "cell_head", fontName="Body-Bold", fontSize=9.2, leading=12.6,
        textColor=colors.HexColor("#10222c"), splitLongWords=1, spaceAfter=0,
    )
    st["cell_mono"] = ParagraphStyle(
        "cell_mono", fontName="Mono", fontSize=8.6, leading=12.4, textColor=INK,
        splitLongWords=1, spaceAfter=0,
    )
    st["small"] = ParagraphStyle(
        "small", fontName="Body-Italic", fontSize=8.8, leading=12.5, textColor=MUTED,
        alignment=TA_LEFT, spaceBefore=16,
    )
    st["toc_head"] = ParagraphStyle(
        "toc_head", fontName="Body-Bold", fontSize=12, leading=16, textColor=INK,
        spaceAfter=7,
    )
    st["toc_part"] = ParagraphStyle(
        "toc_part", fontName="Body-Bold", fontSize=10, leading=14, textColor=ACCENT,
        spaceBefore=6, spaceAfter=2,
    )
    st["toc_item"] = ParagraphStyle(
        "toc_item", fontName="Body", fontSize=9.4, leading=13, textColor=MUTED,
        leftIndent=12, spaceAfter=1,
    )
    return st


# --------------------------------------------------------------------- inline markup
def inline(text: str) -> str:
    """Escape XML, then re-apply the small Markdown inline subset."""
    text = html.escape(text, quote=False)
    text = re.sub(r"`([^`]+)`", r'<font face="Mono" size="9">\1</font>', text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"(?<![\*\w])\*([^*]+)\*(?!\w)", r"<i>\1</i>", text)
    # Красный выделенный фрагмент: {{red:текст}}. Применяется последним,
    # чтобы внутри него уже работали **bold** и *italic*.
    text = re.sub(
        r"\{\{red:(.*?)\}\}",
        rf'<font color="{RED_HEX}">\1</font>',
        text,
        flags=re.S,
    )
    return text


# ------------------------------------------------------------------- markdown parsing
WIDTHS_RE = re.compile(r"^<!--\s*widths:\s*([0-9,\.\s]+)-->\s*$")
HEAD_RE = re.compile(r"^(#{1,3})\s+(.*)$")
NUM_RE = re.compile(r"^(\d+)\.\s+(.*)$")


def parse_blocks(lines: list[str]) -> list[tuple]:
    blocks: list[tuple] = []
    i, n = 0, len(lines)
    while i < n:
        raw = lines[i].rstrip()
        line = raw.strip()

        if not line:
            i += 1
            continue

        if WIDTHS_RE.match(line):
            i += 1
            continue

        if line == "---":
            blocks.append(("rule", None))
            i += 1
            continue

        m = HEAD_RE.match(line)
        if m:
            blocks.append((f"h{len(m.group(1))}", m.group(2).strip()))
            i += 1
            continue

        if line.startswith(">"):
            note: list[str] = []
            while i < n and lines[i].strip().startswith(">"):
                note.append(lines[i].strip().lstrip(">").strip())
                i += 1
            blocks.append(("note", " ".join(x for x in note if x)))
            continue

        if line.startswith("|"):
            rows: list[list[str]] = []
            while i < n and lines[i].strip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(set(c) <= set("-: ") and c for c in cells):
                    rows.append(cells)
                i += 1
            if rows:
                blocks.append(("table", rows))
            continue

        if line.startswith("- "):
            items: list[str] = []
            while i < n and lines[i].strip().startswith("- "):
                items.append(lines[i].strip()[2:].strip())
                i += 1
            blocks.append(("ul", items))
            continue

        m = NUM_RE.match(line)
        if m:
            items = []
            while i < n:
                mm = NUM_RE.match(lines[i].strip())
                if not mm:
                    break
                items.append((mm.group(1), mm.group(2).strip()))
                i += 1
            blocks.append(("ol", items))
            continue

        para = [line]
        i += 1
        while i < n:
            nxt = lines[i].strip()
            if (
                not nxt
                or nxt.startswith(("#", ">", "|", "- "))
                or nxt == "---"
                or NUM_RE.match(nxt)
                or WIDTHS_RE.match(nxt)
            ):
                break
            para.append(nxt)
            i += 1
        blocks.append(("p", " ".join(para)))
    return blocks


# ------------------------------------------------------------------------ rendering
def make_table(rows: list[list[str]], st: dict, avail: float) -> Table:
    ncol = max(len(r) for r in rows)
    rows = [r + [""] * (ncol - len(r)) for r in rows]
    head, body = rows[0], rows[1:]

    data = [[Paragraph(inline(c), st["cell_head"]) for c in head]]
    for r in body:
        row = []
        for idx, c in enumerate(r):
            style = st["cell_mono"] if (idx == 0 and c.startswith("N ")) else st["cell"]
            row.append(Paragraph(inline(c), style))
        data.append(row)

    widths = [1.0] * ncol
    total = sum(widths)
    col_w = [avail * w / total for w in widths]

    t = Table(data, colWidths=col_w, repeatRows=1, hAlign="LEFT")
    style = [
        ("BACKGROUND", (0, 0), (-1, 0), HEAD_BG),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("GRID", (0, 0), (-1, -1), 0.4, RULE),
        ("LINEBELOW", (0, 0), (-1, 0), 0.9, ACCENT),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]
    for r in range(1, len(data)):
        if r % 2 == 0:
            style.append(("BACKGROUND", (0, r), (-1, r), ZEBRA))
    t.setStyle(TableStyle(style))
    return t


def toc_table(toc: list[tuple[int, str]], st: dict, avail: float) -> Table:
    """Двухколоночное оглавление: главы и их разделы."""
    half = (len(toc) + 1) // 2
    cols: list[list] = [[], []]
    for i, (level, item) in enumerate(toc):
        style = st["toc_part"] if level == 0 else st["toc_item"]
        cols[0 if i < half else 1].append(Paragraph(inline(item), style))
    while len(cols[0]) < len(cols[1]):
        cols[0].append(Spacer(1, 1))
    while len(cols[1]) < len(cols[0]):
        cols[1].append(Spacer(1, 1))

    t = Table([cols], colWidths=[avail / 2 - 6, avail / 2 - 6], hAlign="LEFT")
    t.setStyle(TableStyle([
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (0, -1), 0),
        ("RIGHTPADDING", (0, 0), (0, -1), 8),
        ("LEFTPADDING", (1, 0), (1, -1), 0),
        ("RIGHTPADDING", (1, 0), (1, -1), 0),
        ("TOPPADDING", (0, 0), (-1, -1), 0),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 0),
    ]))
    return t


def note_box(text: str, st: dict, avail: float) -> Table:
    is_warn = text.lower().lstrip("*").startswith(("важно", "осторожно", "не путайте"))
    bg = WARN_BG if is_warn else NOTE_BG
    bar = WARN_BAR if is_warn else NOTE_BAR
    p = Paragraph(inline(text), st["note"])
    t = Table([[p]], colWidths=[avail], hAlign="LEFT")
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), bg),
        ("LINEBEFORE", (0, 0), (0, -1), 3, bar),
        ("BOX", (0, 0), (-1, -1), 0.3, colors.HexColor("#dfe6ec")),
        ("LEFTPADDING", (0, 0), (-1, -1), 9),
        ("RIGHTPADDING", (0, 0), (-1, -1), 9),
        ("TOPPADDING", (0, 0), (-1, -1), 7),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
    ]))
    return t


# ------------------------------------------------------------------------- document
def draw_page(canvas, doc) -> None:
    canvas.saveState()
    w, h = A4
    page = canvas.getPageNumber()
    canvas.setFont("Body", 7.8)
    canvas.setFillColor(MUTED)
    canvas.drawString(doc.leftMargin, 1.0 * cm, PAGE_FOOTER)
    canvas.drawRightString(w - doc.rightMargin, 1.0 * cm, f"стр. {page}")
    if page > 1:
        canvas.setStrokeColor(RULE)
        canvas.setLineWidth(0.4)
        canvas.line(doc.leftMargin, h - 1.35 * cm, w - doc.rightMargin, h - 1.35 * cm)
    canvas.restoreState()


def build() -> Path:
    register_fonts()
    st = build_styles()
    text = SRC.read_text(encoding="utf-8")
    blocks = parse_blocks(text.splitlines())

    doc = BaseDocTemplate(
        str(DST), pagesize=A4,
        leftMargin=2.0 * cm, rightMargin=1.7 * cm, topMargin=1.9 * cm, bottomMargin=1.6 * cm,
        title="Фонарь Proteus32 — инструкция для начинающего",
        author="Proteus32", subject="Руководство пользователя",
    )
    frame = Frame(
        doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="body",
        leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0,
    )
    doc.addPageTemplates([PageTemplate(id="main", frames=[frame], onPage=draw_page)])

    avail = doc.width
    story: list = []

    # Разделитель перед заголовком первой строки не нужен: там всё равно будет
    # разрыв страницы, и линия осталась бы одна на пустой странице.
    blocks = [
        b for i, b in enumerate(blocks)
        if not (b[0] == "rule" and i + 1 < len(blocks) and blocks[i + 1][0] == "h1")
    ]

    # Оглавление на титульном листе: части (#) и их разделы (##).
    toc: list = []
    part: str | None = None
    for kind, payload in blocks[2:]:
        if kind == "h1":
            part = payload
            toc.append((0, payload))
        elif kind == "h2" and part:
            toc.append((1, payload))

    for idx, (kind, payload) in enumerate(blocks):
        if idx == 0 and kind == "h1":
            # титульный блок: заголовок + подзаголовок + линейка, затем новая страница
            story.append(Spacer(1, 3.4 * cm))
            story.append(Paragraph(inline(payload), st["title"]))
            story.append(HRFlowable(width="100%", thickness=2, color=ACCENT,
                                    spaceBefore=2, spaceAfter=12))
            continue

        if idx == 1:
            story.append(Paragraph(inline(payload), st["subtitle"]))
            story.append(HRFlowable(width="100%", thickness=0.5, color=RULE,
                                    spaceBefore=10, spaceAfter=0))
            story.append(Spacer(1, 0.8 * cm))
            story.append(Paragraph("Содержание", st["toc_head"]))
            story.append(toc_table(toc, st, avail))
            story.append(Spacer(1, 0.5 * cm))
            story.append(note_box(
                "Инструкция описывает заводские настройки фонаря. Если вы что-то "
                "перенастроили через меню, ориентируйтесь на свои настройки.",
                st, avail,
            ))
            story.append(PageBreak())
            continue

        if kind == "h1":
            story.append(PageBreak())
            story.append(Paragraph(inline(payload), st["h1"]))
            story.append(HRFlowable(width="100%", thickness=1, color=ACCENT,
                                    spaceBefore=2, spaceAfter=10))

        elif kind == "h2":
            story.append(Paragraph(inline(payload), st["h2"]))

        elif kind == "h3":
            story.append(Paragraph(inline(payload), st["h3"]))

        elif kind == "p":
            if payload.startswith("*") and payload.endswith("*") and not payload.startswith("**"):
                story.append(Paragraph(inline(payload), st["small"]))
            else:
                story.append(Paragraph(inline(payload), st["body"]))

        elif kind in ("ul", "ol"):
            # Список держим вместе: иначе последний пункт «отваливается» на
            # почти пустую страницу.
            items = []
            if kind == "ul":
                for item in payload:
                    items.append(Paragraph(inline(item), st["li"], bulletText="•"))
            else:
                for num, item in payload:
                    items.append(Paragraph(inline(item), st["li"], bulletText=f"{num}."))
            items.append(Spacer(1, 3))
            story.append(KeepTogether(items))

        elif kind == "table":
            story.append(Spacer(1, 4))
            story.append(make_table(payload, st, avail))
            story.append(Spacer(1, 8))

        elif kind == "note":
            story.append(Spacer(1, 4))
            story.append(note_box(payload, st, avail))
            story.append(Spacer(1, 8))

        elif kind == "rule":
            story.append(Spacer(1, 6))
            story.append(HRFlowable(width="60%", thickness=0.5, color=RULE,
                                    spaceBefore=4, spaceAfter=8, hAlign="CENTER"))

    if len(story) < 3:
        sys.exit("empty document")
    doc.build(story)
    return DST


if __name__ == "__main__":
    out = build()
    print(f"OK: {out}  ({out.stat().st_size} bytes)")
