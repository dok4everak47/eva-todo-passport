#!/usr/bin/env python3
"""USB 串口 Todo 管理网页端(纯标准库,无需 pip 安装任何东西)。

     浏览器  <--HTTP-->  本服务  <--USB 串口-->  设备

为什么需要这个桥:ESP32-C3 的 USB 只有 USB-Serial/JTAG 控制器(没有 USB-OTG),
无法给浏览器提供网络接口,所以网页必须由本机服务代理到串口。

设备侧协议(main/todo_usb.c):每条消息一行,行首 '#' + JSON。
  发: {"cmd":"ping"} / {"cmd":"list"} / {"cmd":"set","tasks":[...]}
  收: {"ok":true,"cmd":"list",...} / {"event":"changed",...}

可选镜像:改动同时写入本机 Go 服务(server-go),使设备经 Wi-Fi 同步时
不会与 USB 端的清单分叉。关闭用 --no-mirror。
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import select
import sys
import termios
import threading
import time
import uuid
import tty
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.0.0"
DEFAULT_HTTP_PORT = 8899
# 空闲时自动向设备催一次清单的间隔(秒):页面正在轮询时用 FAST(准实时补拉,
# 兜住设备上报偶发丢行);页面没打开时退回 SLOW,避免无人看时也空转。
IDLE_REFRESH_FAST = 2.0
IDLE_REFRESH_SLOW = 10.0
PAGE_IDLE_WINDOW = 10.0
MAX_TASKS = 12

def new_task_id() -> str:
    """网页端新建任务的 id:毫秒时间戳 + 随机后缀(避免并发/重试碰撞)。"""
    return f"usb-{int(time.time() * 1000)}-{uuid.uuid4().hex[:6]}"
RECENT_RIDS: dict[str, tuple[float, str]] = {}   # rid -> (时间, 任务id),防重复提交
TITLE_MAX = 63
NOTES_MAX = 79
LINE_MAX = 4000
GO_SERVER = "http://127.0.0.1:8080/api/v1"
ENV_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "server-go", ".env")


# ---------------------------------------------------------------------------
# 设备串口连接
# ---------------------------------------------------------------------------
class Device:
    def __init__(self, explicit_port: str | None):
        self.explicit_port = explicit_port
        self.port: str | None = None
        self.fd: int | None = None
        self.connected = False
        self.firmware = ""
        self.total = 0
        self.done = 0
        self.server_version = 0
        self.last_seen = 0.0
        self.last_page = 0.0
        self.last_error = ""
        self.tasks: list[dict] = []
        self._buf = bytearray()
        self.synced = False        # 是否已从设备读到过清单(未读到前禁止写入,防止整表替换把任务抹掉)
        self._wlock = threading.Lock()
        self._lock = threading.Lock()
        self._stop = False

    # --- 端口发现 ---
    def _find_port(self) -> str | None:
        if self.explicit_port:
            return self.explicit_port if os.path.exists(self.explicit_port) else None
        for pat in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*"):
            hits = sorted(glob.glob(pat))
            if hits:
                return hits[0]
        return None

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "connected": self.connected,
                "port": self.port or "",
                "firmware": self.firmware,
                "total": self.total,
                "done": self.done,
                "serverVersion": self.server_version,
                "lastSeen": self.last_seen,
                "error": self.last_error,
                "tasks": [dict(t) for t in self.tasks],
            }

    # --- 收发 ---
    def send(self, obj: dict) -> bool:
        if self.fd is None:
            return False
        data = ("#" + json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(data) > LINE_MAX:
            self.last_error = "清单过长,未发送(减少任务或缩短文字)"
            return False
        with self._wlock:
            try:
                os.write(self.fd, data)
                return True
            except OSError as exc:
                self.last_error = f"写入串口失败: {exc}"
                self._drop()
                return False

    def _drop(self) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = None
        self.connected = False
        self.port = None
        self.synced = False

    def _open(self, port: str) -> bool:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as exc:
            self.last_error = f"打开 {port} 失败: {exc}"
            return False
        try:
            tty.setraw(fd)
        except termios.error:
            pass
        self.fd = fd
        self.port = port
        self.connected = True
        self.synced = False        # 重连后需重新读到清单才允许写入
        self.last_seen = time.time()
        self.last_error = ""
        self._buf.clear()
        self.send({"cmd": "ping"})
        self.send({"cmd": "list"})
        return True

    def _handle_line(self, raw: bytes) -> None:
        if not raw.startswith(b"#"):
            return                      # 设备日志等,忽略
        try:
            msg = json.loads(raw[1:].decode("utf-8", "replace"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return
        if not isinstance(msg, dict):
            return
        with self._lock:
            self.last_seen = time.time()
            if msg.get("fw"):
                self.firmware = str(msg["fw"])
            if isinstance(msg.get("tasks"), list):
                tasks = []
                for t in msg["tasks"]:
                    if not isinstance(t, dict) or not t.get("id"):
                        continue
                    tasks.append({
                        "id": str(t.get("id", "")),
                        "title": str(t.get("title", "")),
                        "notes": str(t.get("notes", "")),
                        "status": "done" if t.get("status") == "done" else "todo",
                        "urgent": bool(t.get("urgent")),
                    })
                self.tasks = tasks[:MAX_TASKS]
                self.synced = True
                self.total = int(msg.get("total", len(self.tasks)) or 0)
                self.done = int(msg.get("done", 0) or 0)
                self.server_version = int(msg.get("serverVersion", self.server_version) or 0)
            if msg.get("event") == "changed" and msg.get("tasks") is not None:
                self.last_error = ""

    def run(self) -> None:
        while not self._stop:
            if self.fd is None:
                port = self._find_port()
                if not port:
                    time.sleep(1.0)
                    continue
                if not self._open(port):
                    time.sleep(1.5)
                    continue
            try:
                ready, _, _ = select.select([self.fd], [], [], 0.2)
                if ready:
                    chunk = os.read(self.fd, 4096)
                    if not chunk:
                        self._drop()
                        continue
                    self._buf.extend(chunk)
                    while b"\n" in self._buf:
                        line, _, rest = self._buf.partition(b"\n")
                        self._buf = bytearray(rest)
                        self._handle_line(line.rstrip(b"\r"))
                    if len(self._buf) > 65536:
                        self._buf.clear()
                else:
                    # 空闲时定期催一次清单:兼作探测与"补拉"(设备上报偶尔会丢行)。
                    # 页面在轮询 /api/state 时按准实时节奏,页面关掉后退回慢节奏。
                    page_active = (time.time() - self.last_page) < PAGE_IDLE_WINDOW
                    interval = IDLE_REFRESH_FAST if page_active else IDLE_REFRESH_SLOW
                    if time.time() - self.last_seen > interval:
                        self.send({"cmd": "list"})
            except OSError as exc:
                self.last_error = f"串口异常: {exc}"
                self._drop()
                time.sleep(1.0)
        self._drop()

    # --- 本地改动 + 下发 ---
    def push_tasks(self, tasks: list[dict]) -> tuple[bool, str]:
        if not self.connected:
            return False, "设备未连接,无法下发"
        if not self.synced:
            return False, "尚未读到设备上的清单,已拒绝写入(请稍候或点\"从设备刷新\")"
        payload = tasks[:MAX_TASKS]
        ok = self.send({"cmd": "set", "tasks": payload})
        if ok:
            with self._lock:
                self.tasks = payload        # 只有确实写进串口后才更新本地视图
            return True, ""
        return False, (self.last_error or "下发失败")


# ---------------------------------------------------------------------------
# 镜像到本机 Go 服务(避免 Wi-Fi 同步与 USB 清单分叉)
# ---------------------------------------------------------------------------
class Mirror:
    def __init__(self, enabled: bool):
        self.enabled = enabled
        self.status = "关闭" if not enabled else "等待"
        self.token = self._read_token()

    @staticmethod
    def _read_token() -> str:
        try:
            with open(ENV_FILE, "r", encoding="utf-8") as fh:
                for line in fh:
                    if line.startswith("ADMIN_TOKEN="):
                        return line.split("=", 1)[1].strip()
        except OSError:
            pass
        return ""

    def _req(self, method: str, path: str, payload: dict | None = None, timeout: float = 2.5) -> dict | None:
        if not self.token:
            return None
        url = GO_SERVER + path
        data = json.dumps(payload).encode("utf-8") if payload is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.token)
        if data:
            req.add_header("Content-Type", "application/json")
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # 本机直连,不走代理
        with opener.open(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def fetch_history(self) -> list[dict]:
        """历史记录:已完成(done)与已归档(archived=已删除)的任务,按完成时间倒序。
        completedAt 只有 /sync 路径会自动打,直接 PATCH 的不打,故回退用 updatedAt。"""
        cur = self._req("GET", "/tasks?includeDeleted=1", timeout=4.0) or {}
        out = []
        for t in (cur.get("tasks") or []):
            st = str(t.get("status") or "")
            completed = t.get("completedAt")
            # 历史 = 当前已完成,或"完成过且已归档"的留存记录。
            # 排除:删除但从未完成的(那是删除记录),以及被"恢复为未完成"的(已完成置空但
            # completedAt 仍残留——服务端直接 PATCH 不会清它,故按状态判断)。
            if not (st == "done" or (st == "archived" and completed)):
                continue
            done_at = completed or t.get("updatedAt")
            out.append({
                "id": t.get("id"),
                "title": str(t.get("title") or ""),
                "notes": str(t.get("notes") or ""),
                "status": st,
                "archived": st == "archived",
                "canRestore": st != "archived",     # 归档在服务端不可撤销,故不给恢复
                "doneAt": str(done_at or t.get("updatedAt") or ""),
            })
        out.sort(key=lambda x: x["doneAt"], reverse=True)
        return out

    def act_history(self, task_id: str, op: str) -> tuple[bool, str]:
        """restore=恢复为未完成;delete=归档(从牌子列表移走,历史保留)。"""
        if not self.token:
            return False, "没有 ADMIN_TOKEN,无法操作服务端"
        path = "/tasks/" + urllib.parse.quote(str(task_id), safe="")
        if op == "restore":
            try:
                cur = self._req("GET", "/tasks?includeDeleted=1", timeout=4.0) or {}
                one = next((t for t in (cur.get("tasks") or []) if t.get("id") == task_id), None)
                if one and str(one.get("status")) == "archived":
                    return False, "已归档的记录在服务端无法撤销(只能留存查看)"
            except Exception:
                pass
        try:
            if op == "restore":
                self._req("PATCH", path, {"status": "todo"})
            elif op == "delete":
                self._req("DELETE", path)
            else:
                return False, "未知操作"
            return True, ""
        except Exception as exc:
            return False, f"服务端不可达({type(exc).__name__})"

    def archive_done(self) -> tuple[int, int, str]:
        """一键归档:把服务端所有"已完成且未归档"的任务标记归档。
        返回 (成功数, 失败数, 错误)。归档会让设备下次同步时把它们移出牌子列表,记录留在历史里。"""
        if not self.token:
            return 0, 0, "没有 ADMIN_TOKEN"
        cur = self._req("GET", "/tasks?includeDeleted=1", timeout=4.0) or {}
        done, fails = 0, 0
        for t in (cur.get("tasks") or []):
            if str(t.get("status")) != "done":
                continue
            try:
                self._req("DELETE", "/tasks/" + urllib.parse.quote(str(t.get("id")), safe=""))
                done += 1
            except Exception:
                fails += 1
        return done, fails, ("" if not fails else f"{fails} 条归档失败")

    def sync(self, tasks: list[dict]) -> None:
        """把当前清单镜像到 Go 服务:/sync 的 upsert 保留我们的 id,缺失的 delete。"""
        if not self.enabled:
            return
        if not self.token:
            self.status = "跳过(无 ADMIN_TOKEN)"
            return
        try:
            cur = self._req("GET", "/tasks") or {}
            remote = {t.get("id"): t for t in (cur.get("tasks") or [])}
            local_ids = {t["id"] for t in tasks}
            # 设备只保留 MAX_TASKS 条:本地清单一旦填满,远端多出来的任务很可能是被设备
            # 截断(设备根本显示不了)而不是用户删除的——此时一律不下发 delete,避免误删服务器数据。
            may_truncate = len(tasks) >= MAX_TASKS
            mutations = []
            skipped = 0
            for t in tasks:
                prev = remote.get(t["id"]) or {}
                task = {
                    "id": t["id"],
                    "title": t["title"],
                    "notes": t["notes"],
                    "status": t["status"],
                    "urgent": bool(t["urgent"]),
                }
                # 服务端 upsert 是「整条替换」:网页端不跟踪的字段(优先级/排序/标签/截止)
                # 要从服务端原记录带回去,否则会被静默清空。
                task["priority"] = int(t.get("priority", prev.get("priority") or 0) or 0)
                if prev.get("sortOrder") is not None:
                    task["sortOrder"] = prev["sortOrder"]
                for k in ("tags", "dueAt"):
                    if prev.get(k):
                        task[k] = prev[k]
                mutations.append({"operation": "upsert", "task": task})
            for rid in remote:
                if rid not in local_ids:
                    if may_truncate:
                        skipped += 1        # 设备显示不了,不代表用户删了
                        continue
                    mutations.append({"operation": "delete", "task": {"id": rid}})
            self._req("POST", "/sync", {"deviceId": "usb-web", "localVersion": 0, "mutations": mutations})
            if skipped:
                self.status = f"已同步({len(mutations)} 项;跳过 {skipped} 项删除:设备已满 {MAX_TASKS} 条)"
            else:
                self.status = f"已同步({len(mutations)} 项)"
        except Exception as exc:          # 镜像失败绝不能影响用户请求
            self.status = f"跳过({type(exc).__name__})"


# ---------------------------------------------------------------------------
# 网页
# ---------------------------------------------------------------------------
PAGE = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>USB TODO</title>
<style>
:root{--bg:#000;--g:#95ef5e;--y:#ffdc00;--r:#ff3232;--dim:#8a8a8a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--y);
font:15px/1.45 Arial,"Microsoft YaHei",sans-serif;padding:16px;max-width:960px;margin:auto}
.brand{background:var(--g);color:#000;font-weight:900;font-size:24px;padding:12px 14px;display:flex;justify-content:space-between;align-items:baseline}
.brand small{font-size:12px;font-weight:700;letter-spacing:2px}
.bar{border:2px solid var(--y);padding:10px;margin-top:12px;display:flex;flex-wrap:wrap;gap:14px;font-size:13px;color:var(--dim)}
.bar b{color:var(--y);font-weight:700}
.ok{color:var(--g)!important}.bad{color:var(--r)!important}
.panel{border:2px solid var(--y);padding:12px;margin-top:12px}
.panel h2{margin:0 0 10px;font-size:15px;letter-spacing:1px;color:var(--g)}
label{display:block;margin:8px 0 3px;font-size:12px;color:var(--dim)}
input,textarea,select{width:100%;background:#050505;color:var(--y);border:1px solid var(--y);padding:8px;border-radius:0;font:inherit}
textarea{height:52px;resize:vertical}
button{background:#050505;color:var(--y);border:1px solid var(--y);padding:8px 12px;font-weight:900;cursor:pointer;border-radius:0}
button:hover{background:var(--y);color:#000}
button.danger{border-color:var(--r);color:var(--r)}button.danger:hover{background:var(--r);color:#000}
button.go{border-color:var(--g);color:var(--g)}button.go:hover{background:var(--g);color:#000}
.row{display:flex;gap:8px;align-items:center;margin-top:8px}
.task{border:1px solid var(--y);padding:10px;margin:8px 0;display:grid;grid-template-columns:1fr auto;gap:10px}
.task.done{color:var(--g);border-color:var(--g)}
.task.urgent{color:var(--r);border-color:var(--r)}
.t-title{font-weight:900}.t-notes{font-size:13px;color:var(--dim)}
.task.done .t-notes,.task.urgent .t-notes{color:inherit;opacity:.75}
.meta{font-size:11px;color:var(--dim);margin-top:4px;letter-spacing:.5px}
.empty{color:var(--dim);padding:8px 0}
.msg{min-height:18px;font-size:12px;margin-top:8px;color:var(--dim)}
.cnt{font-size:11px;color:var(--dim);margin-left:8px;font-weight:400}
.cnt.bad{color:var(--r)}
.msg.bad{color:var(--r)}
.bar b.bad{color:var(--r)}
.ed{display:grid;gap:6px;width:100%}
</style></head><body>
<div class="brand"><span>任务列表 <small>TASK LIST</small></span><small>USB / SERIAL</small></div>
<div class="bar">
  <span>串口: <b id="s-port">-</b></span>
  <span>链路: <b id="s-link">-</b></span>
  <span>固件: <b id="s-fw">-</b></span>
  <span>进度: <b id="s-prog">-</b></span>
  <span>镜像: <b id="s-mirror">-</b></span>
  <span>上限: <b id="s-cap">-</b></span>
  <span>更新: <b id="s-seen">-</b></span>
</div>
<div class="panel">
  <h2>新增任务 / ADD</h2>
  <label>主标题<span class="cnt" id="n-title-info"></span></label>
  <input id="n-title" maxlength="63" placeholder="SHORT TITLE">
  <label>副标题 / 中文详情<span class="cnt" id="n-notes-info"></span></label>
  <textarea id="n-notes" maxlength="79" placeholder="中文详情"></textarea>
  <div class="cnt" style="margin:6px 0 0">设备屏幕一行只有 176px:主标题约 12 个汉字、副标题约 14 个汉字;超出部分会被截断。</div>
  <div class="row"><label style="margin:0"><input type="checkbox" id="n-urgent" style="width:auto"> 紧急(红色警示)</label>
  <button id="add" class="go">ADD TASK</button></div>
  <div class="msg" id="msg"></div>
</div>
<div class="panel">
  <h2>任务 / TASKS</h2>
  <button id="refresh">从设备刷新</button>
  <div id="list"></div>
</div>
<div class="panel">
  <h2>历史记录 / HISTORY <span class="cnt" id="h-count"></span></h2>
  <div class="row"><button id="h-refresh">刷新历史</button>
  <button id="h-archive-done" disabled>一键归档已完成</button>
  <input id="h-search" placeholder="搜索已完成的记录" style="flex:1;min-width:120px"></div>
  <div class="cnt" style="margin:6px 0 0">勾选完成会自动记入这里(任务仍留在牌子列表里)。「恢复为未完成」改回未完成;「归档」把它从牌子列表移走、记录留在这里。牌子本身没有时钟,所以牌子上只按完成先后排序,带时间戳的记录以服务端为准。</div>
  <div id="hlist"></div>
</div>
<div class="panel">
  <h2>导入 JSON / IMPORT</h2>
  <div class="row"><input type="file" id="imp-file" accept=".json,application/json" style="flex:1;min-width:150px">
  <button id="imp-preview">预览</button></div>
  <div class="msg" id="imp-msg"></div>
  <button id="imp-go" class="go" style="display:none">确认导入</button>
  <details style="margin-top:10px"><summary class="cnt" style="cursor:pointer">格式说明 / 示例（点击展开）</summary>
  <pre class="cnt" style="white-space:pre-wrap;margin:6px 0 0">{
  "format": "eva-todo/import",     // 可省略
  "version": 1,                    // 可省略
  "mode": "merge",                 // merge(默认)=追加/按 id 更新;replace=替换整份清单
  "tasks": [
    { "title": "巡检机房", "notes": "九点前完成", "status": "todo", "urgent": false, "priority": 2 },
    { "title": "续签证书", "status": "done", "id": "cert-2026" }
  ]
}
顶层也可以直接是数组: [ {...}, {...} ]
字段: title 必填(≤63 字节) / notes 可选(≤79 字节) / status: todo 或 done /
      urgent: true 时牌子上变红 / priority: 0-3(仅服务端记着,牌子不显示) /
      id: 可选;给了就按 id 更新,同一文件重复导入不会产生重复任务</pre></details>
</div>
<script>
const $=id=>document.getElementById(id);let state={tasks:[]},editing=null,lastSig='';
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function say(t,bad){const m=$('msg');m.textContent=t;m.className='msg'+(bad?' bad':'')}
async function api(path,opt={}){const r=await fetch(path,{headers:{'content-type':'application/json'},...opt});
 const j=await r.json().catch(()=>({}));if(!r.ok||j.ok===false)throw Error(j.error||r.statusText);return j}
function fmt(ts){return ts?new Date(ts*1000).toLocaleTimeString('zh-CN',{hour12:false}):'-'}
function fmt2(s){if(!s)return '-';const d=new Date(s);if(isNaN(d))return '-';
 const p=n=>String(n).padStart(2,'0');
 return `${p(d.getMonth()+1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;}
function render(){const t=state.tasks||[];
 const sig=JSON.stringify(t)+'|'+editing;if(sig===lastSig)return;lastSig=sig;   // 关键:避免定时轮询抹掉正在输入的内容
 $('list').innerHTML=t.length?t.map((x,i)=>{
 const cls=x.status==='done'?'done':(x.urgent?'urgent':'');
 if(editing===x.id){return `<div class="task ${cls}"><div class="ed">
   <label style="margin:0">主标题<span class="cnt" id="e-t-info"></span></label>
   <input id="e-t" value="${esc(x.title)}" maxlength="63">
   <label style="margin:0">副标题<span class="cnt" id="e-n-info"></span></label>
   <textarea id="e-n" maxlength="79">${esc(x.notes)}</textarea>
   <label style="margin:0"><input type="checkbox" id="e-u" ${x.urgent?'checked':''} style="width:auto"> 紧急</label>
   <div class="row"><button class="go" data-ed-save="${esc(x.id)}">保存</button><button data-ed-cancel="1">取消</button></div></div></div>`}
 return `<div class="task ${cls}"><div><div class="t-title">${esc(x.title)}</div>
   <div class="t-notes">${esc(x.notes)}</div><div class="meta">#${i+1} · ${x.status==='done'?'已完成':(x.urgent?'紧急':'未完成')}</div></div>
   <div style="align-self:start;display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end">
   <button data-act="toggle" data-id="${esc(x.id)}">${x.status==='done'?'重开':'完成'}</button>
   <button data-act="edit" data-id="${esc(x.id)}">编辑</button>
   <button class="danger" data-act="del" data-id="${esc(x.id)}">删除</button></div></div>`}).join(''):'<div class="empty">设备上没有任务</div>'}
const FONT={title:{wide:14,narrow:7},notes:{wide:12,narrow:6}},LABEL_W=176;
function textPx(s,kind){let px=0;for(const ch of String(s||''))px+=(ch.codePointAt(0)>0x2e80)?FONT[kind].wide:FONT[kind].narrow;return px}
function updateCnt(inputId,infoId,kind){const el=$(inputId),info=$(infoId);if(!el||!info)return;
 const px=textPx(el.value,kind),pct=Math.round(px/LABEL_W*100),fit=Math.floor(LABEL_W/FONT[kind].wide);
 info.textContent=`屏幕宽度 ${pct}% · 单行上限约 ${fit} 个汉字`+(px>LABEL_W?' · 超出部分会被截断':'');
 info.className='cnt'+(px>LABEL_W?' bad':'');}
document.addEventListener('input',e=>{const id=e.target.id;
 if(id==='n-title')updateCnt('n-title','n-title-info','title');
 else if(id==='n-notes')updateCnt('n-notes','n-notes-info','notes');
 else if(id==='e-t')updateCnt('e-t','e-t-info','title');
 else if(id==='e-n')updateCnt('e-n','e-n-info','notes');});
async function poll(){try{const j=await api('/api/state');state=j;
 $('s-port').textContent=j.port||'未检测到';$('s-link').textContent=j.connected?'已连接':'未连接';
 $('s-link').className=j.connected?'ok':'bad';$('s-fw').textContent=j.firmware||'-';
 $('s-prog').textContent=j.total?`${j.done}/${j.total} 已完成`:'-';
 $('s-mirror').textContent=j.mirror||'-';
 $('s-cap').textContent=(j.tasks?j.tasks.length:0)+'/'+(j.cap||'-')+(j.truncated?' 已满':'');
 $('s-cap').className=j.truncated?'bad':'';
 $('s-seen').textContent=fmt(j.lastSeen);
 if(!editing){render();}                                                       // 编辑中保持 DOM 不动
 refreshEditCnt();
 if(editing&&!state.tasks.some(t=>t.id===editing)){editing=null;lastSig='';}   // 编辑中的任务被删掉了
}catch(e){$('s-link').textContent='服务异常';$('s-link').className='bad'}}
let histTasks=[];
function histRender(){const box=$('hlist');if(!box)return;
 const q=($('h-search').value||'').trim().toLowerCase();
 const rows=histTasks.filter(x=>!q||((x.title+' '+x.notes).toLowerCase().includes(q)));
 box.innerHTML=rows.length?rows.map(x=>`<div class="task ${x.archived?'':'done'}"><div>
   <div class="t-title">${esc(x.title)}</div><div class="t-notes">${esc(x.notes)}</div>
   <div class="meta">${x.archived?'已归档':'已完成'} · ${fmt2(x.doneAt)}</div></div>
   <div style="align-self:start;display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end">
   ${x.canRestore?`<button data-hist="restore" data-id="${esc(x.id)}">恢复为未完成</button>
   <button class="danger" data-hist="delete" data-id="${esc(x.id)}">归档(移出牌子)</button>`:''}</div></div>`).join('')
 :'<div class="empty">没有匹配的历史记录</div>';}
async function histLoad(){try{const j=await api('/api/history');histTasks=j.tasks||[];
 $('h-count').textContent=(j.ok===false?('不可用'):(j.count||0)+' 条');histRender();histButton();}
 catch(e){$('hlist').innerHTML='<div class="empty">历史加载失败</div>'}}
async function histAct(op,id){try{
 if(op==='delete'&&!confirm('把这条从牌子列表移走并归档?(历史记录里仍可查)'))return;
 const j=await api('/api/history/'+encodeURIComponent(id),{method:'POST',body:JSON.stringify({op})});
 histTasks=j.tasks||[];$('h-count').textContent=(j.count||0)+' 条';histRender();
 say(op==='restore'?'已恢复为未完成':'已归档');await poll();
}catch(e){say(e.message,true)}}
$('h-refresh').onclick=()=>histLoad();
$('h-archive-done').onclick=async()=>{
 const n=histTasks.filter(x=>x.canRestore).length;          // canRestore=未归档(即牌子上还有的已完成)
 if(!n)return say('没有可归档的已完成任务');
 if(!confirm(`把 ${n} 条已完成归档?\n它们会从牌子列表移走、腾出名额,记录保留在历史里。`))return;
 const b=$('h-archive-done');b.disabled=true;
 try{const j=await api('/api/history/batch',{method:'POST',body:JSON.stringify({op:'archive-done'})});
  histTasks=j.tasks||[];histRender();$('h-count').textContent=(j.count||0)+' 条';
  say(`已归档 ${j.archived||0} 条`+(j.error?(' · '+j.error):''), !!j.error);await poll();
 }catch(e){say(e.message,true)}finally{b.disabled=false;histButton();}};
function histButton(){const b=$('h-archive-done');if(!b)return;const n=histTasks.filter(x=>x.canRestore).length;
 b.disabled=(n===0);b.textContent=n?`一键归档已完成 (${n})`:'一键归档已完成';}
$('h-search').oninput=()=>histRender();
let impBuf=null;
function impMsg(html,bad){const m=$('imp-msg');m.className='msg'+(bad?' bad':'');m.innerHTML=html;}
$('imp-preview').onclick=async()=>{
 const f=$('imp-file').files[0];if(!f){impMsg('先选一个 .json 文件',true);return}
 try{
  const j=JSON.parse(await f.text());
  const arr=Array.isArray(j)?j:(j.tasks||[]);
  const mode=(Array.isArray(j)?'merge':String(j.mode||'merge')).toLowerCase();
  if(mode!=='merge'&&mode!=='replace')throw Error('mode 只能是 merge 或 replace');
  if(!Array.isArray(arr)||!arr.length)throw Error('没有 tasks 数组或为空');
  const cur=state.tasks||[],ids=new Set(cur.map(t=>t.id));
  const upd=arr.filter(x=>x&&x.id&&ids.has(x.id)).length,add=arr.length-upd;
  const total=(mode==='replace'?arr.length:cur.length+add);
  impBuf={mode,tasks:arr};
  impMsg(`文件 <b>${esc(f.name)}</b> · 模式 <b>${mode}</b> · ${arr.length} 条`+
   `（新增 ${add}${upd?` / 按 id 更新 ${upd}`:''}）→ 导入后 <b>${total}</b> 条`+
   (total>12?' <span class="bad">超过上限 12,会被拒绝</span>':'')+
   (()=>{const ov=arr.filter(x=>x&&textPx(x.title||'','title')>176).length,
     on=arr.filter(x=>x&&textPx(x.notes||x.desc||x.detail||'','notes')>176).length;
     return (ov||on)?` <span class="bad">牌子上放不下:主标题超宽 ${ov} 条 / 副标题超宽 ${on} 条(会以 … 截断,全文可在详情页看)</span>`:''})());
  $('imp-go').style.display='';$('imp-go').disabled=(total>12);
  $('imp-go').textContent=`确认导入 ${arr.length} 条`;
 }catch(e){impBuf=null;$('imp-go').style.display='none';impMsg('JSON 有问题:'+esc(e.message),true)}
};
$('imp-go').onclick=async()=>{if(!impBuf)return;
 $('imp-go').disabled=true;
 try{const j=await api('/api/import',{method:'POST',body:JSON.stringify(impBuf)});
  impMsg(`已导入:模式 ${j.mode} · 新增 ${j.added} · 更新 ${j.updated}`+
   (j.skipped?` · 跳过 ${j.skipped}(${(j.skipped_reasons||[]).join(';')})`:'')+
   ((j.warnings&&j.warnings.length)?` <span class="bad">· 注意 ${j.warnings.join(';')}</span>`:'')+
   ` · 共 ${j.total} 条 · 镜像 ${esc(j.mirror||'-')}`);
  impBuf=null;$('imp-go').style.display='none';$('imp-file').value='';await poll();histLoad();
 }catch(e){impMsg(esc(e.message),true)}finally{$('imp-go').disabled=false}
};
document.addEventListener('click',e=>{const b=e.target.closest('button[data-hist]');
 if(b)histAct(b.dataset.hist,b.dataset.id)});
async function act(act,id,payload){try{
 if(act==='toggle'){const t=state.tasks.find(x=>x.id===id);await api('/api/tasks/'+encodeURIComponent(id),{method:'PATCH',body:JSON.stringify({status:t.status==='done'?'todo':'done'})})}
 if(act==='toggle'||act==='del')setTimeout(histLoad,800);      // 完成/删除会进历史
 if(act==='del'&&confirm('删除这条任务?'))await api('/api/tasks/'+encodeURIComponent(id),{method:'DELETE'})
 if(act==='edit'){editing=id;render();return}
 if(act==='ed-save'){await api('/api/tasks/'+encodeURIComponent(id),{method:'PATCH',body:JSON.stringify({title:$('e-t').value,notes:$('e-n').value,urgent:$('e-u').checked})});editing=null;say('已保存')}
 if(act==='ed-cancel'){editing=null}
 await poll()}catch(e){say(e.message,true)}}
$('add').onclick=async()=>{if(!$('n-title').value.trim())return say('主标题不能为空',true);
 const rid=(self.crypto&&crypto.randomUUID)?crypto.randomUUID():String(Date.now())+Math.random();
 $('add').disabled=true;                                   // 防双击重复建任务
 try{await api('/api/tasks',{method:'POST',body:JSON.stringify({title:$('n-title').value,notes:$('n-notes').value,urgent:$('n-urgent').checked,rid})});
 $('n-title').value='';$('n-notes').value='';$('n-urgent').checked=false;say('已下发到设备');await poll()}
 catch(e){say(e.message,true)}finally{$('add').disabled=false}};
$('refresh').onclick=async()=>{try{await api('/api/refresh',{method:'POST'});say('已请求设备清单');await poll()}catch(e){say(e.message,true)}};
function refreshEditCnt(){if(editing){updateCnt('e-t','e-t-info','title');updateCnt('e-n','e-n-info','notes');}}
document.addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;
 if(b.dataset.act)return act(b.dataset.act,b.dataset.id);
 if(b.dataset.edSave)return act('ed-save',b.dataset.edSave);
 if(b.dataset.edCancel)return act('ed-cancel')});
updateCnt('n-title','n-title-info','title');updateCnt('n-notes','n-notes-info','notes');
poll();setInterval(poll,1500);histLoad();setInterval(histLoad,30000);
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    device: Device
    mirror: Mirror
    server_version = f"usb-todo/{VERSION}"

    def log_message(self, fmt, *args):     # 保持安静
        return

    # --- 工具 ---
    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        try:
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n) if n else b""
            return json.loads(raw.decode("utf-8")) if raw else {}
        except (ValueError, json.JSONDecodeError):
            return {}

    def _import(self, data):
        """导入 JSON。格式:{format,version,mode,tasks} 或裸数组(见 README /sample-import.json)。
        mode=merge(默认):保留现有,tasks 追加;带 id 且已存在则按 id 更新。
        mode=replace:整份清单替换为 tasks。"""
        if isinstance(data, list):
            payload = {"tasks": data}
        elif isinstance(data, dict):
            payload = data
        else:
            return self._json({"ok": False, "error": "JSON 顶层必须是对象或数组"}, 400)
        mode = str(payload.get("mode") or "merge").strip().lower()
        if mode not in ("merge", "replace"):
            return self._json({"ok": False, "error": "mode 只能是 merge 或 replace"}, 400)
        raw = payload.get("tasks")
        if not isinstance(raw, list) or not raw:
            return self._json({"ok": False, "error": "tasks 必须是非空数组"}, 400)

        cur = list(self.device.snapshot()["tasks"])
        pos = {t["id"]: i for i, t in enumerate(cur)}
        result = list(cur) if mode == "merge" else []
        added = updated = skipped = 0
        reasons = []
        warnings = []          # 超长被截断等提示(不阻断导入)
        for k, item in enumerate(raw):
            if not isinstance(item, dict):
                skipped += 1; reasons.append(f"第{k + 1}项不是对象"); continue
            raw_title = item.get("title") or item.get("name") or item.get("text")
            title = self._clean(raw_title, TITLE_MAX)
            if not title:
                skipped += 1; reasons.append(f"第{k + 1}项缺标题"); continue
            raw_notes = (item.get("notes") or item.get("desc")
                         or item.get("description") or item.get("detail"))
            notes = self._clean(raw_notes, NOTES_MAX)
            for _lb, _raw, _lim in (("主标题", raw_title, TITLE_MAX), ("副标题", raw_notes, NOTES_MAX)):
                if _raw and len(str(_raw).encode("utf-8")) > _lim:
                    warnings.append(f"第{k + 1}项{_lb}超过 {_lim} 字节,已截断")
            st = str(item.get("status") or item.get("state") or "todo").strip().lower()
            task = {
                "id": str(item.get("id") or "").strip() or new_task_id(),
                "title": title,
                "notes": notes,
                "status": "done" if st in ("done", "completed", "complete", "finished", "已完成") else "todo",
                "urgent": bool(item.get("urgent")),
            }
            if item.get("priority") is not None:
                try:
                    task["priority"] = max(0, min(3, int(item["priority"])))
                except (TypeError, ValueError):
                    pass
            if mode == "merge" and task["id"] in pos:
                result[pos[task["id"]]] = task; updated += 1
            else:
                hit = next((i for i, t in enumerate(result) if t["id"] == task["id"]), None)
                if hit is None:
                    result.append(task); added += 1
                else:
                    result[hit] = task; updated += 1

        if len(result) > MAX_TASKS:
            return self._json({"ok": False, "error": (
                f"导入后会有 {len(result)} 条,超过设备上限 {MAX_TASKS} 条"
                f"(现有 {len(cur)} 条 + 文件 {len(raw)} 条);请先归档/删除,或用 replace 模式")}, 400)

        ok, err = self.device.push_tasks(result)
        if not ok:
            return self._json({"ok": False, "error": err}, 409)
        self.mirror.sync(result)
        return self._json({"ok": True, "mode": mode, "added": added, "updated": updated,
                           "skipped": skipped, "skipped_reasons": reasons[:5],
                           "warnings": warnings[:8],
                           "total": len(result), "mirror": self.mirror.status, "tasks": result})

    def _clean(self, s: str, limit: int) -> str:
        t = re.sub(r"[\r\n\t]+", " ", str(s or "")).strip()
        # 按"字节上限"截断(与固件 title 64B / notes 80B 一致),但绝不切开多字节字符——
        # 否则标题末尾会留下半个汉字(显示为 ).
        if len(t.encode("utf-8")) <= limit:
            return t
        return t.encode("utf-8")[:limit].decode("utf-8", "ignore")

    # --- 路由 ---
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")     # 便于修完 bug 后普通刷新即可生效
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/history":
            try:
                hist = self.mirror.fetch_history()
                self._json({"ok": True, "tasks": hist, "count": len(hist)})
            except Exception as exc:
                self._json({"ok": False, "tasks": [], "error": f"历史需要连上本机服务端({type(exc).__name__})"})
        elif self.path == "/api/state":
            self.device.last_page = time.time()     # 页面在轮询 => 启用准实时补拉
            snap = self.device.snapshot()
            snap["mirror"] = self.mirror.status
            snap["cap"] = MAX_TASKS
            snap["truncated"] = len(snap.get("tasks") or []) >= MAX_TASKS
            self._json(snap)
        else:
            self._json({"error": "not_found"}, 404)

    def do_POST(self):
        print(f"[REQ] {self.command} {self.path} ua={self.headers.get('User-Agent','')[:28]!r} "
              f"peer={self.client_address[0]} len={self.headers.get('Content-Length','-')}", flush=True)
        if self.path == "/api/tasks":
            data = self._body()
            title = self._clean(data.get("title"), TITLE_MAX)
            notes = self._clean(data.get("notes"), NOTES_MAX)
            rid = str(data.get("rid") or "")
            if not title:
                return self._json({"error": "主标题不能为空"}, 400)
            tasks = list(self.device.snapshot()["tasks"])
            if len(tasks) >= MAX_TASKS:
                return self._json({"error": f"最多 {MAX_TASKS} 条,请先删掉一些"}, 400)
            if rid and rid in RECENT_RIDS and time.time() - RECENT_RIDS[rid][0] < 30:
                prev_id = RECENT_RIDS[rid][1]
                if any(t["id"] == prev_id for t in (self.device.snapshot()["tasks"] or [])):
                    return self._json({"ok": True, "dup": True, "id": prev_id,
                                       "tasks": self.device.snapshot()["tasks"], "mirror": self.mirror.status})
                RECENT_RIDS.pop(rid, None)      # 上次没落地(设备离线等):允许重试
            new_id = new_task_id()
            if rid:
                RECENT_RIDS[rid] = (time.time(), new_id)
                for k in [k for k, v in RECENT_RIDS.items() if time.time() - v[0] > 60]:
                    RECENT_RIDS.pop(k, None)
            tasks.append({
                "id": new_id,
                "title": title,
                "notes": notes,
                "status": "todo",
                "urgent": bool(data.get("urgent")),
            })
            return self._push(tasks)
        if self.path == "/api/history/batch":
            data = self._body()
            if str(data.get("op") or "") != "archive-done":
                return self._json({"ok": False, "error": "未知操作"}, 400)
            done, fails, err = self.mirror.archive_done()
            try:
                hist = self.mirror.fetch_history()
            except Exception:
                hist = []
            return self._json({"ok": not err, "archived": done, "error": err,
                               "tasks": hist, "count": len(hist)})
        if self.path.startswith("/api/history/"):
            tid = urllib.parse.unquote(self.path[len("/api/history/"):])
            data = self._body()
            ok, err = self.mirror.act_history(tid, str(data.get("op") or ""))
            if not ok:
                return self._json({"ok": False, "error": err}, 409)
            try:
                hist = self.mirror.fetch_history()
            except Exception:
                hist = []
            return self._json({"ok": True, "tasks": hist, "count": len(hist)})
        if self.path == "/api/import":
            return self._import(self._body())
        if self.path == "/api/refresh":
            self.device.send({"cmd": "list"})
            return self._json({"ok": True})
        self._json({"error": "not_found"}, 404)

    def do_PATCH(self):
        print(f"[REQ] {self.command} {self.path} ua={self.headers.get('User-Agent','')[:28]!r} "
              f"peer={self.client_address[0]} len={self.headers.get('Content-Length','-')}", flush=True)
        m = re.fullmatch(r"/api/tasks/(.+)", self.path)
        if not m:
            return self._json({"error": "not_found"}, 404)
        tid = urllib.parse.unquote(m.group(1))
        data = self._body()
        tasks = list(self.device.snapshot()["tasks"])
        hit = False
        for t in tasks:
            if t["id"] != tid:
                continue
            hit = True
            if "title" in data:
                t["title"] = self._clean(data["title"], TITLE_MAX) or t["title"]
            if "notes" in data:
                t["notes"] = self._clean(data["notes"], NOTES_MAX)
            if "urgent" in data:
                t["urgent"] = bool(data["urgent"])
            if data.get("status") == "done":
                t["status"] = "done"
                t["urgent"] = False
            elif data.get("status") == "todo":
                t["status"] = "todo"
        if not hit:
            return self._json({"error": "任务不存在"}, 404)
        return self._push(tasks)

    def do_DELETE(self):
        print(f"[REQ] {self.command} {self.path} ua={self.headers.get('User-Agent','')[:28]!r} "
              f"peer={self.client_address[0]} len={self.headers.get('Content-Length','-')}", flush=True)
        m = re.fullmatch(r"/api/tasks/(.+)", self.path)
        if not m:
            return self._json({"error": "not_found"}, 404)
        tid = urllib.parse.unquote(m.group(1))
        tasks = [t for t in self.device.snapshot()["tasks"] if t["id"] != tid]
        return self._push(tasks)

    def _push(self, tasks: list[dict]):
        ok, err = self.device.push_tasks(tasks)
        if not ok:
            # 关键安全点:只有确实下发到设备成功后才镜像到 Go 服务。
            # 设备离线时镜像会用一份"凭空拼出来的本地清单"去覆盖服务器(会误删任务)。
            self.mirror.status = "未镜像(设备未连接)"
            return self._json({"ok": False, "error": err}, 409)
        self.mirror.sync(tasks)
        self._json({"ok": True, "tasks": tasks, "mirror": self.mirror.status})


def main() -> int:
    ap = argparse.ArgumentParser(description="USB 串口 Todo 管理网页端")
    ap.add_argument("--http-port", type=int, default=DEFAULT_HTTP_PORT)
    ap.add_argument("--serial", default=None, help="显式指定串口,如 /dev/cu.usbmodem1101")
    ap.add_argument("--no-mirror", action="store_true", help="不镜像到本机 Go 服务")
    ap.add_argument("--no-open", action="store_true", help="不自动打开浏览器")
    args = ap.parse_args()

    device = Device(args.serial)
    mirror = Mirror(not args.no_mirror)
    threading.Thread(target=device.run, daemon=True).start()

    Handler.device = device
    Handler.mirror = mirror
    httpd = ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    url = f"http://127.0.0.1:{args.http_port}/"
    print(f"USB TODO 网页端: {url}   串口: {args.serial or '自动发现'}")
    if not args.no_open:
        threading.Thread(target=lambda: (time.sleep(0.6), os.system(f"open {url}")), daemon=True).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        device._stop = True
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
