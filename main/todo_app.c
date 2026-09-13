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
#include "todo_sync.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(todo_font_cjk_12);
LV_FONT_DECLARE(todo_font_cjk_14);

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
static todo_app_delete_cb_t s_delete_cb;
static void *s_delete_user;

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
static lv_obj_t *s_link_status;
static lv_obj_t *s_settings_scr;
static lv_obj_t *s_settings_info[4];
static lv_obj_t *s_settings_page_label;
static int s_settings_page;
static lv_timer_t *s_status_timer;
static bool s_in_settings;
static bool s_delete_prompt;
static bool s_delete_choice_yes;
static lv_obj_t *s_delete_overlay;
static lv_obj_t *s_delete_choice_no;
static lv_obj_t *s_delete_choice_yes_obj;
static bool s_screen_dimmed;
static int64_t s_last_input_us;
#define TODO_DEFAULT_DIM_SECONDS 60
#define SETTINGS_PAGE_COUNT 4
static char s_settings_host[64];
static char s_settings_path[64];
static uint8_t s_page_buf[PAGE_W * PAGE_H];
static lv_image_dsc_t s_page_dsc;
static lv_obj_t *s_page_img;

static void apply_page(void);
static void render_progress(void);
static void render_settings_page(void);
static void show_delete_prompt(void);
static void close_delete_prompt(bool confirm);
static int cursor_max_row(void);

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

static void render_link_status(void)
{
    if (!s_link_status) return;
    lv_label_set_text(s_link_status,
                      todo_sync_server_connected() ? "CF OK" :
                      (todo_sync_wifi_connected() ? "WIFI" : "OFF"));
    lv_obj_set_style_text_color(s_link_status,
        lv_color_hex(todo_sync_server_connected() ? C_GREEN : C_YELLOW), 0);
}

static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    render_link_status();
    if (s_in_settings && s_settings_info[0]) render_settings_page();
    if (!s_screen_dimmed && s_last_input_us > 0 &&
        esp_timer_get_time() - s_last_input_us >=
            (int64_t)TODO_DEFAULT_DIM_SECONDS * 1000000LL) {
        bsp_display_backlight(0);
        s_screen_dimmed = true;
        ESP_LOGI(TAG, "display backlight off after inactivity");
    }
}

static void render_settings_page(void)
{
    if (!s_settings_info[0]) return;

    const bool wifi = todo_sync_wifi_connected();
    const bool cloud = todo_sync_server_connected();
    const bool ap = todo_sync_provisioning();
    const char *ip = wifi ? todo_sync_ip_address() : (ap ? todo_sync_ap_ip_address() : "0.0.0.0");
    const char *url = todo_sync_api_base_url();

    // Separate the URL authority and path so neither is clipped on the 240px screen.
    const char *authority = strstr(url, "://");
    const char *host = authority ? authority + 3 : url;
    const char *path = strchr(host, '/');
    size_t host_len = path ? (size_t)(path - host) : strlen(host);
    if (host_len >= sizeof(s_settings_host)) host_len = sizeof(s_settings_host) - 1;
    memcpy(s_settings_host, host, host_len);
    s_settings_host[host_len] = '\0';
    snprintf(s_settings_path, sizeof(s_settings_path), "%s", path ? path : "/");

    if (s_settings_page == 0) {
        // Keep values deliberately short: every line must fit the 240px panel.
        lv_label_set_text_fmt(s_settings_info[0], "WIFI  %s", wifi ? "OK" : "--");
        lv_label_set_text_fmt(s_settings_info[1], "IP    %s", ip);
        lv_label_set_text_fmt(s_settings_info[2], "CF    %s", cloud ? "OK" : "--");
        lv_label_set_text_fmt(s_settings_info[3], "WEB   %s:80", ip);
    } else if (s_settings_page == 1) {
        lv_label_set_text_fmt(s_settings_info[0], "HOST  %s", s_settings_host);
        lv_label_set_text_fmt(s_settings_info[1], "PATH  %s", s_settings_path);
        lv_label_set_text_fmt(s_settings_info[2], "PORT  %u", (unsigned)todo_sync_api_port());
        lv_label_set_text_fmt(s_settings_info[3], "MODE  %s", todo_sync_dhcp_enabled() ? "DHCP" : "STATIC");
    } else if (s_settings_page == 2) {
        lv_label_set_text_fmt(s_settings_info[0], "PAIR  %s", ap ? "READY" : "STANDBY");
        lv_label_set_text_fmt(s_settings_info[1], "SSID  %s", ap ? todo_sync_ap_ssid() : "EVA-PASSPORT");
        lv_label_set_text_fmt(s_settings_info[2], "AP IP %s", ap ? todo_sync_ap_ip_address() : "192.168.192.1");
        lv_label_set_text(s_settings_info[3], "WEB   HTTP :80");
    } else {
        lv_label_set_text(s_settings_info[0], "使用说明 / HELP");
        lv_label_set_text(s_settings_info[1], "上下键  移动 / 翻页");
        lv_label_set_text(s_settings_info[2], "OK短按  完成任务");
        lv_label_set_text(s_settings_info[3], "OK长按  删除确认");
    }

    for (int i = 0; i < 4; i++) {
        uint32_t color = C_YELLOW;
        if ((s_settings_page == 0 && i == 0 && wifi) ||
            (s_settings_page == 0 && i == 2 && cloud) ||
            (s_settings_page == 1 && i == 0 && url[0]) ||
            (s_settings_page == 2 && i == 0 && ap) ||
            (s_settings_page == 3 && i == 0)) {
            color = C_GREEN;
        }
        lv_obj_set_style_text_color(s_settings_info[i], lv_color_hex(color), 0);
    }
    lv_label_set_text_fmt(s_settings_page_label, "%d / %d", s_settings_page + 1, SETTINGS_PAGE_COUNT);
}

static void settings_flip(int dir)
{
    int next = s_settings_page + dir;
    if (next < 0) next = SETTINGS_PAGE_COUNT - 1;
    if (next >= SETTINGS_PAGE_COUNT) next = 0;
    s_settings_page = next;
    render_settings_page();
}

static void update_delete_choice(void)
{
    if (!s_delete_choice_no || !s_delete_choice_yes_obj) return;
    lv_obj_set_style_text_color(s_delete_choice_no,
        lv_color_hex(s_delete_choice_yes ? C_YELLOW : C_GREEN), 0);
    lv_obj_set_style_text_color(s_delete_choice_yes_obj,
        lv_color_hex(s_delete_choice_yes ? C_GREEN : C_YELLOW), 0);
}

static void show_delete_prompt(void)
{
    if (s_delete_prompt || global_of(s_cursor) < 0) return;
    s_delete_prompt = true;
    s_delete_choice_yes = false;
    s_delete_overlay = panel(s_scr, 18, 92, 204, 132, C_BG, C_RED, 2);
    lv_obj_t *title = make_label(s_delete_overlay, 12, 10, 180, &todo_font_cjk_14);
    lv_label_set_text(title, "警告 / WARNING");
    lv_obj_set_style_text_color(title, lv_color_hex(C_RED), 0);
    lv_obj_t *body = make_label(s_delete_overlay, 12, 42, 180, &todo_font_cjk_12);
    lv_label_set_text(body, "删除这项任务？");
    lv_obj_set_style_text_color(body, lv_color_hex(C_YELLOW), 0);
    s_delete_choice_no = make_label(s_delete_overlay, 18, 88, 72, &todo_font_cjk_12);
    s_delete_choice_yes_obj = make_label(s_delete_overlay, 112, 88, 72, &todo_font_cjk_12);
    lv_label_set_text(s_delete_choice_no, "[否]");
    lv_label_set_text(s_delete_choice_yes_obj, "[是]");
    update_delete_choice();
}

static void close_delete_prompt(bool confirm)
{
    if (!s_delete_prompt) return;
    int g = global_of(s_cursor);
    char id[TODO_APP_ID_LEN] = { 0 };
    if (confirm && g >= 0) {
        const todo_item_t *item = todo_model_item(&s_model, g);
        if (item && item->id) snprintf(id, sizeof(id), "%s", item->id);
    }
    if (s_delete_overlay) lv_obj_delete(s_delete_overlay);
    s_delete_overlay = NULL;
    s_delete_choice_no = NULL;
    s_delete_choice_yes_obj = NULL;
    s_delete_prompt = false;
    if (confirm && id[0]) {
        for (int i = g; i + 1 < s_item_count; i++) {
            memmove(s_ids[i], s_ids[i + 1], sizeof(s_ids[i]));
            memmove(s_titles[i], s_titles[i + 1], sizeof(s_titles[i]));
            memmove(s_notes[i], s_notes[i + 1], sizeof(s_notes[i]));
            s_states[i] = s_states[i + 1];
        }
        s_item_count--;
        for (int i = 0; i < s_item_count; i++) {
            s_items[i].id = s_ids[i];
            s_items[i].en = s_titles[i];
            s_items[i].zh = s_notes[i];
            s_items[i].state = (todo_state_t)s_states[i];
        }
        todo_model_init(&s_model, s_items, s_states, s_item_count);
        if (s_cursor > cursor_max_row()) {      /* 同一口径:入口行也算可达 */
            s_cursor = visible_count() > 0 ? visible_count() - 1 : 0;
        }
        apply_page();
        render_progress();
        if (s_delete_cb) s_delete_cb(id, s_delete_user);
    }
}

static void render_page_indicator(void)
{
    char text[16];
    snprintf(text, sizeof(text), "%d / %d", s_page + 1,
             todo_model_page_count(&s_model));
    draw_dot_text(s_page_img, s_page_buf, PAGE_W, PAGE_H, PAGE_SCALE, text);
}


/* ---------------------------------------------------------------------------
 * 历史记录:列表最后一页的「历史 / HISTORY」行进入,看所有已完成的任务。
 * - 任务完成后仍留在牌子上(方案:不改变列表与 02/05 计数),历史是它的"记录视图"
 * - 牌子上没有时钟,所以这里不显示完成时间,只按"最近完成在前"排序;
 *   带时间戳的完整历史在网页端看(时间戳以 Go 服务 completedAt 为准)
 * - 入口行需要最后一页有 2 个空行;空间不足时优先保「设置」入口(与改动前一致)
 * ------------------------------------------------------------------------- */
static uint16_t s_done_seq[TODO_APP_MAX_TASKS];   /* 每条的完成顺序(0=未完成) */
static uint16_t s_done_tick;

static int special_row_history(void)
{
    if (s_page != todo_model_page_count(&s_model) - 1) return -1;
    int vc = visible_count();
    return (vc + 2 <= TODO_PAGE_SIZE) ? vc : -1;      /* 需要 2 个空行 */
}

static int special_row_settings(void)
{
    if (s_page != todo_model_page_count(&s_model) - 1) return -1;
    int vc = visible_count();
    if (vc + 2 <= TODO_PAGE_SIZE) return vc + 1;      /* 历史 + 设置都在 */
    return vc;                                        /* 只剩一行:设置(与原行为一致) */
}

/* 光标可停留的最大行号。入口行(历史/设置)也算可达:
   此前 cursor_move/apply_page 各算一份且都只 +1,导致"设置"行被排除、无法移入。 */
static int cursor_max_row(void)
{
    int srow = special_row_settings();
    if (srow >= 0) return srow;                       /* 入口行即最大行 */
    if (s_page == todo_model_page_count(&s_model) - 1) return visible_count();
    return visible_count() - 1;
}

/* 光标所在行的副标题自动来回滚动,其余行仍以 … 截断。
   牌子上副标题一行只有 176px(约 14 个汉字),长副标题只有滚动才能读全;
   只让选中行滚动,其他行保持安静(也不额外消耗重绘)。 */
static void sync_note_scroll(void)
{
    for (int r = 0; r < TODO_PAGE_SIZE; r++) {
        if (!s_note_lbl[r]) continue;
        bool focus = (r == s_cursor) && (global_of(r) >= 0);
        lv_label_set_long_mode(s_note_lbl[r],
                               focus ? LV_LABEL_LONG_MODE_SCROLL : LV_LABEL_LONG_MODE_DOTS);
    }
}

#define HIST_ROWS 6
static lv_obj_t *s_hist_scr;
static lv_obj_t *s_hist_mark[HIST_ROWS];
static lv_obj_t *s_hist_title[HIST_ROWS];
static lv_obj_t *s_hist_note[HIST_ROWS];
static lv_obj_t *s_hist_head;
static lv_obj_t *s_hist_hint;
static bool s_in_history;
static int  s_hist_page;

static int history_collect(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_item_count && i < TODO_APP_MAX_TASKS; i++) {
        if (s_states[i] != TODO_STATE_DONE) continue;
        if (n < max) out[n] = i;
        n++;
    }
    for (int a = 0; a + 1 < n && a < max; a++) {       /* 最近完成的在前(数量很小,冒泡足够) */
        for (int b = a + 1; b < n && b < max; b++) {
            if (s_done_seq[out[b]] > s_done_seq[out[a]]) {
                int t = out[a]; out[a] = out[b]; out[b] = t;
            }
        }
    }
    return n < max ? n : max;
}

static void history_fill(void)
{
    int idx[TODO_APP_MAX_TASKS];
    int n = history_collect(idx, TODO_APP_MAX_TASKS);
    int pages = (n + HIST_ROWS - 1) / HIST_ROWS;
    if (pages < 1) pages = 1;
    if (s_hist_page >= pages) s_hist_page = pages - 1;
    if (s_hist_page < 0) s_hist_page = 0;

    char buf[64];
    if (n == 0) {
        lv_label_set_text(s_hist_head, "暂无已完成任务");
        lv_label_set_text(s_hist_hint, "OK 返回");
    } else {
        snprintf(buf, sizeof(buf), "已完成 %d 条 · 第 %d/%d 页", n, s_hist_page + 1, pages);
        lv_label_set_text(s_hist_head, buf);
        lv_label_set_text(s_hist_hint, pages > 1 ? "上下键 翻页 · OK 返回" : "OK 返回");
    }

    for (int r = 0; r < HIST_ROWS; r++) {
        int k = s_hist_page * HIST_ROWS + r;
        if (k >= n) {
            hide(s_hist_mark[r]); hide(s_hist_title[r]); hide(s_hist_note[r]);
            continue;
        }
        const todo_item_t *it = todo_model_item(&s_model, idx[k]);
        lv_label_set_text(s_hist_title[r], (it && it->en && it->en[0]) ? it->en : "TASK");
        lv_label_set_text(s_hist_note[r], (it && it->zh) ? it->zh : "");
        show(s_hist_mark[r]); show(s_hist_title[r]); show(s_hist_note[r]);
    }
    ESP_LOGI(TAG, "history: %d done, page %d/%d", n, s_hist_page + 1, pages);
}

static void show_history(void)
{
    if (!s_hist_scr) {
        s_hist_scr = lv_obj_create(NULL);
        lv_obj_remove_flag(s_hist_scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_hist_scr, 240, 320);
        lv_obj_set_style_bg_color(s_hist_scr, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(s_hist_scr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_hist_scr, 0, 0);
        lv_obj_set_style_pad_all(s_hist_scr, 0, 0);

        panel(s_hist_scr, 0, 0, 240, 40, C_GREEN, C_GREEN, 0);
        lv_obj_t *hdr = make_label(s_hist_scr, 10, 11, 220, &todo_font_cjk_14);
        lv_label_set_text(hdr, "历史记录 / HISTORY");
        lv_obj_set_style_text_color(hdr, lv_color_hex(C_BG), 0);

        s_hist_head = make_label(s_hist_scr, 10, 48, 220, &todo_font_cjk_12);
        lv_obj_set_style_text_color(s_hist_head, lv_color_hex(C_GREEN), 0);

        for (int r = 0; r < HIST_ROWS; r++) {
            int y = 76 + r * 36;
            s_hist_mark[r]  = panel(s_hist_scr, 12, y + 6, 12, 12, C_GREEN, C_GREEN, 0);
            s_hist_title[r] = make_label(s_hist_scr, 34, y + 0, 196, &todo_font_cjk_14);
            lv_obj_set_height(s_hist_title[r], 18);
            lv_label_set_long_mode(s_hist_title[r], LV_LABEL_LONG_MODE_DOTS);
            s_hist_note[r]  = make_label(s_hist_scr, 34, y + 18, 196, &todo_font_cjk_12);
            lv_obj_set_height(s_hist_note[r], 16);
            lv_label_set_long_mode(s_hist_note[r], LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_color(s_hist_title[r], lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_color(s_hist_note[r], lv_color_hex(C_GREEN), 0);
        }

        s_hist_hint = make_label(s_hist_scr, 10, 296, 220, &todo_font_cjk_12);
        lv_obj_set_style_text_color(s_hist_hint, lv_color_hex(C_GREEN), 0);
    }
    s_hist_page = 0;
    history_fill();
    s_in_history = true;
    lv_screen_load(s_hist_scr);
}

static void sync_cursor(void)
{
    sync_note_scroll();
    for (int r = 0; r < TODO_PAGE_SIZE; r++) {
        bool special_row = (r == special_row_history() || r == special_row_settings());
        if ((!special_row && global_of(r) < 0) || r != s_cursor) hide(s_cursor_bar[r]);
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
        int last_page = todo_model_page_count(&s_model) - 1;
        if (s_page == last_page && row == special_row_history()) {
            show(s_frame[row]);
            hide(s_check[row]);
            hide(s_urgent[row]);
            hide(s_en_img[row]);
            hide(s_zh_img[row]);
            lv_label_set_text(s_title_lbl[row], "历史 / HISTORY");
            lv_label_set_text(s_note_lbl[row], "已完成记录 / DONE LOG");
            lv_obj_set_style_text_color(s_title_lbl[row], lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_color(s_note_lbl[row], lv_color_hex(C_GREEN), 0);
            show(s_title_lbl[row]);
            show(s_note_lbl[row]);
            return;
        }
        if (s_page == last_page && row == special_row_settings()) {
            show(s_frame[row]);
            hide(s_check[row]);
            hide(s_urgent[row]);
            hide(s_en_img[row]);
            hide(s_zh_img[row]);
            lv_label_set_text(s_title_lbl[row], "设置 / SETTINGS");
            lv_label_set_text(s_note_lbl[row], "设备配置 / CONFIG");
            lv_obj_set_style_text_color(s_title_lbl[row], lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_color(s_note_lbl[row], lv_color_hex(C_GREEN), 0);
            show(s_title_lbl[row]);
            show(s_note_lbl[row]);
            return;
        }
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
        lv_label_set_text(s_title_lbl[row], item->en ? item->en : "REMOTE TASK");
        lv_label_set_text(s_note_lbl[row], item->zh ? item->zh : "");
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
    int cursor_limit = cursor_max_row() + 1;   /* 与 cursor_move 同一口径 */
    if (s_cursor >= cursor_limit) s_cursor = cursor_limit - 1;
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
    s_link_status = make_label(s_scr, 166, STATUS_Y + 34, 62, &lv_font_montserrat_14);
    render_link_status();
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
        s_title_lbl[r] = make_label(s_scr, LABEL_X, y + 0, 176, &todo_font_cjk_14);
        /* 高度必须钉成一行(14px 字 line_height=18):DOTS 按标签高度决定截断位置,
           高度自适应时 LVGL 会按换行后的高度算,标签被撑高就会压到副标题行。 */
        lv_obj_set_height(s_title_lbl[r], 18);
        lv_label_set_long_mode(s_title_lbl[r], LV_LABEL_LONG_MODE_DOTS);
        s_note_lbl[r] = make_label(s_scr, LABEL_X, y + 17, 176, &todo_font_cjk_12);
        lv_obj_set_height(s_note_lbl[r], 16);
        lv_label_set_long_mode(s_note_lbl[r], LV_LABEL_LONG_MODE_DOTS);
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
    int vis = cursor_max_row() + 1;            /* vis = 有效行数(最大行号 + 1) */
    int next = s_cursor + dir;
    if (next < 0) {
        if (s_page > 0) {
            s_page--;
            s_cursor = todo_model_visible_on_page(&s_model, s_page) - 1;
            apply_page();
            return;
        }
        /* 到顶:第一条再按 UP -> 绕到最后页的最后一行(循环) */
        s_page = todo_model_page_count(&s_model) - 1;
        s_cursor = cursor_max_row();
        if (s_cursor > TODO_PAGE_SIZE - 1) s_cursor = TODO_PAGE_SIZE - 1;   /* 只落在可见行 */
        apply_page();
        return;
    }
    if (next >= vis) {
        if (s_page < todo_model_page_count(&s_model) - 1) {
            s_page++;
            s_cursor = 0;
            apply_page();
            return;
        }
        /* 到底:最后一行再按 DOWN -> 回第一页第一行(循环) */
        s_page = 0;
        s_cursor = 0;
        apply_page();
        return;
    }
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
    if (special_row_history() >= 0 && s_cursor == special_row_history()) {
        show_history();
        return;
    }
    if (g < 0 && s_cursor == special_row_settings()) {
        s_in_settings = true;
        if (!s_settings_scr) {
            s_settings_scr = lv_obj_create(NULL);
            lv_obj_remove_flag(s_settings_scr, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(s_settings_scr, 240, 320);
            lv_obj_set_style_bg_color(s_settings_scr, lv_color_hex(C_BG), 0);
            lv_obj_set_style_bg_opa(s_settings_scr, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_settings_scr, 0, 0);
            lv_obj_set_style_pad_all(s_settings_scr, 0, 0);
            lv_obj_t *title = lv_label_create(s_settings_scr);
            lv_label_set_text(title, "设置 / SETTINGS");
            lv_obj_set_style_text_color(title, lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_font(title, &todo_font_cjk_14, 0);
            lv_obj_set_pos(title, 12, 8);
            for (int i = 0; i < 4; i++) {
                s_settings_info[i] = lv_label_create(s_settings_scr);
                lv_obj_set_style_text_color(s_settings_info[i], lv_color_hex(C_YELLOW), 0);
                lv_obj_set_style_text_font(s_settings_info[i], &todo_font_cjk_12, 0);
                lv_obj_set_width(s_settings_info[i], 220);
                lv_obj_set_height(s_settings_info[i], 22);
                lv_label_set_long_mode(s_settings_info[i], LV_LABEL_LONG_CLIP);
                lv_obj_set_pos(s_settings_info[i], 12, 56 + i * 32);
            }
            s_settings_page_label = lv_label_create(s_settings_scr);
            lv_obj_set_style_text_color(s_settings_page_label, lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_font(s_settings_page_label, &todo_font_cjk_12, 0);
            lv_obj_set_width(s_settings_page_label, 54);
            lv_label_set_long_mode(s_settings_page_label, LV_LABEL_LONG_CLIP);
            lv_obj_set_pos(s_settings_page_label, 176, 14);
            lv_obj_t *hint = lv_label_create(s_settings_scr);
            lv_label_set_text(hint, "上下键  切页");
            lv_obj_set_style_text_color(hint, lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_font(hint, &todo_font_cjk_12, 0);
            lv_obj_set_pos(hint, 12, 256);
            lv_obj_t *back_hint = lv_label_create(s_settings_scr);
            lv_label_set_text(back_hint, "长按 OK  返回");
            lv_obj_set_style_text_color(back_hint, lv_color_hex(C_GREEN), 0);
            lv_obj_set_style_text_font(back_hint, &todo_font_cjk_12, 0);
            lv_obj_set_pos(back_hint, 12, 280);
        }
        s_settings_page = 0;
        status_timer_cb(NULL);
        lv_screen_load(s_settings_scr);
        return;
    }
    if (g < 0) return;
    const todo_item_t *item = todo_model_item(&s_model, g);
    todo_state_t state = todo_model_toggle(&s_model, g);
    /* 记下完成先后(牌子无时钟);取消勾选则清掉 */
    if (state == TODO_STATE_DONE) {
        if (s_done_seq[g] == 0) s_done_seq[g] = ++s_done_tick;
    } else {
        s_done_seq[g] = 0;
    }
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
    s_last_input_us = esp_timer_get_time();
    s_screen_dimmed = false;

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
    s_status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "todo screen ready: %d tasks, %d pages",
             todo_model_total(&s_model), todo_model_page_count(&s_model));
}


/* ---------------------------------------------------------------------------
 * 任务详情页:列表一行(176px)放不下的长文本,在这里整屏自动换行显示。
 * 入口:列表页双击 OK;出口:详情页单击 OK 或长按 OK 返回。
 * ------------------------------------------------------------------------- */
static lv_obj_t *s_detail_scr;
static lv_obj_t *s_detail_title;
static lv_obj_t *s_detail_notes;
static lv_obj_t *s_detail_meta;
static lv_obj_t *s_detail_hint;
static bool      s_in_detail;

static void detail_fill(int g)
{
    const todo_item_t *item = todo_model_item(&s_model, g);
    if (!item) return;
    todo_state_t st = todo_model_state(&s_model, g);
    uint32_t col = state_color(st);

    lv_label_set_text(s_detail_title, (item->en && item->en[0]) ? item->en : "REMOTE TASK");
    lv_label_set_text(s_detail_notes, (item->zh && item->zh[0]) ? item->zh : "(无副标题)");
    lv_label_set_text(s_detail_meta,
                      st == TODO_STATE_DONE ? "已完成 / DONE" :
                      (st == TODO_STATE_URGENT ? "紧急 / URGENT" : "未完成 / PENDING"));
    lv_obj_set_style_text_color(s_detail_title, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_detail_notes, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_detail_meta, lv_color_hex(col), 0);

    char buf[48];
    snprintf(buf, sizeof(buf), "第 %d/%d 条", g + 1, todo_model_total(&s_model));
    lv_label_set_text(s_detail_hint, buf);
}

static void show_task_detail(void)
{
    int g = global_of(s_cursor);
    if (g < 0) return;                              /* 光标在 SETTINGS 行或空白 */

    if (!s_detail_scr) {
        s_detail_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_detail_scr, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(s_detail_scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_detail_scr, 0, 0);
        lv_obj_set_style_border_width(s_detail_scr, 0, 0);

        panel(s_detail_scr, 0, 0, 240, 40, C_GREEN, C_GREEN, 0);
        lv_obj_t *hdr = make_label(s_detail_scr, 10, 11, 220, &todo_font_cjk_14);
        lv_label_set_text(hdr, "任务详情 / DETAIL");
        lv_obj_set_style_text_color(hdr, lv_color_hex(C_BG), 0);

        s_detail_meta = make_label(s_detail_scr, 10, 48, 220, &todo_font_cjk_12);

        s_detail_title = make_label(s_detail_scr, 10, 76, 220, &todo_font_cjk_14);
        lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_MODE_WRAP);

        panel(s_detail_scr, 10, 176, 220, 1, C_YELLOW, C_YELLOW, 0);

        s_detail_notes = make_label(s_detail_scr, 10, 188, 220, &todo_font_cjk_12);
        lv_label_set_long_mode(s_detail_notes, LV_LABEL_LONG_MODE_WRAP);

        s_detail_hint = make_label(s_detail_scr, 10, 296, 220, &todo_font_cjk_12);
        lv_obj_set_style_text_color(s_detail_hint, lv_color_hex(C_GREEN), 0);
    }
    detail_fill(g);
    s_in_detail = true;
    lv_screen_load(s_detail_scr);
}

void todo_app_handle_button(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_scr) return;
    s_last_input_us = esp_timer_get_time();
    if (s_screen_dimmed) {
        bsp_display_backlight(100);
        s_screen_dimmed = false;
        return;
    }
    if (s_delete_prompt) {
        if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) && ev == BSP_BTN_PRESS) {
            s_delete_choice_yes = !s_delete_choice_yes;
            update_delete_choice();
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            close_delete_prompt(s_delete_choice_yes);
        }
        return;
    }
    if (s_in_history) {
        if (btn == BSP_BTN_UP && ev == BSP_BTN_PRESS) {
            if (s_hist_page > 0) { s_hist_page--; history_fill(); }
        } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_PRESS) {
            s_hist_page++; history_fill();          /* fill 内会夹紧页码 */
        } else if (btn == BSP_BTN_OK &&
                   (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG || ev == BSP_BTN_DOUBLE)) {
            s_in_history = false;
            lv_screen_load(s_scr);
        }
        return;
    }
    if (s_in_detail) {
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            s_in_detail = false;
            lv_screen_load(s_scr);
        }
        return;
    }
    if (s_in_settings) {
        if (btn == BSP_BTN_UP && ev == BSP_BTN_PRESS) {
            settings_flip(-1);
            return;
        }
        if (btn == BSP_BTN_DOWN && ev == BSP_BTN_PRESS) {
            settings_flip(1);
            return;
        }
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            s_in_settings = false;
            lv_screen_load(s_scr);
        }
        return;
    }
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
         else if (ev == BSP_BTN_LONG) show_delete_prompt();
         else if (ev == BSP_BTN_DOUBLE) show_task_detail();   /* 双击看全文 */
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

void todo_app_set_delete_callback(todo_app_delete_cb_t cb, void *user)
{
    s_delete_cb = cb;
    s_delete_user = user;
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
    if (s_in_detail) {   /* 清单变了:退回列表,避免详情页显示已删除/过期条目 */
        s_in_detail = false;
        lv_screen_load(s_scr);
    }
    if (s_in_history) {  /* 同上:历史页内容已变,退回列表 */
        s_in_history = false;
        lv_screen_load(s_scr);
    }
    s_done_tick = 0;                     /* 远程清单里的已完成任务按列表顺序记序 */
    for (int i = 0; i < s_item_count; i++) {
        s_done_seq[i] = (s_states[i] == TODO_STATE_DONE) ? (uint16_t)++s_done_tick : 0;
    }
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

int todo_app_export_tasks(todo_app_remote_task_t *out, int max_count)
{
    if (!out || max_count <= 0) return 0;
    int n = s_item_count < max_count ? s_item_count : max_count;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        copy_trunc(out[i].id, sizeof(out[i].id), s_ids[i]);
        copy_trunc(out[i].title, sizeof(out[i].title), s_titles[i]);
        copy_trunc(out[i].notes, sizeof(out[i].notes), s_notes[i]);
        out[i].state = (todo_state_t)s_states[i];
        out[i].urgent = (out[i].state == TODO_STATE_URGENT);
        out[i].deleted = false;
        out[i].version = s_server_version;
        out[i].sort_order = i;
    }
    return n;
}
