#!/usr/bin/env python3
"""Render 1:1 preview PNGs of the EVA Todo UI exactly like todo_app.c lays out.

The preview reuses the same generated bitmaps as the firmware
(main/todo_text_assets.c, produced by tools/prepare_todo_text.py) so the mock
matches the on-device pixels for all text/icons. Outputs:
  docs/preview_todo_page1.png  (default page, cursor on row 1)
  docs/preview_todo_page2.png  (page 2, only the 5th task)

Usage:
    python tools/preview_todo_ui.py
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_todo_text import DOT_DIGITS, DOT_EXTRA, build_assets  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# ---- palette & layout constants (mirror todo_app.c) ----------------------
C_BG = (0, 0, 0)
C_GREEN = (0x95, 0xEF, 0x5E)
C_YELLOW = (0xFF, 0xDC, 0x00)
C_RED = (0xFF, 0x32, 0x32)
BLACK = (0, 0, 0)

X0 = 4
PANEL_W = 232
HEADER_Y, HEADER_H, HEADER_GREEN_W = 6, 50, 150
INTERNAL_X, INTERNAL_W = 158, 78
STATUS_Y, STATUS_H = 58, 50
TASKS_Y, TASKS_H = 116, 132
ROW_H, ROW0_Y = 32, 120
CURSOR_X, CHECK_X, LABEL_X, CHECK_SIZE = 10, 22, 44, 16
NAV_Y, NAV_H, NAV_W = 258, 42, 72
PREV_X, PAGE_X, NEXT_X = 4, 84, 164
BAR_Y, BAR_H = 312, 4
PROG_W, PROG_H, PROG_SCALE = 116, 28, 4
PAGE_W, PAGE_H, PAGE_SCALE = 38, 14, 2

# 5x7 dot matrix font (0-9, /, !, -, .)
DOT = {**DOT_DIGITS, **DOT_EXTRA}


def dot_w(text, scale):
    total = 0
    for ch in text:
        total += 5 if ch in DOT else 2
    return total * scale


def draw_dot(draw, x, y, scale, text, color):
    cx = x
    for ch in text:
        if ch not in DOT:
            cx += 2 * scale
            continue
        glyph = DOT[ch]
        for row in range(7):
            bits = glyph[row]
            for col in range(5):
                if bits & (0x10 >> col):
                    draw.rectangle([cx + col * scale, y + row * scale,
                                    cx + col * scale + scale - 1,
                                    y + row * scale + scale - 1], fill=color)
        cx += 5 * scale


def paste_a8(base, asset, x, y, color):
    """Paste an A8 mask recolored to `color` (LVGL image recolor equivalent)."""
    a = asset.image.convert("L")
    solid = Image.new("RGB", a.size, color)
    base.paste(solid, (x, y), mask=a)


def paste_argb(base, asset, x, y):
    base.alpha_composite(asset.image.convert("RGBA"), (x, y))


def asset_map():
    return {a.name: a for a in build_assets()}


def draw_checkbox(draw, x, y, state):
    if state == "done":
        draw.rectangle([x, y, x + CHECK_SIZE - 1, y + CHECK_SIZE - 1], fill=C_GREEN)
    else:
        draw.rectangle([x, y, x + CHECK_SIZE - 1, y + CHECK_SIZE - 1],
                       outline=C_YELLOW, width=1)


def render_page(tasks, page_no, total_pages, assets, done_total, total_count, cursor=0, out=None):
    img = Image.new("RGBA", (240, 320), C_BG + (255,))
    draw = ImageDraw.Draw(img)

    # ---- header ----
    draw.rectangle([X0, HEADER_Y, X0 + HEADER_GREEN_W - 1, HEADER_Y + HEADER_H - 1],
                   fill=C_GREEN)
    ttl_en = assets["todo_text_ttl_en"]
    ttl_zh = assets["todo_text_ttl_zh"]
    paste_a8(img, ttl_zh, X0 + (HEADER_GREEN_W - ttl_zh.image.width) // 2,
             HEADER_Y + 1, BLACK)
    paste_a8(img, ttl_en, X0 + (HEADER_GREEN_W - ttl_en.image.width) // 2,
             HEADER_Y + HEADER_H - 2 - ttl_en.image.height, BLACK)

    draw.rectangle([INTERNAL_X, HEADER_Y, INTERNAL_X + INTERNAL_W - 1,
                    HEADER_Y + HEADER_H - 1], outline=C_YELLOW, width=1)
    paste_argb(img, assets["todo_img_internal"], INTERNAL_X + 1, HEADER_Y + 1)

    # ---- status panel ----
    draw.rectangle([X0, STATUS_Y, X0 + PANEL_W - 1, STATUS_Y + STATUS_H - 1],
                   outline=C_YELLOW, width=1)
    paste_a8(img, assets["todo_text_status_zh"], 12, STATUS_Y + 4, C_YELLOW)
    paste_a8(img, assets["todo_text_status_en"], 12, STATUS_Y + 31, C_YELLOW)
    prog = f"{done_total:02d} / {total_count:02d}"
    draw_dot(draw, X0 + PANEL_W - 6 - PROG_W,
             STATUS_Y + (STATUS_H - PROG_H) // 2, PROG_SCALE, prog, C_YELLOW)

    # ---- task panel ----
    draw.rectangle([X0, TASKS_Y, X0 + PANEL_W - 1, TASKS_Y + TASKS_H - 1],
                   outline=C_YELLOW, width=1)
    color_of = {"done": C_GREEN, "urgent": C_RED, "pending": C_YELLOW}
    for r, (asset_idx, en, zh, state) in enumerate(tasks):
        y = ROW0_Y + r * ROW_H
        col = color_of[state]
        if state == "urgent":
            draw.rectangle([X0 + 4, y + 2, X0 + PANEL_W - 5, y + ROW_H - 3],
                           outline=C_RED, width=1)
        if r == cursor:
            draw.rectangle([CURSOR_X, y + 8, CURSOR_X + 3, y + 8 + CHECK_SIZE - 1],
                           fill=C_YELLOW)
        if state == "urgent":
            paste_argb(img, assets["todo_img_urgent"], CHECK_X, y + 8)
        else:
            draw_checkbox(draw, CHECK_X, y + 8, state)
        paste_a8(img, assets[f"todo_text_t{asset_idx}_zh"], LABEL_X, y + 1, col)
        paste_a8(img, assets[f"todo_text_t{asset_idx}_en"], LABEL_X, y + 20, col)
        if r > 0:
            draw.rectangle([X0 + 8, y - 1, X0 + PANEL_W - 9, y - 1],
                           fill=C_YELLOW)

    # ---- nav ----
    for x in (PREV_X, PAGE_X, NEXT_X):
        draw.rectangle([x, NAV_Y, x + NAV_W - 1, NAV_Y + NAV_H - 1],
                       outline=C_YELLOW, width=1)
    prev = assets["todo_text_btn_prev"]
    nxt = assets["todo_text_btn_next"]
    paste_a8(img, prev, PREV_X + (NAV_W - prev.image.width) // 2,
             NAV_Y + (NAV_H - prev.image.height) // 2, C_YELLOW)
    paste_a8(img, nxt, NEXT_X + (NAV_W - nxt.image.width) // 2,
             NAV_Y + (NAV_H - nxt.image.height) // 2, C_YELLOW)
    pg = assets["todo_text_page"]
    paste_a8(img, pg, PAGE_X + (NAV_W - pg.image.width) // 2, NAV_Y + 3, C_YELLOW)
    page_txt = f"{page_no} / {total_pages}"
    draw_dot(draw, PAGE_X + (NAV_W - dot_w(page_txt, PAGE_SCALE)) // 2,
             NAV_Y + 21, PAGE_SCALE, page_txt, C_YELLOW)

    # ---- bottom green bar ----
    draw.rectangle([X0, BAR_Y, X0 + PANEL_W - 1, BAR_Y + BAR_H - 1], fill=C_GREEN)

    rgb = img.convert("RGB")
    if out is not None:
        rgb.save(out)
    return rgb


PAGE1 = [
    (0, "YAO-SHAN MW TRANS.", "巡检尧山微波传输链路", "done"),
    (1, "TAILSCALE CERT.", "续签 Tailscale 节点证书", "pending"),
    (2, "GO API TEST", "编写 Go 后端 API 测试脚本", "pending"),
    (3, "SMART GRID AUD.", "审核年度智能配电监控方案", "urgent"),
]
PAGE2 = [(4, "BACKUP RECORDS", "备份本地任务清单", "done")]


def main():
    docs = ROOT / "docs"
    docs.mkdir(exist_ok=True)
    assets = asset_map()

    for name, page, cursor in (("preview_todo_page1.png", PAGE1, 0),
                               ("preview_todo_page2.png", PAGE2, 0)):
        im = render_page(page, 1 if name.endswith("1.png") else 2, 2,
                         assets, done_total=2, total_count=5, cursor=cursor)
        # 与参考设计稿同为 456x605 的比例(最近邻放大,保留硬边像素)
        big = im.resize((456, 605), Image.NEAREST)
        big.save(str(docs / name))
        print("wrote", docs / name)

    print("assets in use:")
    total = 0
    for a in assets.values():
        n = len(list(a.image.getdata())) * (1 if a.color_format == "A8" else 4)
        total += n
        print(f"  {a.name}: {a.image.width}x{a.image.height} {a.color_format} {n}B")
    print(f"  total ~{total / 1024:.1f} KiB of Flash bitmaps")


if __name__ == "__main__":
    main()

