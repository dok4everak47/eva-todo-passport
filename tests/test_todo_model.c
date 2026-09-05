/* tests/test_todo_model.c - host-side unit test for the pure todo model.
 * Build & run:
 *   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_todo_model.c main/todo_model.c -o build_test_todo_model.exe
 *   ./build_test_todo_model.exe
 */
#include "todo_model.h"
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

static const todo_item_t ITEMS[] = {
    { "a", "A", "甲", TODO_STATE_DONE },
    { "b", "B", "乙", TODO_STATE_PENDING },
    { "c", "C", "丙", TODO_STATE_URGENT },
    { "d", "D", "丁", TODO_STATE_PENDING },
    { "e", "E", "戊", TODO_STATE_DONE },
};
static uint8_t STATES[5];

int main(void)
{
    todo_model_t m;
    todo_model_init(&m, ITEMS, STATES, 5);

    CHECK(todo_model_total(&m) == 5);
    CHECK(todo_model_done_count(&m) == 2);          /* A 与 E */
    CHECK(todo_model_page_count(&m) == 2);          /* 4 + 1 */
    CHECK(todo_model_visible_on_page(&m, 0) == 4);
    CHECK(todo_model_visible_on_page(&m, 1) == 1);
    CHECK(todo_model_visible_on_page(&m, 2) == 0);
    CHECK(todo_model_global_index(0, 0) == 0);
    CHECK(todo_model_global_index(1, 0) == 4);
    CHECK(todo_model_global_index(0, 4) == -1);
    CHECK(todo_model_global_index(1, 1) == 5);      /* 越界,由 UI 层兜底 */

    CHECK(todo_model_state(&m, 2) == TODO_STATE_URGENT);
    CHECK(todo_model_item(&m, 2) == &ITEMS[2]);
    CHECK(todo_model_item(&m, 99) == NULL);

    /* 勾选:URGENT -> DONE */
    CHECK(todo_model_toggle(&m, 2) == TODO_STATE_DONE);
    CHECK(todo_model_done_count(&m) == 3);
    /* 再勾选:DONE -> PENDING */
    CHECK(todo_model_toggle(&m, 2) == TODO_STATE_PENDING);
    CHECK(todo_model_done_count(&m) == 2);
    /* PENDING -> DONE */
    CHECK(todo_model_toggle(&m, 1) == TODO_STATE_DONE);
    CHECK(todo_model_done_count(&m) == 3);
    /* 非法下标不改变任何状态 */
    CHECK(todo_model_toggle(&m, -1) == TODO_STATE_PENDING);
    CHECK(todo_model_toggle(&m, 99) == TODO_STATE_PENDING);
    CHECK(todo_model_done_count(&m) == 3);
    CHECK(STATES[2] == (uint8_t)TODO_STATE_PENDING);
    CHECK(STATES[3] == (uint8_t)TODO_STATE_PENDING);

    if (s_failures == 0) {
        printf("test_todo_model: OK\n");
        return 0;
    }
    printf("test_todo_model: %d failure(s)\n", s_failures);
    return 1;
}
