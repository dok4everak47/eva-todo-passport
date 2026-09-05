#!/usr/bin/env python3
"""Generate EVA Todo UI text/icon bitmaps into main/todo_text_assets.{c,h}.

All UI copy is rendered once at *development* time and committed as C arrays so
the firmware needs no font file and no runtime text layout. The fonts below are
host fonts (Windows). They are only needed to re-run this generator, not to build
the firmware from a checkout that already contains the generated .c/.h files.

Usage:
    python tools/prepare_todo_text.py [--header main/todo_text_assets.h]
                                      [--source main/todo_text_assets.c]
"""
import argparse
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Host fonts (edit paths for non-Windows machines).
# ---------------------------------------------------------------------------
EN_FONT = r"C:\Windows\Fonts\arialbd.ttf"     # Arial Bold: EN 标题/小标签
ZH_FONT = r"C:\Windows\Fonts\simhei.ttf"      # SimHei 黑体:中文
FALLBACK_EN = [
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\bahnschrift.ttf",
    r"C:\Windows\Fonts\NotoSans-Bold.ttf",
]
FALLBACK_ZH = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\NotoSansSC-VF.ttf",
]

# Palette (same values as todo_app.c)
YELLOW = (255, 220, 0, 255)
RED = (255, 50, 50, 255)
BLACK = (0, 0, 0, 255)

# 5x7 dot font table: shared with main/todo_dotfont.c. Keep in sync!
DOT_DIGITS = {
    '0': [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
    '1': [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
    '2': [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F],
    '3': [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E],
    '4': [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02],
    '5': [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E],
    '6': [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E],
    '7': [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
    '8': [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E],
    '9': [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C],
}
DOT_EXTRA = {
    '/': [0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10],
    '!': [0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04],
    '-': [0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00],
    '.': [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C],
}


def find_font(candidates):
    for path in candidates:
        if Path(path).exists():
            return path
    raise FileNotFoundError("No font found from: " + ", ".join(candidates))


def en_font(size):
    return ImageFont.truetype(find_font(FALLBACK_EN), size)


def zh_font(size):
    return ImageFont.truetype(find_font(FALLBACK_ZH), size)


def text_bbox(text, font):
    probe = Image.new("L", (8, 8), 0)
    return ImageDraw.Draw(probe).textbbox((0, 0), text, font=font)


def render_a8_text(text, font, pad_x=2, pad_y=2):
    """White text on transparent/black, cropped to ink + padding."""
    left, top, right, bottom = text_bbox(text, font)
    w = (right - left) + pad_x * 2
    h = (bottom - top) + pad_y * 2
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((pad_x - left, pad_y - top), text, font=font, fill=255)
    return img


def render_stacked_a8(top_text, top_font, bottom_text, bottom_font,
                      pad_x=2, pad_y=1, gap=0):
    """Two-line A8 text block for small bilingual UI buttons."""
    top = render_a8_text(top_text, top_font, 0, 0)
    bottom = render_a8_text(bottom_text, bottom_font, 0, 0)
    w = max(top.width, bottom.width) + pad_x * 2
    h = top.height + bottom.height + gap + pad_y * 2
    img = Image.new("L", (w, h), 0)
    img.paste(top, ((w - top.width) // 2, pad_y), mask=top)
    img.paste(bottom, ((w - bottom.width) // 2, pad_y + top.height + gap),
              mask=bottom)
    return img


def fitted_font(text, max_width, preferred, minimum=6, zh=False):
    maker = zh_font if zh else en_font
    for size in range(preferred, minimum - 1, -1):
        font = maker(size)
        if text_bbox(text, font)[2] - text_bbox(text, font)[0] <= max_width:
            return font
    return maker(minimum)


# ---------------------------------------------------------------------------
# Asset definitions
# ---------------------------------------------------------------------------
@dataclass
class Asset:
    name: str
    image: Image.Image
    color_format: str  # "A8" or "ARGB8888"


def build_assets():
    assets = []

    def add_text(name, text, font, fmt="A8"):
        assets.append(Asset(name, render_a8_text(text, font), fmt))

    # 顶部绿色块标题(黑字,白模 -> A8 recolor 成黑). 参考设计稿:
    # 中文/日文混排大标题在上,英文副标题在下。
    add_text("todo_text_ttl_en", "TASK LIST", fitted_font("TASK LIST", 134, 17))
    add_text("todo_text_ttl_zh", "任务リスト", fitted_font("任务リスト", 134, 27, 18, zh=True))

    # 状态面板:中文大字 + 英文小字,给 02 / 05 留足视觉权重。
    add_text("todo_text_status_en", "STATUS", fitted_font("STATUS", 100, 13))
    add_text("todo_text_status_zh", "进度", fitted_font("进度", 100, 28, 18, zh=True))

    # 底部导航(黄字)
    assets.append(Asset("todo_text_btn_prev",
                        render_stacked_a8("上页", fitted_font("上页", 62, 18, 12, zh=True),
                                          "PREV", fitted_font("PREV", 62, 13)),
                        "A8"))
    assets.append(Asset("todo_text_btn_next",
                        render_stacked_a8("下页", fitted_font("下页", 62, 18, 12, zh=True),
                                          "NEXT", fitted_font("NEXT", 62, 13)),
                        "A8"))
    add_text("todo_text_page", "PAGE", fitted_font("PAGE", 60, 9))

    # 每行任务:中文主标题 + 英文/缩写副标题。静态默认任务可用预渲染 CJK;
    # 服务器动态任务仍建议 title 放短英文,notes 放中文详情。
    tasks = [
        ("YAO-SHAN MW TRANS.", "巡检尧山微波传输链路"),
        ("TAILSCALE CERT.",    "续签 Tailscale 节点证书"),
        ("GO API TEST",        "编写 Go 后端 API 测试脚本"),
        ("SMART GRID AUD.",    "审核年度智能配电监控方案"),
        ("BACKUP RECORDS",     "备份本地任务清单"),
    ]
    for i, (en, zh) in enumerate(tasks):
        add_text(f"todo_text_t{i}_en", en, fitted_font(en, 178, 10))
        add_text(f"todo_text_t{i}_zh", zh, fitted_font(zh, 178, 15, 10, zh=True))

    # 右侧 INTERNAL 块:ARGB,黄色文字 + 右侧红色斜线装饰(参考 EVA 播放器 internal)
    assets.append(Asset("todo_img_internal", render_internal(), "ARGB8888"))

    # 紧急任务警示图标:ARGB 红底 + 黑色 "!"
    assets.append(Asset("todo_img_urgent", render_urgent_icon(), "ARGB8888"))
    return assets


def render_internal():
    """74x42 ARGB:两行双语(喜碧 / HEBEI) + 三条红色斜线于最右。"""
    img = Image.new("RGBA", (74, 42), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    zh = fitted_font("喜碧", 50, 22, 16, zh=True)
    en = fitted_font("HEBEI", 50, 11)
    draw.text((2, 0), "喜碧", font=zh, fill=YELLOW)
    draw.text((2, 27), "HEBEI", font=en, fill=YELLOW)
    for y in (1, 15, 29):
        draw.polygon([(58, y), (73, y), (66, min(y + 11, 41)), (51, min(y + 11, 41))], fill=RED)
    return img


def render_urgent_icon():
    """16x16 ARGB:红色实心方块,中央黑色点阵 '!'。"""
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = img.load()
    for y in range(16):
        for x in range(16):
            px[x, y] = RED
    bang = DOT_EXTRA["!"]
    # '!' 放大 2 倍 = 10x14,居中
    ox, oy = 3, 1
    for row in range(7):
        bits = bang[row]
        for col in range(5):
            if bits & (0x10 >> col):
                for dy in range(2):
                    for dx in range(2):
                        px[ox + col * 2 + dx, oy + row * 2 + dy] = BLACK
    return img


# ---------------------------------------------------------------------------
# C output
# ---------------------------------------------------------------------------
def asset_bytes(asset):
    if asset.color_format == "A8":
        data = list(asset.image.convert("L").getdata())
        return data, asset.image.width
    data = []
    for r, g, b, a in asset.image.convert("RGBA").getdata():
        data.extend((b, g, r, a))
    return data, asset.image.width * 4


def build_source(assets):
    lines = ['#include "todo_text_assets.h"', ""]
    for asset in assets:
        data, stride = asset_bytes(asset)
        data_name = f"{asset.name}_data"
        lines.append(f"static const uint8_t {data_name}[] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{value:02X}" for value in data[i:i + 16]) + ",")
        lines.extend([
            "};",
            "",
            f"const lv_image_dsc_t {asset.name} = {{",
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
            f"    .header.cf = LV_COLOR_FORMAT_{asset.color_format},",
            "    .header.flags = 0,",
            f"    .header.w = {asset.image.width},",
            f"    .header.h = {asset.image.height},",
            f"    .header.stride = {stride},",
            f"    .data_size = {len(data)},",
            f"    .data = {data_name},",
            "};",
            "",
        ])
    return "\n".join(lines)


def build_header(assets):
    lines = ["#pragma once", "", '#include "lvgl.h"', ""]
    lines.extend(f"extern const lv_image_dsc_t {asset.name};" for asset in assets)
    lines.append("")
    return "\n".join(lines)


def print_dotfont_ascii():
    print("== dot font check ==")
    for ch, rows in {**DOT_DIGITS, **DOT_EXTRA}.items():
        print(f"[{ch}]")
        for row in rows:
            print("".join("#" if row & (0x10 >> col) else "." for col in range(5)))
        print()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", default="main/todo_text_assets.h")
    parser.add_argument("--source", default="main/todo_text_assets.c")
    args = parser.parse_args()

    assets = build_assets()
    Path(args.header).write_text(build_header(assets), encoding="utf-8")
    Path(args.source).write_text(build_source(assets), encoding="utf-8")
    for asset in assets:
        print(f"{asset.name}: {asset.image.width}x{asset.image.height} {asset.color_format}")
    print_dotfont_ascii()


if __name__ == "__main__":
    main()
