// main/todo_dotfont.h
// 5x7 点阵字体:用于"02 / 05"大号进度数字与底部 "1 / 2" 页码。
// 只有数字、/、!、-、. 与空格,全部直接写进 A8 缓冲区(按 scale 放大成方块),
// 由 UI 以 lv_image + 单色 recolor 上色。零字体文件、零运行时解析。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TODO_DOTFONT_CHAR_W 5
#define TODO_DOTFONT_CHAR_H 7

// 计算 text 放大 scale 倍后的像素宽高(空格按 2 列计)。
bool todo_dotfont_measure(const char *text, int scale, int *out_w, int *out_h);

// 把 text 以 scale 倍方块画进已清零的 A8 缓冲区(buf,stride)。
// 缓冲区的宽高必须不小于 todo_dotfont_measure 的结果。
// 不认识的字符按空格跳过;成功返回 true。
bool todo_dotfont_render_a8(uint8_t *buf, size_t stride, int scale, const char *text);
