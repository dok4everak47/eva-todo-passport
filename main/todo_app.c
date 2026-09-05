// main/todo_app.c
// EVA(新世纪福音战士)风格 Todo List。
//
// 运行方式:
//  * 离线启动:先显示固件内置 5 条默认任务。
//  * 网络同步成功:服务器返回的任务全量覆盖本地显示(最多 TODO_APP_MAX_TASKS 条)。
//  * 本地 OK 勾选:立刻更新 UI,并通过轻量回调把 mutation 交给同步模块稍后上传。
#include "todo_app.h"
#include "todo_model.h"
#include "todo_dotfont.h"
#include "todo_text_assets.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "todo_app";

#define C_BG      0x000000
#define C_GREEN   0x95EF5E
#define C_YELLOW  0xFFDC00
#define C_RED     0xFF3232
#define C_GRAY    0x606060

#define X0           4
#define PANEL_W      232
#define HEADER_Y     6
#define HEADER_H     50
#define HEADER_GREEN_W 150
#define INTERNAL_X   158
#define INTERNAL_W   78
#define STATUS_Y     58
#define STATUS_H     50
#define STATUS_LABEL_X 12
#define TASKS_Y      116
#define TASKS_H      132
#define ROW_H        32
#define ROW0_Y       120
#define CURSOR_X     10
#define CHECK_X      22
#define LABEL_X      44
#define CHECK_SIZE   16
#define NAV_Y        258
#define NAV_H        42
#define NAV_W        72
#define PREV_X       4
#define PAGE_X       84
#define NEXT_X       164
#define BAR_Y        312
#define BAR_H        4
#define PROG_W       116
#define PROG_H       28
#define PROG_SCALE   4
#define PAGE_W       38
#define PAGE_H       14
#define PAGE_SCALE   2

typedef struct {
    const char *id;
    const char *en;
    const char *zh;
    todo_state_t state;
    const lv_image_dsc_t *en_img;
    const lv_image_dsc_t *zh_img;
} default_task_t;

static const default_task_t DEFAULTS[] = {
    { "task-yaoshan-mw",       "YAO-SHAN MW TRANS.", "巡检尧山微波传输链路", TODO_STATE_DONE,    &todo_text_t0_en, &todo_text_t0_zh },
    { "task-tailscale-cert",   "TAILSCALE CERT.",    "续签 Tailscale 节点证书", TODO_STATE_PENDING, &todo_text_t1_en, &todo_text_t1_zh },
    { "task-go-api-test",      "GO API TEST",        "编写 Go 后端 API 测试脚本", TODO_STATE_PENDING, &todo_text_t2_en, &todo_text_t2_zh },
    { "task-smart-grid-audit", "SMART GRID AUD.",    "审核年度智能配电监控方案", TODO_STATE_URGENT,  &todo_text_t3_en, &todo_text_t3_zh },
    { "task-backup-records",   "BACKUP RECORDS",     "备份本地任务清单", TODO_STATE_DONE,    &todo_text_t4_en, &todo_text_t4_zh },
};

static char s_ids[TODO_APP_MAX_TASKS][TODO_APP_ID_LEN];
static char s_titles[TODO_APP_MAX_TASKS][TODO_APP_TITLE_LEN];
static char s_notes[TODO_APP_MAX_TASKS][TODO_APP_NOTE_LEN];
static todo_item_t s_items[TODO_APP_MAX_TASKS];
static uint8_t s_states[TODO_APP_MAX_TASKS];
static int s_item_count;
static int s_server_version;

static todo_model_t s_model;
static lv_obj_t *s_scr;
static int s_page;
static int s_cursor;
static todo_app_mutation_cb_t s_mutation_cb;
static void *s_mutation_user;

static lv_obj_t *s_frame[TODO_PAGE_SIZE];
static lv_obj_t *s_cursor_bar[TODO_PAGE_SIZE];
static lv_obj_t *s_check[TODO_PAGE_SIZE];
static lv_obj_t *s_urgent[TODO_PAGE_SIZE];
static lv_obj_t *s_en_img[TODO_PAGE_SIZE];
static lv_obj_t *s_zh_img[TODO_PAGE_SIZE];
static lv_obj_t *s_title_lbl[TODO_PAGE_SIZE];
static lv_obj_t *s_note_lbl[TODO_PAGE_SIZE];
static lv_obj_t *s_sep[TODO_PAGE_SIZE - 1];
static lv_obj_t *s_prev_box, *s_next_box;
static lv_obj_t *s_prev_txt, *s_next_txt;
static uint8_t s_prog_buf[PROG_W * PROG_H];
static lv_image_dsc_t s_prog_dsc;
static lv_obj_t *s_prog_img;
static uint8_t s_page_buf[PAGE_W * PAGE_H];
static lv_image_dsc_t s_page_dsc;
static lv_obj_t *s_page_img;

static void apply_page(void);
static void render_progress(void);

static void copy_trunc(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_size, "%s", src);
}

static void set_item(int i, const char *id, const char *title, const char *notes,
                     todo_state_t state)
{
    copy_trunc(s_ids[i], sizeof(s_ids[i]), id);
    copy_trunc(s_titles[i], sizeof(s_titles[i]), title);
    copy_trunc(s_notes[i], sizeof(s_notes[i]), notes);
    s_items[i].id = s_ids[i];
    s_items[i].en = s_titles[i];
    s_items[i].zh = s_notes[i];
    s_items[i].state = state;
}

static bool ascii_printable(const char *s)
{
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) return false;
    }
    return true;
}

static void reset_defaults(void)
{
    s_item_count = (int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0]));
    if (s_item_count > TODO_APP_MAX_TASKS) s_item_count = TODO_APP_MAX_TASKS;
    for (int i = 0; i < s_item_count; i++) {
        set_item(i, DEFAULTS[i].id, DEFAULTS[i].en, DEFAULTS[i].zh, DEFAULTS[i].state);
    }
}

static const default_task_t *matching_default(const todo_item_t *item)
{
    if (!item || !item->id) return NULL;
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        if (strcmp(item->id, DEFAULTS[i].id) == 0 &&
            strcmp(item->en, DEFAULTS[i].en) == 0 &&
            strcmp(item->zh, DEFAULTS[i].zh) == 0) {
            return &DEFAULTS[i];
        }
    }
    return NULL;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t bg, uint32_t border, int border_w)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(obj, border_w, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *make_image(lv_obj_t *parent, const lv_image_dsc_t *src,
                            int x, int y, uint32_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    lv_obj_set_style_image_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w,
                            const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    return label;
}

static void hide(lv_obj_t *obj) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); }

static int visible_count(void)
{
    return todo_model_visible_on_page(&s_model, s_page);
}

static int global_of(int row)
{
    int g = todo_model_global_index(s_page, row);
    return (g < 0 || g >= todo_model_total(&s_model)) ? -1 : g;
}

static uint32_t state_color(todo_state_t st)
{
    switch (st) {
    case TODO_STATE_DONE: return C_GREEN;
    case TODO_STATE_URGENT: return C_RED;
    default: return C_YELLOW;
    }
}

static void draw_dot_text(lv_obj_t *img, uint8_t *buf, int w, int h,
                          int scale, const char *text)
{
    int tw = 0, th = 0;
    if (!todo_dotfont_measure(text, scale, &tw, &th) || tw > w || th > h) {
        memset(buf, 0, (size_t)w * (size_t)h);
        lv_obj_invalidate(img);
        return;
    }
    memset(buf, 0, (size_t)w * (size_t)h);
    todo_dotfont_render_a8(buf, (size_t)w, scale, text);
    lv_obj_invalidate(img);
}

static void render_progress(void)
{
    char text[16];
    snprintf(text, sizeof(text), "%02d / %02d",
             todo_model_done_count(&s_model), todo_model_total(&s_model));
    draw_dot_text(s_prog_img, s_prog_buf, PROG_W, PROG_H, PROG_SCALE, text);
}

static void render_page_indicator(void)
{
    char text[16];
    snprintf(text, sizeof(text), "%d / %d", s_page + 1,
             todo_model_page_count(&s_model));
    draw_dot_text(s_page_img, s_page_buf, PAGE_W, PAGE_H, PAGE_SCALE, text);
}

static void sync_cursor(void)
{
    for (int r = 0; r < TODO_PAGE_SIZE; r++) {
        if (global_of(r) < 0 || r != s_cursor) hide(s_cursor_bar[r]);
        else show(s_cursor_bar[r]);
    }
}

static void sync_nav(void)
{
    int pages = todo_model_page_count(&s_model);
    uint32_t prev_col = (s_page > 0) ? C_YELLOW : C_GRAY;
    uint32_t next_col = (s_page < pages - 1) ? C_YELLOW : C_GRAY;
    lv_obj_set_style_border_color(s_prev_box, lv_color_hex(prev_col), 0);
    lv_obj_set_style_border_color(s_next_box, lv_color_hex(next_col), 0);
    lv_obj_set_style_image_recolor(s_prev_txt, lv_color_hex(prev_col), 0);
    lv_obj_set_style_image_recolor(s_next_txt, lv_color_hex(next_col), 0);
}

static void sync_row(int row)
{
    int g = global_of(row);
    if (g < 0) {
        hide(s_frame[row]);
        hide(s_cursor_bar[row]);
        hide(s_check[row]);
        hide(s_urgent[row]);
        hide(s_en_img[row]);
        hide(s_zh_img[row]);
        hide(s_title_lbl[row]);
        hide(s_note_lbl[row]);
        return;
    }

    const todo_item_t *item = todo_model_item(&s_model, g);
    todo_state_t st = todo_model_state(&s_model, g);
    uint32_t col = state_color(st);

    const default_task_t *def = matching_default(item);
    if (def) {
        lv_image_set_src(s_en_img[row], def->en_img);
        lv_image_set_src(s_zh_img[row], def->zh_img);
        lv_obj_set_style_image_recolor(s_en_img[row], lv_color_hex(col), 0);
        lv_obj_set_style_image_recolor(s_zh_img[row], lv_color_hex(col), 0);
        show(s_en_img[row]);
        show(s_zh_img[row]);
        hide(s_title_lbl[row]);
        hide(s_note_lbl[row]);
    } else {
        lv_label_set_text(s_title_lbl[row], item->en);
        lv_label_set_text(s_note_lbl[row],
                          ascii_printable(item->zh) ? item->zh : "SERVER TASK");
        lv_obj_set_style_text_color(s_title_lbl[row], lv_color_hex(col), 0);
        lv_obj_set_style_text_color(s_note_lbl[row], lv_color_hex(col), 0);
        hide(s_en_img[row]);
        hide(s_zh_img[row]);
        show(s_title_lbl[row]);
        show(s_note_lbl[row]);
    }

    if (st == TODO_STATE_URGENT) {
        hide(s_check[row]);
        show(s_urgent[row]);
        show(s_frame[row]);
    } else {
        show(s_check[row]);
        hide(s_urgent[row]);
        hide(s_frame[row]);
        if (st == TODO_STATE_DONE) {
            lv_obj_set_style_bg_opa(s_check[row], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_check[row], lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_border_color(s_check[row], lv_color_hex(C_GREEN), 0);
        } else {
            lv_obj_set_style_bg_opa(s_check[row], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(s_check[row], lv_color_hex(C_YELLOW), 0);
        }
    }
}

static void sync_separators(void)
{
    for (int k = 1; k < TODO_PAGE_SIZE; k++) {
        if (global_of(k) < 0) hide(s_sep[k - 1]);
        else show(s_sep[k - 1]);
    }
}

static void apply_page(void)
{
    int pages = todo_model_page_count(&s_model);
    if (s_page < 0) s_page = 0;
    if (s_page >= pages) s_page = pages - 1;
    if (s_cursor >= visible_count()) s_cursor = visible_count() - 1;
    if (s_cursor < 0) s_cursor = 0;

    for (int r = 0; r < TODO_PAGE_SIZE; r++) sync_row(r);
    sync_separators();
    render_page_indicator();
    sync_nav();
    sync_cursor();
}

static void build_header(void)
{
    lv_obj_t *green = panel(s_scr, X0, HEADER_Y, HEADER_GREEN_W, HEADER_H,
                            C_GREEN, C_GREEN, 0);
    lv_obj_t *ttl_en = make_image(green, &todo_text_ttl_en, 0, 0, 0x000000);
    lv_obj_t *ttl_zh = make_image(green, &todo_text_ttl_zh, 0, 0, 0x000000);
    lv_obj_align(ttl_zh, LV_ALIGN_TOP_MID, 0, 1);
    lv_obj_align(ttl_en, LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_t *internal = panel(s_scr, INTERNAL_X, HEADER_Y, INTERNAL_W, HEADER_H,
                               C_BG, C_YELLOW, 1);
    lv_obj_t *img = lv_image_create(internal);
    lv_image_set_src(img, &todo_img_internal);
    lv_obj_set_pos(img, 1, 1);
}

static void build_status(void)
{
    panel(s_scr, X0, STATUS_Y, PANEL_W, STATUS_H, C_BG, C_YELLOW, 1);
    make_image(s_scr, &todo_text_status_zh, STATUS_LABEL_X, STATUS_Y + 4, C_YELLOW);
    make_image(s_scr, &todo_text_status_en, STATUS_LABEL_X, STATUS_Y + 31, C_YELLOW);

    s_prog_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_prog_dsc.header.cf = LV_COLOR_FORMAT_A8;
    s_prog_dsc.header.flags = 0;
    s_prog_dsc.header.w = PROG_W;
    s_prog_dsc.header.h = PROG_H;
    s_prog_dsc.header.stride = PROG_W;
    s_prog_dsc.data_size = sizeof(s_prog_buf);
    s_prog_dsc.data = s_prog_buf;
    s_prog_img = make_image(s_scr, &s_prog_dsc,
                            X0 + PANEL_W - 6 - PROG_W,
                            STATUS_Y + (STATUS_H - PROG_H) / 2,
                            C_YELLOW);
}

static void build_rows(void)
{
    panel(s_scr, X0, TASKS_Y, PANEL_W, TASKS_H, C_BG, C_YELLOW, 1);
    for (int r = 0; r < TODO_PAGE_SIZE; r++) {
        int y = ROW0_Y + r * ROW_H;
        s_frame[r] = panel(s_scr, X0 + 4, y + 2, PANEL_W - 8, ROW_H - 4,
                           C_BG, C_RED, 1);
        hide(s_frame[r]);
        s_cursor_bar[r] = panel(s_scr, CURSOR_X, y + 8, 4, CHECK_SIZE,
                                C_YELLOW, C_YELLOW, 0);
        hide(s_cursor_bar[r]);
        s_check[r] = panel(s_scr, CHECK_X, y + 8, CHECK_SIZE, CHECK_SIZE,
                           C_BG, C_YELLOW, 1);
        lv_obj_set_style_bg_opa(s_check[r], LV_OPA_TRANSP, 0);
        s_urgent[r] = lv_image_create(s_scr);
        lv_image_set_src(s_urgent[r], &todo_img_urgent);
        lv_obj_set_pos(s_urgent[r], CHECK_X, y + 8);
        hide(s_urgent[r]);

        s_en_img[r] = make_image(s_scr, &todo_text_t0_en, LABEL_X, y + 20, C_YELLOW);
        s_zh_img[r] = make_image(s_scr, &todo_text_t0_zh, LABEL_X, y + 1, C_YELLOW);
        s_title_lbl[r] = make_label(s_scr, LABEL_X, y + 0, 176, &lv_font_montserrat_14);
        s_note_lbl[r] = make_label(s_scr, LABEL_X, y + 16, 176, &lv_font_montserrat_14);
        hide(s_title_lbl[r]);
        hide(s_note_lbl[r]);

        if (r > 0) {
            s_sep[r - 1] = panel(s_scr, X0 + 8, y - 1, PANEL_W - 16, 1,
                                 C_YELLOW, C_YELLOW, 0);
        }
    }
}

static void build_nav(void)
{
    s_prev_box = panel(s_scr, PREV_X, NAV_Y, NAV_W, NAV_H, C_BG, C_YELLOW, 1);
    s_next_box = panel(s_scr, NEXT_X, NAV_Y, NAV_W, NAV_H, C_BG, C_YELLOW, 1);
    lv_obj_t *page_box = panel(s_scr, PAGE_X, NAV_Y, NAV_W, NAV_H, C_BG, C_YELLOW, 1);
    s_prev_txt = make_image(s_prev_box, &todo_text_btn_prev, 0, 0, C_YELLOW);
    s_next_txt = make_image(s_next_box, &todo_text_btn_next, 0, 0, C_YELLOW);
    lv_obj_center(s_prev_txt);
    lv_obj_center(s_next_txt);
    lv_obj_t *page_label = make_image(page_box, &todo_text_page, 0, 0, C_YELLOW);
    lv_obj_set_pos(page_label, (NAV_W - 30) / 2, 3);

    s_page_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_page_dsc.header.cf = LV_COLOR_FORMAT_A8;
    s_page_dsc.header.flags = 0;
    s_page_dsc.header.w = PAGE_W;
    s_page_dsc.header.h = PAGE_H;
    s_page_dsc.header.stride = PAGE_W;
    s_page_dsc.data_size = sizeof(s_page_buf);
    s_page_dsc.data = s_page_buf;
    s_page_img = make_image(page_box, &s_page_dsc, 0, 0, C_YELLOW);
    lv_obj_set_pos(s_page_img, (NAV_W - PAGE_W) / 2, 21);
}

static void cursor_move(int dir)
{
    int vis = visible_count();
    int next = s_cursor + dir;
    if (next < 0) next = 0;
    if (next >= vis) next = vis - 1;
    if (next != s_cursor) {
        s_cursor = next;
        sync_cursor();
    }
}

static void flip_page(int dir)
{
    int pages = todo_model_page_count(&s_model);
    int next = s_page + dir;
    if (next < 0 || next >= pages) return;
    s_page = next;
    s_cursor = 0;
    apply_page();
}

static void toggle_current(void)
{
    int g = global_of(s_cursor);
    if (g < 0) return;
    const todo_item_t *item = todo_model_item(&s_model, g);
    todo_state_t state = todo_model_toggle(&s_model, g);
    sync_row(s_cursor);
    render_progress();
    if (s_mutation_cb && item && item->id && item->id[0]) {
        s_mutation_cb(item->id, state, s_mutation_user);
    }
    ESP_LOGI(TAG, "toggle task %d (%s) -> state %d (done %d/%d)",
             g, item ? item->id : "?", (int)state,
             todo_model_done_count(&s_model), todo_model_total(&s_model));
}

void todo_app_start(void)
{
    reset_defaults();
    todo_model_init(&s_model, s_items, s_states, s_item_count);
    s_page = 0;
    s_cursor = 0;
    s_server_version = 0;

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    build_header();
    build_status();
    build_rows();
    build_nav();
    panel(s_scr, X0, BAR_Y, PANEL_W, BAR_H, C_GREEN, C_GREEN, 0);

    apply_page();
    render_progress();
    lv_screen_load(s_scr);
    ESP_LOGI(TAG, "todo screen ready: %d tasks, %d pages",
             todo_model_total(&s_model), todo_model_page_count(&s_model));
}

void todo_app_handle_button(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr) return;
    switch (btn) {
    case BSP_BTN_UP:
        if (ev == BSP_BTN_PRESS) cursor_move(-1);
        else if (ev == BSP_BTN_LONG) flip_page(-1);
        break;
    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_PRESS) cursor_move(1);
        else if (ev == BSP_BTN_LONG) flip_page(1);
        break;
    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK) toggle_current();
        break;
    default:
        break;
    }
}

void todo_app_set_mutation_callback(todo_app_mutation_cb_t cb, void *user)
{
    s_mutation_cb = cb;
    s_mutation_user = user;
}

void todo_app_apply_remote_tasks(const todo_app_remote_task_t *tasks, int count,
                                 int server_version)
{
    if (!tasks || count < 0) return;
    int new_count = 0;
    for (int i = 0; i < count && new_count < TODO_APP_MAX_TASKS; i++) {
        if (tasks[i].deleted || !tasks[i].id[0] || !tasks[i].title[0]) continue;
        todo_state_t state = tasks[i].state;
        if (tasks[i].urgent && state != TODO_STATE_DONE) state = TODO_STATE_URGENT;
        set_item(new_count, tasks[i].id, tasks[i].title, tasks[i].notes, state);
        new_count++;
    }
    s_item_count = new_count;
    todo_model_init(&s_model, s_items, s_states, s_item_count);
    s_server_version = server_version;
    apply_page();
    render_progress();
    ESP_LOGI(TAG, "remote tasks applied: %d active, server version %d",
             s_item_count, s_server_version);
}

void todo_app_get_report(int *task_total, int *task_done, int *server_version)
{
    if (task_total) *task_total = todo_model_total(&s_model);
    if (task_done) *task_done = todo_model_done_count(&s_model);
    if (server_version) *server_version = s_server_version;
}
