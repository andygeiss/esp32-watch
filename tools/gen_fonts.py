#!/usr/bin/env python3
"""Generate the fonts the watch face draws with.

LVGL ships Montserrat pre-generated up to 48 px, which is far too small on a
410 x 502 panel, so the clock digits and the date come from here instead. The
assistant's two indicators come from here too: LVGL's built-in symbols have no
microphone at all, and the speaker belongs in the same file as its opposite
number. Both source faces are the ones LVGL ships in its gitignored checkout.
The output is checked in; this is only needed to change a size. It wants node,
because the converter comes from npm.

    tools/gen_fonts.py

Both fonts get tabular figures: every digit is rewritten to one fixed advance
with its ink centred in that cell, the way the digit cells of a VFD sit.
Montserrat's figures are proportional — a '1' is little more than half the
width of a '0' — so without this a centred clock shifts sideways whenever a
digit changes, which at this size is impossible to miss. Punctuation keeps its
own advance: a '/' stranded in the middle of a digit cell just looks lost.

Changing a size here means changing three more places: the font name and the
metrics in ui.c, and the file name in CMakeLists.txt.
"""

import pathlib
import re
import subprocess
import sys

# Pinned like everything else here: a converter bump may change the emitted
# layout, and the rewrite below reads that layout.
CONVERTER = "lv_font_conv@1.5.3"

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILT_IN = ROOT / "lvgl" / "scripts" / "built_in_font"
MONTSERRAT = BUILT_IN / "Montserrat-Medium.ttf"
FONTAWESOME = BUILT_IN / "FontAwesome5-Solid+Brands+Regular.woff"

# The clock digits, as large as a 32 px edge margin leaves room for.
TIME_SIZE = 118
# The date sits under the time at about 60% of it, and needs the '/' of MM/DD.
DATE_SIZE = 72
# The corner readouts, matching the built-in Montserrat the other three use.
CORNER_SIZE = 18

FONTS = [
    (TIME_SIZE, MONTSERRAT, "0x30-0x39", "ui_font_digits_{}".format(TIME_SIZE)),
    (DATE_SIZE, MONTSERRAT, "0x2F-0x39", "ui_font_date_{}".format(DATE_SIZE)),
    # fa-volume-up and fa-microphone: speaking, and listening.
    (CORNER_SIZE, FONTAWESOME, "0xF028,0xF130",
     "ui_font_assistant_{}".format(CORNER_SIZE)),
]

# One entry of the emitted glyph_dsc[] table, in glyph id order. adv_w is 8.4
# fixed point: the stored value is 16 * pixels.
GLYPH_RE = re.compile(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
    r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}"
)

# One cmap entry, which is what says who is a digit: glyph id first_id + i
# holds the character range_start + i.
CMAP_RE = re.compile(
    r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+)"
)


def digit_glyph_ids(source):
    """Glyph ids of '0'-'9', read from the font's own cmap."""
    cmaps = list(CMAP_RE.finditer(source))
    ids = set()
    for m in cmaps:
        start, length, first_id = (int(g) for g in m.groups())
        for i in range(length):
            if 0x30 <= start + i <= 0x39:
                ids.add(first_id + i)

    # An icon font has no digits and never reads this mapping, so only insist
    # on the plain contiguous cmap when something actually depends on it.
    if ids and source.count("LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY") != len(cmaps):
        sys.exit("cmap is not the plain contiguous form — has {} changed its "
                 "output format?".format(CONVERTER))
    return ids


def tabular(source):
    """Give every digit the widest digit's advance, ink centred in the cell."""
    digits = digit_glyph_ids(source)
    if not digits:
        return source, None  # an icon font has nothing to line up

    entries = list(GLYPH_RE.finditer(source))
    if not digits.issubset(range(len(entries))):
        sys.exit("cmap points past the end of glyph_dsc")

    cell = max(round(int(entries[i].group(2)) / 16) for i in digits)

    seen = [-1]

    def rewrite(m):
        seen[0] += 1
        if seen[0] not in digits:
            return m.group(0)
        box_w = int(m.group(3))
        return (
            "{{.bitmap_index = {}, .adv_w = {}, .box_w = {}, .box_h = {}, "
            ".ofs_x = {}, .ofs_y = {}}}"
        ).format(m.group(1), cell * 16, box_w, m.group(4),
                 (cell - box_w) // 2, m.group(6))

    return GLYPH_RE.sub(rewrite, source), cell


def generate(size, source_font, unicode_range, name):
    out = ROOT / "ui" / "{}.c".format(name)

    # --no-kerning: tabular figures must not have pairs nudging digits back out
    # of their cells. --no-compress keeps the glyphs a plain byte array.
    subprocess.run(
        ["npx", "--yes", CONVERTER,
         "--font", str(source_font), "--range", unicode_range,
         "--size", str(size), "--bpp", "4",
         "--no-compress", "--no-prefilter", "--no-kerning",
         "--format", "lvgl", "--lv-font-name", name,
         "-o", str(out)],
        check=True,
    )

    source, cell = tabular(out.read_text())
    if cell is None:
        header = (
            "/* Generated by tools/gen_fonts.py — do not edit by hand.\n"
            " * {} {} px, {}.\n"
            " */\n"
        ).format(source_font.name, size, unicode_range)
    else:
        header = (
            "/* Generated by tools/gen_fonts.py — do not edit by hand.\n"
            " * {} {} px, {}, digits rewritten to a fixed {} px advance so\n"
            " * that nothing shifts when a digit changes.\n"
            " */\n"
        ).format(source_font.name, size, unicode_range, cell)
    out.write_text(header + source)

    line_height = re.search(r"\.line_height = (\d+)", source).group(1)
    print("{}: {} px, {} px line height, {}".format(
        out.name, size, line_height,
        "no digits to line up" if cell is None else "{} px digit cell".format(cell)))


def main():
    for face in (MONTSERRAT, FONTAWESOME):
        if not face.exists():
            sys.exit("{} is missing — see CLAUDE.md for the lvgl/ checkout".format(face))
    for font in FONTS:
        generate(*font)


if __name__ == "__main__":
    main()
