// main/main.c
// FoloToy AI Passport(ESP32-C3,240x320)上的 EVA 风格 Todo List。
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"   // 错误日志里要打印 BSP_LCD_* 引脚号
#include "todo_app.h"
#include "todo_sync.h"
#include "todo_usb.h"
#include "esp_log.h"

static const char *TAG = "main";

// 按键回调运行在 button 组件的任务里,操作 LVGL 必须加锁。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    todo_app_handle_button(btn, ev);
    bsp_lvgl_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy EVA Todo List 启动");

    // 本应用无声频/电量计需求,只初始化显示、LVGL 与按键。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    bool button_ok = (bsp_button_init(on_key, NULL) == ESP_OK);

    if (bsp_lvgl_lock(1000)) {
        todo_app_start();
        bsp_lvgl_unlock();
    }
    todo_usb_start();     // USB 串口任务通道(内部自行加 LVGL 锁)
    todo_sync_start();

    ESP_LOGI(TAG, "就绪:Button=%d", button_ok ? 1 : 0);
}
