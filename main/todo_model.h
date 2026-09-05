// main/todo_model.h
// Todo 列表的纯数据模型:不依赖 LVGL / FreeRTOS,可在主机上单测。
// UI 层只调用这里的状态读写与翻页/光标辅助函数,保证"勾选逻辑可测试"。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 每页最多显示的任务行数(与 UI 布局一一对应)。
#define TODO_PAGE_SIZE 4

// 任务状态。URGENT 是"未完成但紧急"的特殊项,勾选后转为 DONE。
typedef enum {
    TODO_STATE_PENDING = 0,   // 未完成:黄色空心框
    TODO_STATE_DONE,          // 已完成:绿色实心框
    TODO_STATE_URGENT,        // 紧急  :红色警示框
} todo_state_t;

// 静态/运行时任务描述。id 用于远程同步,en/zh 用于界面双语排版。
typedef struct {
    const char *id;
    const char *en;
    const char *zh;
    todo_state_t state;       // 初始状态
} todo_item_t;

typedef struct {
    const todo_item_t *items; // 任务表(默认状态)
    uint8_t *states;          // 运行时状态数组,长度 count
    int count;                // 任务总数
} todo_model_t;

// 用 items 的默认状态初始化模型。states 必须能容纳 count 个字节。
void todo_model_init(todo_model_t *model, const todo_item_t *items,
                     uint8_t *states, int count);

// ---- 查询 ----------------------------------------------------------------
int todo_model_total(const todo_model_t *model);
int todo_model_done_count(const todo_model_t *model);
int todo_model_page_count(const todo_model_t *model);          // 按 TODO_PAGE_SIZE
int todo_model_visible_on_page(const todo_model_t *model, int page);
int todo_model_global_index(int page, int row);                // 越界返回 -1
todo_state_t todo_model_state(const todo_model_t *model, int global_index);
const todo_item_t *todo_model_item(const todo_model_t *model, int global_index);

// ---- 修改 ----------------------------------------------------------------
// 勾选/取消一个任务。URGENT 勾选后变 DONE;PENDING<->DONE 互切。
// 返回新状态;索引非法返回 TODO_STATE_PENDING 且不修改任何数据。
todo_state_t todo_model_toggle(todo_model_t *model, int global_index);
