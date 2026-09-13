// main/todo_usb.h
// USB 串口任务通道:主机(Mac)通过 USB-Serial-JTAG 用 JSON 行协议读写任务清单。
// 之所以需要它:ESP32-C3 的 USB 只有 USB-Serial/JTAG 控制器,没有 USB-OTG,
// 因此不可能给浏览器提供网络接口,只能由主机侧做「浏览器 <-> 串口」的桥。
#pragma once

// 启动 USB 通道:装 USB-Serial-JTAG 驱动、从 NVS 恢复任务清单并上屏、
// 启动命令读取任务(每 1 秒比对清单指纹,变化时落 NVS 并主动上报 changed)。
// 须在 todo_app_start() 之后调用;内部自行加 LVGL 锁,调用方不必持锁。
void todo_usb_start(void);
