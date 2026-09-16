#!/usr/bin/env python3
"""Mirror of LayoutControls' arithmetic: verifies no control overlaps another and
nothing escapes the client area, across DPI scalings and window widths.
Also renders a preview PNG of the header rows so the alignment can be eyeballed.
"""
from PIL import Image, ImageDraw, ImageFont
import sys

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def scale(v, dpi):
    return (v * dpi) // 96


def measure(text, font):
    return int(font.getlength(text))


def layout(W, H, dpi, font):
    """Returns dict name -> (x, y, w, h), mirroring translator.c LayoutControls."""
    S = lambda v: scale(v, dpi)
    r = {}

    margin, gap, group_gap, row_gap, label_pad = S(12), S(6), S(16), S(10), S(6)
    content_w = max(W - margin * 2, S(220))

    row_h = max(S(21), S(22))          # ComboClosedHeight, floored at Scale(22)
    label_h = row_h

    # ---- row 1 ----
    row1_y = S(12)
    src_label_w = measure("Source:", font) + label_pad
    tgt_label_w = measure("Target:", font) + label_pad
    combo_space = content_w - src_label_w - tgt_label_w - gap * 2 - group_gap
    combo_w = min(max(combo_space // 2, S(92)), S(190))

    x = margin
    r["lbl Source:"] = (x, row1_y, src_label_w, label_h)
    x += src_label_w + gap
    r["combo source"] = (x, row1_y, combo_w, row_h)
    x += combo_w + group_gap

    tgt_combo_x = margin + content_w - combo_w
    tgt_label_x = tgt_combo_x - gap - tgt_label_w
    if tgt_label_x < x:
        tgt_label_x = x
        tgt_combo_x = tgt_label_x + tgt_label_w + gap
    r["lbl Target:"] = (tgt_label_x, row1_y, tgt_label_w, label_h)
    r["combo target"] = (tgt_combo_x, row1_y, combo_w, row_h)

    # ---- row 2 ----
    row2_y = row1_y + row_h + row_gap
    glyph = S(22)
    cb1_w = measure("Always on top", font) + glyph
    cb2_w = measure("Auto paste", font) + glyph
    eng_label_w = measure("Engine:", font) + label_pad
    eng_combo_w = S(124)

    x = margin
    r["chk topmost"] = (x, row2_y, cb1_w, row_h)
    x += cb1_w + group_gap
    r["chk autopaste"] = (x, row2_y, cb2_w, row_h)
    x += cb2_w + group_gap

    eng_combo_x = margin + content_w - eng_combo_w
    eng_label_x = eng_combo_x - gap - eng_label_w
    if eng_label_x < x:
        eng_label_x = x
        eng_combo_x = eng_label_x + eng_label_w + gap
        overflow = (eng_combo_x + eng_combo_w) - (margin + content_w)
        if overflow > 0:
            eng_combo_w = max(eng_combo_w - overflow, S(84))
    r["lbl Engine:"] = (eng_label_x, row2_y, eng_label_w, label_h)
    r["combo engine"] = (eng_combo_x, row2_y, eng_combo_w, row_h)

    # ---- buttons / edits ----
    btn_h, btn_gap = S(30), S(8)
    btn1_w = measure("Translate", font) + S(36)
    btn2_w = measure("⇄ Reverse", font) + S(36)

    text_label_h = S(20)
    src_text_label_y = row2_y + row_h + S(12)
    input_top = src_text_label_y + text_label_h + S(3)
    prog_h, prog_gap1, prog_gap2, label_gap = S(6), S(6), S(8), S(3)

    fixed_vert = (input_top + btn_gap + btn_h + prog_gap1 + prog_h
                  + prog_gap2 + text_label_h + label_gap + margin)
    edits_total = max(H - fixed_vert, S(56) * 2)
    input_h = edits_total // 2
    output_h = edits_total - input_h

    r["lbl Source text:"] = (margin, src_text_label_y, content_w, text_label_h)
    r["edit input"] = (margin, input_top, content_w, input_h)

    btn_y = input_top + input_h + btn_gap
    r["btn Translate"] = (margin, btn_y, btn1_w, btn_h)
    r["btn Reverse"] = (margin + btn1_w + btn_gap, btn_y, btn2_w, btn_h)

    status_x = margin + btn1_w + btn_gap + btn2_w + S(12)
    status_w = max((margin + content_w) - status_x, S(40))
    r["lbl status"] = (status_x, btn_y + (btn_h - text_label_h) // 2, status_w, text_label_h)

    prog_y = btn_y + btn_h + prog_gap1
    r["progress"] = (margin, prog_y, content_w, prog_h)
    out_label_y = prog_y + prog_h + prog_gap2
    r["lbl Translation:"] = (margin, out_label_y, content_w, text_label_h)
    r["edit output"] = (margin, out_label_y + text_label_h + label_gap, content_w, output_h)
    return r


def rects_overlap(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def check(W, H, dpi, font):
    r = layout(W, H, dpi, font)
    problems = []
    names = list(r)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            # Full-width labels sit above their own edit box; only same-row pairs matter,
            # and the generic overlap test already covers that correctly.
            if rects_overlap(r[names[i]], r[names[j]]):
                problems.append("overlap: %s x %s" % (names[i], names[j]))
    for n, (x, y, w, h) in r.items():
        if x < 0 or y < 0:
            problems.append("negative origin: %s" % n)
        if x + w > W:
            problems.append("escapes right edge: %s (%d > %d)" % (n, x + w, W))
        if w <= 0 or h <= 0:
            problems.append("degenerate size: %s" % n)
    return r, problems


def preview(path, W, H, dpi, font, title):
    r = layout(W, H, dpi, font)
    img = Image.new("RGB", (W, H), (240, 240, 240))
    d = ImageDraw.Draw(img)
    for name, (x, y, w, h) in sorted(r.items(), key=lambda kv: kv[1][1]):
        if name.startswith("edit"):
            fill, outline = (255, 255, 255), (130, 130, 130)
        elif name.startswith("combo"):
            fill, outline = (252, 252, 252), (120, 120, 120)
        elif name.startswith("btn"):
            fill, outline = (225, 225, 225), (120, 120, 120)
        elif name.startswith("progress"):
            fill, outline = (210, 210, 210), (170, 170, 170)
        else:
            fill, outline = None, None
        if fill:
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=fill, outline=outline)
        label = {
            "lbl Source:": "Source:", "lbl Target:": "Target:", "lbl Engine:": "Engine:",
            "chk topmost": "☐ Always on top", "chk autopaste": "☐ Auto paste",
            "combo source": "Auto-detect ▾", "combo target": "Russian ▾",
            "combo engine": "DeepL ▾", "btn Translate": "Translate",
            "btn Reverse": "⇄ Reverse", "lbl status": "Copied to clipboard",
            "lbl Source text:": "Source text:", "lbl Translation:": "Translation:",
        }.get(name)
        if label:
            ty = y + max(0, (h - (font.size + 2)) // 2)
            d.text((x + (4 if fill else 0), ty), label, fill=(20, 20, 20), font=font)
    d.text((8, H - font.size - 6), title, fill=(120, 120, 120), font=font)
    img.save(path)


def main():
    ok = True
    for dpi in (96, 120, 144, 192):
        font = ImageFont.truetype(FONT_PATH, max(11, scale(12, dpi)))
        widths = [scale(560, dpi), scale(620, dpi), scale(900, dpi), scale(1400, dpi)]
        for W in widths:
            H = scale(540, dpi)
            r, problems = check(W, H, dpi, font)
            status = "ok" if not problems else "FAIL"
            print("dpi=%-4d W=%-5d H=%-5d %s" % (dpi, W, H, status))
            for p in problems:
                ok = False
                print("    ", p)

    font96 = ImageFont.truetype(FONT_PATH, 12)
    preview("layout_100.png", 620, 540, 96, font96, "100% (96 dpi), 620x540")
    font150 = ImageFont.truetype(FONT_PATH, 18)
    preview("layout_150.png", 930, 810, 144, font150, "150% (144 dpi), 930x810")
    print("\npreviews written")
    return 0 if ok else 1


sys.exit(main())
