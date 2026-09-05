#!/usr/bin/env python3
"""Host-side checks for generated UI assets (tools/prepare_todo_text.py).

Run: python tests/test_todo_text_assets.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from prepare_todo_text import build_assets  # noqa: E402

FAILURES = []


def check(name, cond):
    if not cond:
        FAILURES.append(name)


def main():
    assets = {a.name: a for a in build_assets()}

    # 标题必须在绿色块(150px)内留边
    ttl = assets["todo_text_ttl_en"]
    check("ttl_en width", ttl.image.width <= 134)
    check("ttl_en a8", ttl.color_format == "A8")
    zh = assets["todo_text_ttl_zh"]
    check("ttl_zh width", zh.image.width <= 134)
    check("ttl_zh ink height", zh.image.height <= 30)

    # 任务行:中文主标题 + 英文副标题;LABEL_X 到右边框留约 184px。
    for i in range(5):
        en = assets[f"todo_text_t{i}_en"]
        z = assets[f"todo_text_t{i}_zh"]
        check(f"t{i}_en fits row", en.image.width <= 184)
        check(f"t{i}_zh fits row", z.image.width <= 184)
        check(f"t{i}_zh compact", z.image.height <= 20)

    # INTERNAL 位图:74x42 ARGB8888,应含黄色文字像素与红色斜线像素
    internal = assets["todo_img_internal"]
    check("internal size", internal.image.size == (74, 42))
    check("internal argb", internal.color_format == "ARGB8888")
    rgba = internal.image.convert("RGBA")
    has_yellow = any(
        r > 200 and g > 180 and b < 120 and a > 200 for r, g, b, a in rgba.getdata()
    )
    has_red = any(
        r > 200 and g < 120 and b < 120 and a > 200 for r, g, b, a in rgba.getdata()
    )
    check("internal has yellow text", has_yellow)
    check("internal has red slashes", has_red)

    # 紧急图标:16x16 ARGB,红底且中央有黑色 "!"
    icon = assets["todo_img_urgent"]
    check("urgent size", icon.image.size == (16, 16))
    rgba = icon.image.convert("RGBA")
    px = list(rgba.getdata())
    check("urgent red corner", px[0][:3] == (255, 50, 50))
    check("urgent black center", px[8 * 16 + 8][:3] == (0, 0, 0))

    if FAILURES:
        print("FAILURES:")
        for f in FAILURES:
            print(" -", f)
        raise SystemExit(1)
    print("test_todo_text_assets: OK")


if __name__ == "__main__":
    main()
