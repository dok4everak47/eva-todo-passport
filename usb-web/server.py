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
                mutations.append({
                    "operation": "upsert",
                    "task": {
                        "id": t["id"],
                        "title": t["title"],
                        "notes": t["notes"],
                        "status": t["status"],
                        "urgent": bool(t["urgent"]),
                    },
                })
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
<script>
const $=id=>document.getElementById(id);let state={tasks:[]},editing=null,lastSig='';
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function say(t,bad){const m=$('msg');m.textContent=t;m.className='msg'+(bad?' bad':'')}
async function api(path,opt={}){const r=await fetch(path,{headers:{'content-type':'application/json'},...opt});
 const j=await r.json().catch(()=>({}));if(!r.ok||j.ok===false)throw Error(j.error||r.statusText);return j}
function fmt(ts){return ts?new Date(ts*1000).toLocaleTimeString('zh-CN',{hour12:false}):'-'}
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
async function act(act,id,payload){try{
 if(act==='toggle'){const t=state.tasks.find(x=>x.id===id);await api('/api/tasks/'+encodeURIComponent(id),{method:'PATCH',body:JSON.stringify({status:t.status==='done'?'todo':'done'})})}
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
poll();setInterval(poll,1500);
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
            new_id = f"usb-{int(time.time() * 1000)}-{uuid.uuid4().hex[:6]}"   # 随机后缀,杜绝碰撞
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
