package main

import (
	"net/http"
	"strings"
)

const skillTemplate = `# EVA Todo API Skill

Use this API to control the EVA Todo badge and its web task list.

## Connection

- Base URL: {{BASE_URL}}
- Admin credential: set EVA_TODO_ADMIN_TOKEN
- Device credential: set EVA_TODO_DEVICE_TOKEN

Send the selected credential as Authorization: Bearer <token>.

## Permissions

- ADMIN_TOKEN: list, create, edit, complete, reopen, delete tasks, and read reports.
- DEVICE_TOKEN: badge /sync, /events, and /report; it cannot change tasks through admin CRUD endpoints.
- Never ask the user to paste a token into a public prompt, commit it, or print it.

## Common calls

GET /health

GET /tasks

POST /tasks with JSON {"title":"SHORT TITLE","notes":"中文详情","priority":1,"urgent":false}

PATCH /tasks/{id} to edit a task.

POST /tasks/{id}/complete or POST /tasks/{id}/reopen.

DELETE /tasks/{id} soft-deletes it. Deleted tasks are physically purged after 24 hours.

After every write, call GET /tasks and report the resulting task id/status. Keep badge-visible titles short; put long Chinese instructions in notes.
`

func skillHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		errorJSON(w, http.StatusMethodNotAllowed, "method_not_allowed", "Method not allowed")
		return
	}
	scheme := r.Header.Get("X-Forwarded-Proto")
	if scheme == "" {
		scheme = "http"
	}
	body := strings.ReplaceAll(skillTemplate, "{{BASE_URL}}", scheme+"://"+r.Host+"/api/v1")
	w.Header().Set("Content-Type", "text/markdown; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="eva-todo-skill.md"`)
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write([]byte(body))
}

func webHandler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" && r.URL.Path != "/index.html" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = w.Write([]byte(webHTML))
}

const webHTML = `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>EVA TODO CONTROL</title>
<style>:root{--bg:#000;--green:#95ef5e;--yellow:#ffdc00;--red:#ff3232}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--yellow);font:16px Arial,"Microsoft YaHei",sans-serif}.wrap{max-width:900px;margin:auto;padding:18px}.top{display:flex;gap:12px}.brand{background:var(--green);color:#000;padding:14px;font-size:26px;font-weight:900;flex:1}.internal{border:2px solid var(--yellow);padding:14px;font-weight:900}.bar,.panel{border:2px solid var(--yellow);padding:12px;margin-top:12px}.bar{display:flex;gap:8px;flex-wrap:wrap}.bar input,.bar button,.panel input,.panel textarea,.panel select{background:#050505;color:var(--yellow);border:1px solid var(--yellow);padding:8px;border-radius:0}.bar input{flex:1;min-width:220px}.bar button,.panel button{font-weight:900;cursor:pointer}.grid{display:grid;grid-template-columns:1fr 300px;gap:12px}.task{border:1px solid var(--yellow);padding:10px;margin:8px 0;display:grid;grid-template-columns:1fr auto;gap:8px}.task.done{color:var(--green);border-color:var(--green)}.task.urgent{color:var(--red);border-color:var(--red)}.meta{color:#aaa;font-size:12px;margin-top:4px}.danger{color:var(--red)}.muted{color:#aaa}@media(max-width:700px){.grid{grid-template-columns:1fr}.internal{display:none}}</style></head>
<body><div class="wrap"><div class="top"><div class="brand">任务列表<small>TASK LIST</small></div><div class="internal">INTERNAL</div></div>
<div class="bar"><input id="token" type="password" placeholder="ADMIN TOKEN"><button id="save">保存 TOKEN</button><button id="skill">复制 AI SKILL</button><span id="msg" class="muted"></span></div>
<div class="grid"><div class="panel"><h2>任务 / TASKS</h2><div id="list"></div></div><div class="panel"><h2>新增任务 / ADD</h2><input id="title" placeholder="SHORT TITLE"><textarea id="notes" placeholder="中文详情"></textarea><select id="priority"><option value="1">NORMAL</option><option value="2">HIGH</option><option value="3">URGENT</option></select><button id="add">ADD TASK</button></div></div></div>
<script>const $=id=>document.getElementById(id),api=p=>'/api/v1'+p;let tasks=[];function token(){return $('token').value||localStorage.evaTodoToken||''}function headers(){return {'content-type':'application/json','authorization':'Bearer '+token()}}async function req(p,o={}){const r=await fetch(api(p),{...o,headers:{...headers(),...(o.headers||{})}}),j=await r.json().catch(()=>({}));if(!r.ok||!j.ok)throw Error(j.error?.message||r.statusText);return j}function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}async function load(){try{const j=await req('/tasks');tasks=j.tasks||[];$('list').innerHTML=tasks.map(t=>'<div class="task '+(t.status==='done'?'done ':'')+(t.urgent?'urgent':'')+'"><div><b>'+esc(t.title)+'</b><div>'+esc(t.notes)+'</div><div class="meta">'+esc(t.status)+' · P'+t.priority+'</div></div><div><button data-id="'+esc(t.id)+'" data-act="done">'+(t.status==='done'?'重开':'完成')+'</button> <button data-id="'+esc(t.id)+'" data-act="del">删除</button></div></div>').join('')||'<p class="muted">NO TASKS</p>'}catch(e){$('list').innerHTML='<p class="danger">'+esc(e.message)+'</p>'}}document.addEventListener('click',async e=>{const id=e.target.dataset.id,act=e.target.dataset.act;if(!id)return;try{if(act==='done')await req('/tasks/'+encodeURIComponent(id)+'/'+(tasks.find(t=>t.id===id).status==='done'?'reopen':'complete'),{method:'POST'});if(act==='del'&&confirm('Delete task?'))await req('/tasks/'+encodeURIComponent(id),{method:'DELETE'});load()}catch(err){alert(err.message)}});$('save').onclick=()=>{localStorage.evaTodoToken=$('token').value;load()};$('skill').onclick=async()=>{try{const r=await fetch('/skill.md'),t=await r.text();await navigator.clipboard.writeText(t);$('msg').textContent='SKILL COPIED · token required'}catch(e){$('msg').textContent='OPEN /skill.md TO DOWNLOAD'}};$('add').onclick=async()=>{try{await req('/tasks',{method:'POST',body:JSON.stringify({title:$('title').value,notes:$('notes').value,priority:Number($('priority').value)})});$('title').value='';$('notes').value='';load()}catch(e){alert(e.message)}};$('token').value=localStorage.evaTodoToken||'';load();</script></body></html>`
