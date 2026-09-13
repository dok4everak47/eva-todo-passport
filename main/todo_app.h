// main/todo_app.h
// EVA 风格 Todo List 应用:LVGL 界面 + 三键输入 + 状态同步。
#pragma once

#include "bsp_button.h"
#include "todo_model.h"
#include <stdbool.h>

#define TODO_APP_MAX_TASKS 12
#define TODO_APP_ID_LEN 64
#define TODO_APP_TITLE_LEN 80
#define TODO_APP_NOTE_LEN 150

typedef struct {
    char id[TODO_APP_ID_LEN];
    char title[TODO_APP_TITLE_LEN];
    char notes[TODO_APP_NOTE_LEN];
    todo_state_t state;
    bool urgent;
    bool deleted;
    int priority;
    int sort_order;
    int version;
} todo_app_remote_task_t;

typedef void (*todo_app_mutation_cb_t)(const char *id, todo_state_t state, void *user);
typedef void (*todo_app_delete_cb_t)(const char *id, void *user);

// 创建并载入主界面。须在持有 LVGL 锁时调用(例如 main.c 里 bsp_lvgl_lock 后)。
void todo_app_start(void);

// 物理按键事件入口。运行于按键任务,调用方需自行加 LVGL 锁(main.c 负责)。
void todo_app_handle_button(bsp_btn_t btn, bsp_btn_ev_t ev);

// 本地勾选/取消时通知同步模块。回调必须非常轻量,不能做网络 IO。
void todo_app_set_mutation_callback(todo_app_mutation_cb_t cb, void *user);
void todo_app_set_delete_callback(todo_app_delete_cb_t cb, void *user);

// 合并服务器下发的任务增量。调用方须持有 LVGL 锁。
void todo_app_apply_remote_tasks(const todo_app_remote_task_t *tasks, int count,
                                 int server_version);

// 同步模块上报状态用。调用方最好持有 LVGL 锁或在 UI 空闲时调用。
void todo_app_get_report(int *task_total, int *task_done, int *server_version);

// 只读导出当前任务清单(供 USB 串口通道等外部通道读取)。
// 调用方须持有 LVGL 锁(与 apply_remote_tasks 同一把)。返回写入的条目数。
int todo_app_export_tasks(todo_app_remote_task_t *out, int max_count);
