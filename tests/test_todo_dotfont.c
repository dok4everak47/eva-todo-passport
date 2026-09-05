/* tests/test_todo_dotfont.c - host-side unit test for the 5x7 dot font.
 * Build & run:
 *   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_todo_dotfont.c main/todo_dotfont.c -o build_test_todo_dotfont.exe
 *   ./build_test_todo_dotfont.exe
 */
#include "todo_dotfont.h"
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                  \
        }                                                                  \
    } while (0)

int main(void)
{
    int w = 0, h = 0;

    /* "02 / 05":5+5+2+5+2+5+5 = 29 列 */
    CHECK(todo_dotfont_measure("02 / 05", 4, &w, &h));
    CHECK(w == 29 * 4);
    CHECK(h == 7 * 4);

    CHECK(todo_dotfont_measure("1 / 2", 2, &w, &h));
    CHECK(w == 19 * 2);
    CHECK(h == 14);

    /* 渲染进 A8 缓冲:内容应落在计算区域内且非全零 */
    {
        uint8_t buf[29 * 4 * 7 * 4];
        memset(buf, 0, sizeof(buf));
        CHECK(todo_dotfont_render_a8(buf, 29 * 4, 4, "02 / 05"));
        int nonzero = 0;
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0) nonzero++;
        }
        CHECK(nonzero > 0);

        /* 第一行是数字 "0" 的顶边:5 列里第 0..4 位为 01110(0x0E),放大 4 倍
         * 后 buf 第 0..3 行、第 4..7/12..15 列应为 0xFF,其余为 0。 */
        CHECK(buf[1 * (29 * 4) + 4] == 0xFF);
        CHECK(buf[1 * (29 * 4) + 8] == 0xFF);
        CHECK(buf[1 * (29 * 4) + 0] == 0x00);
        CHECK(buf[1 * (29 * 4) + 16] == 0x00);
    }

    /* 非法参数 */
    {
        uint8_t small[8];
        memset(small, 0, sizeof(small));
        CHECK(!todo_dotfont_render_a8(NULL, 10, 1, "0"));
        CHECK(!todo_dotfont_render_a8(small, 10, 0, "0"));
        CHECK(!todo_dotfont_render_a8(small, 10, 1, NULL));
    }

    if (s_failures == 0) {
        printf("test_todo_dotfont: OK\n");
        return 0;
    }
    printf("test_todo_dotfont: %d failure(s)\n", s_failures);
    return 1;
}
