// main/todo_dotfont.c
// 5x7 点阵字体实现。字形表与 tools/prepare_todo_text.py 里生成警示图标用的
// 点阵保持一致(0-9 / ! - .)。每行 1 字节,bit4..bit0 对应 5 列。
#include "todo_dotfont.h"

#include <string.h>

static const uint8_t FONT_DIGITS[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, // 0
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, // 1
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, // 2
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, // 3
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, // 4
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, // 5
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, // 6
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, // 7
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, // 8
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, // 9
};

static const uint8_t FONT_SLASH[7] = { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 };
static const uint8_t FONT_BANG[7]  = { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 };
static const uint8_t FONT_DASH[7]  = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
static const uint8_t FONT_DOT[7]   = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C };

// 每字符列宽:数字/符号 5 列,空格 2 列。
static int char_cols(char c)
{
    if (c >= '0' && c <= '9') return TODO_DOTFONT_CHAR_W;
    switch (c) {
    case '/': case '!': case '-': case '.': return TODO_DOTFONT_CHAR_W;
    default: return 2;
    }
}

static const uint8_t *char_glyph(char c)
{
    if (c >= '0' && c <= '9') return FONT_DIGITS[c - '0'];
    switch (c) {
    case '/': return FONT_SLASH;
    case '!': return FONT_BANG;
    case '-': return FONT_DASH;
    case '.': return FONT_DOT;
    default:  return NULL;
    }
}

bool todo_dotfont_measure(const char *text, int scale, int *out_w, int *out_h)
{
    if (!text || scale <= 0) return false;
    int cols = 0;
    for (const char *p = text; *p; p++) cols += char_cols(*p);
    if (out_w) *out_w = cols * scale;
    if (out_h) *out_h = TODO_DOTFONT_CHAR_H * scale;
    return true;
}

bool todo_dotfont_render_a8(uint8_t *buf, size_t stride, int scale, const char *text)
{
    if (!buf || !text || scale <= 0) return false;

    int x = 0;
    for (const char *p = text; *p; p++) {
        const uint8_t *glyph = char_glyph(*p);
        int cols = char_cols(*p);
        if (!glyph) {                       // 空格或未知字符:仅留白
            x += cols * scale;
            continue;
        }
        for (int row = 0; row < TODO_DOTFONT_CHAR_H; row++) {
            uint8_t bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < TODO_DOTFONT_CHAR_W; col++) {
                if (!(bits & (0x10u >> col))) continue;
                int px = x + col * scale;
                int py = row * scale;
                for (int dy = 0; dy < scale; dy++) {
                    memset(buf + (size_t)(py + dy) * stride + px, 0xFF, (size_t)scale);
                }
            }
        }
        x += cols * scale;
    }
    return true;
}
