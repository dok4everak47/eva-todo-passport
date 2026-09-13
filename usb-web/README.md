# USB 网页端(usb-web)

浏览器通过 **USB 串口**(不依赖 Wi-Fi)增删查改设备上的待办清单。

```sh
cd usb-web
./start.sh                 # 自动发现 /dev/cu.usbmodem* 并打开 http://127.0.0.1:8899/
./start.sh --no-mirror     # 不镜像到本机 Go 服务
./start.sh --serial /dev/cu.usbmodem1101
```

## 为什么需要这个桥

ESP32-C3 只有 **USB-Serial/JTAG 控制器**,没有 USB-OTG(`soc_caps.h` 里没有
`SOC_USB_OTG_SUPPORTED`),因此无法提供 USB 网络接口(RNDIS/CDC-ECM)。
浏览器不能直连设备,必须由本机服务把 HTTP 转成串口字节流。

```
浏览器 <--HTTP--> usb-web/server.py <--USB 串口--> 设备(main/todo_usb.c)
```

## 设备侧协议(行协议,行首 `#` + JSON)

| 方向 | 消息 |
|---|---|
| 主机 → 设备 | `{"cmd":"ping"}` |
| 主机 → 设备 | `{"cmd":"list"}` |
| 主机 → 设备 | `{"cmd":"set","tasks":[{"id","title","notes","status":"todo\|done","urgent"}]}` |
| 设备 → 主机 | `{"ok":true,"cmd":"list","count","total","done","serverVersion","tasks":[…]}` |
| 设备 → 主机 | `{"event":"changed",…}` 设备侧(按键或 Wi-Fi 同步)变化后主动上报 |
| 设备 → 主机 | `{"ok":false,"error":"bad_json\|bad_tasks\|unknown_cmd"}` |

- 非 `#` 开头的行是设备日志,主机侧忽略。
- `set` 是**整表替换**,不改动 `server_version`,因此不会误触发 Wi-Fi 侧重同步。
- 设备每 1 秒比对清单指纹:任何来源(按键 / Wi-Fi 同步 / 串口)的变化都会落 NVS 并上报 `changed`。
- 任务持久化在 NVS(namespace `todo_tasks`, key `list`),拔 USB、断电都不丢。

## 与 Wi-Fi 同步的关系

Wi-Fi 同步逻辑未被改动。网页端的每次改动在**下发设备成功后**会镜像到本机 Go 服务
(`/api/v1/sync` 的 upsert/delete,保留同一批 id),因此两条链路的清单不会分叉。

⚠️ 安全点:镜像**只在确实下发设备成功后**执行。设备离线时若照常镜像,会用一份
"凭空拼出来的本地清单"覆盖服务器,导致服务器上的任务被误删(开发中真实踩到过)。

## 字段上限

| 项 | 上限 |
|---|---|
| 任务数 | 12(设备侧 `TODO_APP_MAX_TASKS`) |
| 主标题 title | 63 字符(设备 64 字节,留 NUL) |
| 副标题 notes | 79 字符(设备 80 字节) |
| 单行 JSON | 4000 字节(`LINE_MAX`) |

清单在设备上每页 4 行;`title` 显示在主行、`notes` 显示在副行(中文走 12px 中文字体)。
