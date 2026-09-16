from PIL import Image, ImageDraw, ImageFont
import math

SIZE = 256
img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

# ---- Background: rounded square with subtle vertical gradient (deep teal -> darker teal) ----
def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

top_color = (47, 122, 104)     # #2f7a68
bottom_color = (24, 74, 64)    # #184a40

grad = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
gd = ImageDraw.Draw(grad)
for y in range(SIZE):
    t = y / (SIZE - 1)
    color = lerp(top_color, bottom_color, t)
    gd.line([(0, y), (SIZE, y)], fill=color + (255,))

mask = Image.new("L", (SIZE, SIZE), 0)
mdraw = ImageDraw.Draw(mask)
radius = 56
mdraw.rounded_rectangle([0, 0, SIZE - 1, SIZE - 1], radius=radius, fill=255)
img.paste(grad, (0, 0), mask)
draw = ImageDraw.Draw(img)

# ---- Clipboard body (white, slightly off-white) ----
cb_left, cb_top, cb_right, cb_bottom = 50, 40, 190, 216
clip_color = (247, 245, 238, 255)
shadow_color = (0, 0, 0, 40)

# soft shadow
draw.rounded_rectangle([cb_left+5, cb_top+9, cb_right+5, cb_bottom+9], radius=16, fill=shadow_color)
# clipboard board
draw.rounded_rectangle([cb_left, cb_top, cb_right, cb_bottom], radius=16, fill=clip_color, outline=(210, 205, 190, 255), width=3)

# clip tab at top
tab_w, tab_h = 60, 28
tab_left = (cb_left + cb_right) // 2 - tab_w // 2
tab_top = cb_top - tab_h // 2
draw.rounded_rectangle([tab_left, tab_top, tab_left + tab_w, tab_top + tab_h], radius=9,
                        fill=(190, 190, 182, 255), outline=(150,150,142,255), width=3)

# ---- Lines of "text" on the clipboard (representing document) ----
line_color = (170, 166, 152, 255)
line_x0 = cb_left + 18
line_x1 = cb_right - 18
line_y_start = cb_top + 50
for i in range(3):
    y = line_y_start + i * 19
    draw.rounded_rectangle([line_x0, y, line_x1 - (14 if i == 2 else 0), y + 9], radius=4, fill=line_color)

# ---- Translate glyph badge (circle) bottom-right, overlapping clipboard ----
badge_cx, badge_cy, badge_r = 188, 188, 58
badge_color = (232, 168, 62, 255)  # warm amber accent
draw.ellipse([badge_cx - badge_r, badge_cy - badge_r, badge_cx + badge_r, badge_cy + badge_r],
             fill=badge_color, outline=(255, 255, 255, 255), width=7)

# Draw two curved translate arrows inside the badge (simple "swap" motif)
def draw_arrow_arc(draw, cx, cy, r, start_angle, end_angle, color, width, arrow_size):
    draw.arc([cx - r, cy - r, cx + r, cy + r], start=start_angle, end=end_angle, fill=color, width=width)
    ang = math.radians(end_angle)
    ex = cx + r * math.cos(ang)
    ey = cy + r * math.sin(ang)
    tang = ang + math.pi / 2
    p1 = (ex + arrow_size * math.cos(tang + 2.6), ey + arrow_size * math.sin(tang + 2.6))
    p2 = (ex + arrow_size * math.cos(tang - 2.6), ey + arrow_size * math.sin(tang - 2.6))
    draw.polygon([ (ex, ey), p1, p2 ], fill=color)

arrow_color = (255, 255, 255, 255)
ar = 28
draw_arrow_arc(draw, badge_cx, badge_cy - 8, ar, 200, 340, arrow_color, 7, 8)
draw_arrow_arc(draw, badge_cx, badge_cy + 8, ar, 20, 160, arrow_color, 7, 8)

# ---- Save multi-size ICO ----
sizes = [(256,256), (128,128), (64,64), (48,48), (32,32), (16,16)]
img.save("icon.ico", sizes=sizes)
img.save("icon_preview.png")
print("done")
