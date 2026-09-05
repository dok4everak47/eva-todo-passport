# Asset Preparation(资源生成)

界面文字与图标是**开发期预渲染位图**,提交在 `main/todo_text_assets.c`。普通构建
固件**不需要**字体文件;只有当你修改文案 / 字号 / 配色时才需要重跑本流程。

## 依赖

- Python 3 + Pillow
- 主机字体(默认 Windows):
  - 英文:Arial Bold(`C:\Windows\Fonts\arialbd.ttf`)
  - 中文:SimHei 黑体(`C:\Windows\Fonts\simhei.ttf`)
  - 非 Windows / 无上述字体:改 `tools/prepare_todo_text.py` 顶部的 `FALLBACK_EN` /
    `FALLBACK_ZH` 列表指向你机器上可用的字体。

> 字体本身不随固件分发;生成的像素位图你可以自行决定是否分发(见 NOTICE.md)。

## 生成

```powershell
python tools/prepare_todo_text.py
```

输出:`main/todo_text_assets.h` 与 `main/todo_text_assets.c`。

生成器内文案集中在一处:

```python
tasks = [
    ("MORNING BRIEFING", "晨会准备"),
    ...
]
```

注意:任务列表的实际顺序 / 初始状态在 `main/todo_app.c` 的 `s_items[]` 里,两处都改
才能让"位图"与"逻辑"一致。

## 预览

`tools/preview_todo_ui.py` 用与固件**完全相同**的位图与坐标渲染 456x605 的 PNG:

```powershell
python tools/preview_todo_ui.py   # -> docs/preview_todo_page1.png / page2.png
```

刷机前建议先跑一次预览,确认文案、字号与布局。

## 校验

```powershell
python tests/test_todo_text_assets.py
```
