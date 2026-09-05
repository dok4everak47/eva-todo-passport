# Architecture

本项目三层: components/bsp(沿用 FoloToy AI Passport / EVA 播放器参考工程的板级
支持)、main(EVA Todo List 固件应用)与 server(Cloudflare Worker/D1 Todo API)。
固件应用可调用 BSP API,BSP 不依赖应用;云端通过极简 JSON API 与固件/Web/AI 代理同步。

## 启动流程

~~~text
app_main
  -> bsp_display_init()      SPI + ST7789P3 + 背光
  -> bsp_lvgl_init()         LVGL 9 + 20 行单缓冲(局部刷新)
  -> bsp_button_init()       UP/DOWN/OK(单 ADC 分压)
  -> todo_app_start()        创建主界面并载入
  -> todo_sync_start()       Wi-Fi STA + /sync + /events + /report
~~~

本应用不需要音频 / 电量计,因此不初始化 I2C、ES8311、CW2017(相关 BSP 驱动仍会
编译,但不会运行,省 RAM)。

## 渲染架构(为什么省内存)

设备是 ESP32-C3,无 PSRAM。因此:

- 没有运行时 CJK 字体。默认中英文案都是开发期用 PIL 预渲染成 A8 / ARGB8888 位图,
  提交为 main/todo_text_assets.c 里的 lv_image_dsc_t,只占约 43 KiB Flash。
- 服务器动态任务使用 LVGL 内置小字体裁切显示;默认种子任务仍优先使用预渲染位图。
- 5x7 点阵数字用 todo_dotfont.c 画进约 3.8 KiB 的静态 A8 缓冲,再以
  lv_image 单色 recolor 上色;放大倍数由 scale 决定。
- 界面由约 50 个 lv_obj 色块 / 图片组成,只创建一次。之后的状态变化只做:
  - 改样式(复选框空心/实心、文字 recolor 颜色)
  - 换 lv_image_set_src(翻页时行文字)
  - 重画点阵缓冲并 lv_obj_invalidate 对应图片
  LVGL 的 dirty-rectangle 机制自动只重绘变化区域。

## 模块

| 文件 | 职责 |
| --- | --- |
| main/main.c | 硬件启动与入口 |
| main/todo_app.c | LVGL 主界面、按键映射、行/页/进度同步、远程任务套用 |
| main/todo_sync.c | Wi-Fi、HTTP API、长轮询事件、mutation 队列、设备上报 |
| main/todo_model.c | 纯任务数据模型:状态、勾选、完成数、分页(主机可测) |
| main/todo_dotfont.c | 5x7 点阵字体渲染(主机可测) |
| main/todo_text_assets.c | 预渲染文字 / 图标位图(由 tools 生成) |
| components/bsp/* | 显示、LVGL 接入、按键(来自参考工程) |
| server/src/index.js | Cloudflare Worker API + 内嵌 EVA 风格 Web 前台 |
| server/migrations/* | D1 表结构与初始任务种子 |
| skills/eva-todo-control/* | 给其他 AI 代理使用的 Todo API 控制 Skill |

## 任务数据

默认 5 个任务(2 已完成),第 1 页 4 项、第 2 页 1 项:

~~~text
[✓] YAO-SHAN MW TRANS.  巡检尧山微波传输链路      done
[ ] TAILSCALE CERT.     续签 Tailscale 节点证书   pending
[ ] GO API TEST         编写 Go 后端 API 测试脚本 pending
[!] SMART GRID AUD.     审核年度智能配电监控方案 urgent(紧急,红)
[✓] BACKUP RECORDS      备份本地任务清单          done(第 2 页)
~~~

状态在固件侧先存在运行时 uint8_t s_states[]。联网后 /sync 返回的活跃任务全量覆盖
显示缓存(最多 TODO_APP_MAX_TASKS 条),本地 OK 勾选产生 complete/reopen mutation 并稍后上传。
本版本尚未落 NVS 队列,断电后未上传的本地 mutation 会丢失;已同步任务由 D1 作为源数据。

改默认文案 / 预渲染字体:编辑 tools/prepare_todo_text.py 与 main/todo_app.c 的默认任务,
然后跑 python tools/prepare_todo_text.py 和 python tools/preview_todo_ui.py 重新生成资源。

## 输入模型

| 物理输入 | 界面结果 |
| --- | --- |
| UP / DOWN 短按 | 光标上/下移一行(页内) |
| UP 长按 | 上一页(PREV),光标回第一行 |
| DOWN 长按 | 下一页(NEXT),光标回第一行 |
| OK 单击 | 勾选 / 取消当前任务,并排队上传 mutation |

按键回调运行在按键组件任务里,main.c 在调用 todo_app_handle_button 前后加
bsp_lvgl_lock/unlock。回调保持轻量:勾选只是改模型 + 刷两处小区域 + 排队 mutation。

## 云端架构

~~~text
Web UI / AI Skill / ESP32 Badge
        | Authorization: Bearer token
Cloudflare Worker (server/src/index.js)
        | DB binding
Cloudflare D1 (tasks + meta + device_reports)
~~~

- 任务版本: meta.version 每次写入递增,固件用 sinceVersion 判断是否需要刷新。
- 远程下发: /events 是最多 25 秒的轻量长轮询; /sync 返回完整活跃任务列表,让删除/重排更简单。
- 本地上传:固件只上传 complete/reopen mutation,Web/AI 负责新增、编辑、删除复杂字段。
- 设备报告: /report 记录固件版本、本地任务版本、RSSI、任务总数与完成数。
- Wi-Fi 凭据只存在 main/firmware_private.h,不进入服务端数据库。

## 性能与内存预算

- LVGL 内存池:96 KiB(CONFIG_LV_MEM_SIZE_KILOBYTES=96)。
- 显示缓冲:20 行单缓冲(约 9.6 KiB 内部 RAM,DMA 用)。
- 点阵缓冲:进度 116x28 + 页码 38x14 ≈ 3.8 KiB(BSS)。
- 静态位图:约 43 KiB Flash;固件总量远小于 4 MB factory 分区。
- HTTP 同步:8 KiB 静态响应缓冲 + 2 KiB 请求缓冲,避免网络任务栈被大 JSON 压垮。
- 动画:刻意不用。翻页是机械式硬切换;进度数字局部刷新,不做滑动 / 淡入淡出。

## 验证边界

主机测试覆盖纯逻辑(模型勾选/分页计数、点阵测量/渲染)与资源属性。服务端用
node --check 校验 Worker 语法;Cloudflare/D1 端到端需要 wrangler 部署后验证。
颜色、背光、按键分压、Wi-Fi 稳定性和刷新时序仍需真机验证。

