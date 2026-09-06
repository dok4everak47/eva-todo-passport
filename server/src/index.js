const API_VERSION = "v1";
const STATUS = new Set(["todo", "doing", "done", "archived"]);

export default {
  async fetch(request, env, ctx) {
    try {
      const url = new URL(request.url);
      if (ctx?.waitUntil) ctx.waitUntil(purgeDeleted(env));
      if (request.method === "OPTIONS") return cors(new Response(null, { status: 204 }));
      if (url.pathname === "/skill.md" && request.method === "GET") return skillResponse(request);
      if (url.pathname === "/" || url.pathname === "/index.html") return html();
      if (!url.pathname.startsWith(`/api/${API_VERSION}/`)) return notFound();

      const path = url.pathname.slice(`/api/${API_VERSION}`.length);
      if (path === "/health" && request.method === "GET") return json(await health(env));

      if (path === "/tasks" && request.method === "GET") {
        requireAnyAuth(request, env);
        return json({ ok: true, ...(await listTasks(request, env)) });
      }
      if (path === "/tasks" && request.method === "POST") {
        requireAdmin(request, env);
        return json({ ok: true, task: await createTask(request, env) }, 201);
      }

      const taskMatch = path.match(/^\/tasks\/([^/]+)(?:\/(complete|reopen))?$/);
      if (taskMatch) {
        const id = decodeURIComponent(taskMatch[1]);
        const action = taskMatch[2];
        if (request.method === "GET" && !action) {
          requireAnyAuth(request, env);
          const task = await getTask(env, id);
          if (!task) return error("not_found", "Task not found", 404);
          return json({ ok: true, task });
        }
        requireAdmin(request, env);
        if (request.method === "PATCH" && !action) return json({ ok: true, task: await patchTask(request, env, id) });
        if (request.method === "DELETE" && !action) return json({ ok: true, task: await softDeleteTask(env, id) });
        if (request.method === "POST" && action === "complete") return json({ ok: true, task: await setComplete(env, id, true) });
        if (request.method === "POST" && action === "reopen") return json({ ok: true, task: await setComplete(env, id, false) });
      }

      if (path === "/sync" && request.method === "POST") {
        requireAnyAuth(request, env);
        return json({ ok: true, ...(await sync(request, env)) });
      }
      if (path === "/events" && request.method === "GET") {
        requireAnyAuth(request, env);
        return json({ ok: true, ...(await waitForEvents(request, env)) });
      }
      if (path === "/report" && request.method === "POST") {
        requireAnyAuth(request, env);
        return json({ ok: true, report: await createReport(request, env) }, 201);
      }
      if (path === "/reports" && request.method === "GET") {
        requireAdmin(request, env);
        return json({ ok: true, reports: await listReports(request, env) });
      }

      return notFound();
    } catch (err) {
      if (err instanceof ApiError) return error(err.code, err.message, err.status);
      console.error(err);
      return error("internal_error", "Internal error", 500);
    }
  }
  ,
  async scheduled(controller, env) {
    await purgeDeleted(env);
  }
};

class ApiError extends Error {
  constructor(code, message, status = 400) {
    super(message);
    this.code = code;
    this.status = status;
  }
}

function cors(response) {
  const h = new Headers(response.headers);
  h.set("Access-Control-Allow-Origin", "*");
  h.set("Access-Control-Allow-Methods", "GET,POST,PATCH,DELETE,OPTIONS");
  h.set("Access-Control-Allow-Headers", "Authorization,Content-Type,X-Device-Token,X-Admin-Token");
  h.set("Access-Control-Max-Age", "86400");
  return new Response(response.body, { status: response.status, headers: h });
}

function json(body, status = 200) {
  return cors(Response.json(body, { status }));
}

function error(code, message, status = 400) {
  return json({ ok: false, error: { code, message } }, status);
}

function notFound() {
  return error("not_found", "Endpoint not found", 404);
}

function nowIso() {
  return new Date().toISOString();
}

function bearer(request) {
  const url = new URL(request.url);
  const auth = request.headers.get("authorization") || "";
  if (auth.toLowerCase().startsWith("bearer ")) return auth.slice(7).trim();
  return request.headers.get("x-admin-token")
    || request.headers.get("x-device-token")
    || url.searchParams.get("token")
    || "";
}

function requireAdmin(request, env) {
  if (!env.ADMIN_TOKEN) throw new ApiError("server_not_configured", "ADMIN_TOKEN is missing", 500);
  if (bearer(request) !== env.ADMIN_TOKEN) throw new ApiError("unauthorized", "Admin token required", 401);
}

function requireAnyAuth(request, env) {
  const token = bearer(request);
  if (!token || (token !== env.ADMIN_TOKEN && token !== env.DEVICE_TOKEN)) {
    throw new ApiError("unauthorized", "Bearer token required", 401);
  }
}

async function readJson(request, fallback = {}) {
  const text = await request.text();
  if (!text.trim()) return fallback;
  try {
    return JSON.parse(text);
  } catch {
    throw new ApiError("bad_json", "Request body must be valid JSON", 400);
  }
}

async function health(env) {
  return {
    ok: true,
    name: "eva-todo-api",
    apiVersion: API_VERSION,
    serverTime: nowIso(),
    version: await currentVersion(env)
  };
}

async function currentVersion(env) {
  const row = await env.DB.prepare("SELECT value FROM meta WHERE key = 'version'").first();
  return Number(row?.value || 0);
}

async function purgeDeleted(env) {
  if (!env.DB) return;
  const cutoff = new Date(Date.now() - 24 * 60 * 60 * 1000).toISOString();
  await env.DB.prepare(
    "DELETE FROM tasks WHERE deleted_at IS NOT NULL AND deleted_at <= ?"
  ).bind(cutoff).run();
}

async function nextVersion(env) {
  await env.DB
    .prepare("UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = 'version'")
    .run();
  return await currentVersion(env);
}

function cleanString(v, max = 500) {
  if (v == null) return "";
  return String(v).replace(/\s+/g, " ").trim().slice(0, max);
}

function cleanDate(v) {
  const s = cleanString(v, 64);
  return s || null;
}

function cleanPriority(v) {
  const n = Number.isFinite(Number(v)) ? Math.trunc(Number(v)) : 1;
  return Math.min(3, Math.max(0, n));
}

function cleanStatus(v, fallback = "todo") {
  const s = cleanString(v, 16).toLowerCase();
  return STATUS.has(s) ? s : fallback;
}

function cleanTags(v) {
  const list = Array.isArray(v) ? v : cleanString(v, 200).split(",");
  return [...new Set(list.map(x => cleanString(x, 32).toLowerCase()).filter(Boolean))].slice(0, 12);
}

function boolInt(v) {
  return v === true || v === 1 || v === "1" || String(v).toLowerCase() === "true" ? 1 : 0;
}

function toApiTask(row) {
  if (!row) return null;
  let tags = [];
  try { tags = JSON.parse(row.tags || "[]"); } catch { tags = []; }
  return {
    id: row.id,
    title: row.title,
    notes: row.notes || "",
    status: row.status,
    priority: Number(row.priority || 0),
    urgent: Boolean(row.urgent || (row.status !== "done" && Number(row.priority || 0) >= 3)),
    dueAt: row.due_at || null,
    tags,
    sortOrder: Number(row.sort_order || 0),
    completedAt: row.completed_at || null,
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    deletedAt: row.deleted_at || null,
    version: Number(row.version || 0)
  };
}

async function listTasks(request, env) {
  const url = new URL(request.url);
  const includeDeleted = url.searchParams.get("includeDeleted") === "1";
  const since = Number(url.searchParams.get("sinceVersion") || 0);
  const q = cleanString(url.searchParams.get("q"), 80).toLowerCase();
  const statusParam = url.searchParams.get("status");
  const status = statusParam ? cleanStatus(statusParam, "") : "";
  const tag = cleanString(url.searchParams.get("tag"), 32).toLowerCase();
  const where = [];
  const params = [];
  if (!includeDeleted) where.push("deleted_at IS NULL");
  if (since > 0) { where.push("version > ?"); params.push(since); }
  if (status) { where.push("status = ?"); params.push(status); }
  if (q) {
    where.push("(LOWER(title) LIKE ? OR LOWER(notes) LIKE ?)");
    params.push(`%${q}%`, `%${q}%`);
  }
  if (tag) {
    where.push("LOWER(tags) LIKE ?");
    params.push(`%"${tag.replaceAll('"', '""')}"%`);
  }
  const sql = `SELECT * FROM tasks ${where.length ? "WHERE " + where.join(" AND ") : ""}
    ORDER BY COALESCE(deleted_at, ''), sort_order ASC, created_at ASC`;
  const res = await env.DB.prepare(sql).bind(...params).all();
  return { version: await currentVersion(env), tasks: (res.results || []).map(toApiTask) };
}

async function getTask(env, id) {
  return toApiTask(await env.DB.prepare("SELECT * FROM tasks WHERE id = ?").bind(id).first());
}

async function createTask(request, env) {
  const body = await readJson(request);
  const title = cleanString(body.title, 120);
  if (!title) throw new ApiError("missing_title", "title is required", 400);
  const id = cleanString(body.id, 80) || crypto.randomUUID();
  const ts = nowIso();
  const version = await nextVersion(env);
  const status = cleanStatus(body.status);
  const completedAt = status === "done" ? (cleanDate(body.completedAt) || ts) : null;
  await env.DB.prepare(
    `INSERT INTO tasks
      (id, title, notes, status, priority, urgent, due_at, tags, sort_order, completed_at, created_at, updated_at, deleted_at, version)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?)`
  ).bind(
    id,
    title,
    cleanString(body.notes, 500),
    status,
    cleanPriority(body.priority),
    boolInt(body.urgent),
    cleanDate(body.dueAt),
    JSON.stringify(cleanTags(body.tags)),
    Number.isFinite(Number(body.sortOrder)) ? Math.trunc(Number(body.sortOrder)) : version * 10,
    completedAt,
    ts,
    ts,
    version
  ).run();
  return await getTask(env, id);
}

async function patchTask(request, env, id) {
  const existing = await getTask(env, id);
  if (!existing) throw new ApiError("not_found", "Task not found", 404);
  const body = await readJson(request);
  const status = body.status == null ? existing.status : cleanStatus(body.status, existing.status);
  const ts = nowIso();
  const version = await nextVersion(env);
  const completedAt = status === "done"
    ? (cleanDate(body.completedAt) || existing.completedAt || ts)
    : null;
  await env.DB.prepare(
    `UPDATE tasks SET
      title = ?, notes = ?, status = ?, priority = ?, urgent = ?, due_at = ?, tags = ?,
      sort_order = ?, completed_at = ?, updated_at = ?, deleted_at = ?, version = ?
     WHERE id = ?`
  ).bind(
    body.title == null ? existing.title : cleanString(body.title, 120),
    body.notes == null ? existing.notes : cleanString(body.notes, 500),
    status,
    body.priority == null ? existing.priority : cleanPriority(body.priority),
    body.urgent == null ? (existing.urgent ? 1 : 0) : boolInt(body.urgent),
    body.dueAt === undefined ? existing.dueAt : cleanDate(body.dueAt),
    body.tags === undefined ? JSON.stringify(existing.tags) : JSON.stringify(cleanTags(body.tags)),
    body.sortOrder == null || !Number.isFinite(Number(body.sortOrder))
      ? existing.sortOrder
      : Math.trunc(Number(body.sortOrder)),
    completedAt,
    ts,
    body.deletedAt === undefined ? existing.deletedAt : cleanDate(body.deletedAt),
    version,
    id
  ).run();
  return await getTask(env, id);
}

async function setComplete(env, id, done) {
  const existing = await getTask(env, id);
  if (!existing) throw new ApiError("not_found", "Task not found", 404);
  const ts = nowIso();
  const version = await nextVersion(env);
  await env.DB.prepare(
    "UPDATE tasks SET status = ?, urgent = CASE WHEN ? = 'done' THEN 0 ELSE urgent END, completed_at = ?, updated_at = ?, deleted_at = NULL, version = ? WHERE id = ?"
  ).bind(done ? "done" : "todo", done ? "done" : "todo", done ? ts : null, ts, version, id).run();
  return await getTask(env, id);
}

async function softDeleteTask(env, id) {
  const existing = await getTask(env, id);
  if (!existing) throw new ApiError("not_found", "Task not found", 404);
  const ts = nowIso();
  const version = await nextVersion(env);
  await env.DB.prepare(
    "UPDATE tasks SET deleted_at = ?, updated_at = ?, version = ? WHERE id = ?"
  ).bind(ts, ts, version, id).run();
  return await getTask(env, id);
}

async function upsertFromSync(env, task, operation = "upsert") {
  const id = cleanString(task?.id, 80) || crypto.randomUUID();
  const existing = await getTask(env, id);
  const ts = nowIso();
  const version = await nextVersion(env);
  if (operation === "delete") {
    if (!existing) return null;
    await env.DB.prepare("UPDATE tasks SET deleted_at = ?, updated_at = ?, version = ? WHERE id = ?")
      .bind(ts, ts, version, id).run();
    return await getTask(env, id);
  }
  const status = operation === "complete" ? "done"
    : operation === "reopen" ? "todo"
    : cleanStatus(task?.status, existing?.status || "todo");
  const title = cleanString(task?.title ?? existing?.title, 120);
  if (!title) throw new ApiError("missing_title", "Synced task title is required", 400);
  const notes = cleanString(task?.notes ?? existing?.notes, 500);
  const completedAt = status === "done" ? (cleanDate(task?.completedAt) || existing?.completedAt || ts) : null;
  await env.DB.prepare(
    `INSERT INTO tasks
      (id, title, notes, status, priority, urgent, due_at, tags, sort_order, completed_at, created_at, updated_at, deleted_at, version)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, ?)
     ON CONFLICT(id) DO UPDATE SET
      title = excluded.title, notes = excluded.notes, status = excluded.status,
      priority = excluded.priority, urgent = excluded.urgent, due_at = excluded.due_at,
      tags = excluded.tags, sort_order = excluded.sort_order, completed_at = excluded.completed_at,
      updated_at = excluded.updated_at, deleted_at = NULL, version = excluded.version`
  ).bind(
    id,
    title,
    notes,
    status,
    cleanPriority(task?.priority ?? existing?.priority ?? 1),
    status === "done" ? 0 : boolInt(task?.urgent ?? existing?.urgent ?? 0),
    cleanDate(task?.dueAt ?? existing?.dueAt),
    JSON.stringify(cleanTags(task?.tags ?? existing?.tags ?? [])),
    Number.isFinite(Number(task?.sortOrder ?? existing?.sortOrder))
      ? Math.trunc(Number(task?.sortOrder ?? existing?.sortOrder))
      : version * 10,
    completedAt,
    existing?.createdAt || ts,
    ts,
    version
  ).run();
  return await getTask(env, id);
}

async function sync(request, env) {
  const body = await readJson(request, {});
  const since = Math.max(0, Math.trunc(Number(body.sinceVersion || 0)));
  const mutations = Array.isArray(body.mutations) ? body.mutations.slice(0, 32) : [];
  const accepted = [];
  for (const mutation of mutations) {
    const operation = cleanString(mutation.operation || "upsert", 16);
    if (!["upsert", "complete", "reopen", "delete"].includes(operation)) {
      throw new ApiError("bad_operation", `Unsupported operation: ${operation}`, 400);
    }
    const task = await upsertFromSync(env, mutation.task || {}, operation);
    accepted.push({ clientMutationId: mutation.clientMutationId || null, operation, task });
  }
  // The badge keeps a tiny full-list cache. Return the active list on every sync
  // so remote deletes and edits are reflected without a second reconciliation path.
  const remote = await listTasks(new Request(`https://local/api/${API_VERSION}/tasks`), env);
  return {
    serverTime: nowIso(),
    version: remote.version,
    accepted,
    tasks: remote.tasks
  };
}

async function waitForEvents(request, env) {
  const url = new URL(request.url);
  const since = Math.max(0, Math.trunc(Number(url.searchParams.get("sinceVersion") || 0)));
  const timeoutMs = Math.min(25000, Math.max(0, Math.trunc(Number(url.searchParams.get("timeoutMs") || 15000))));
  const started = Date.now();
  let version = await currentVersion(env);
  while (version <= since && Date.now() - started < timeoutMs) {
    await new Promise(resolve => setTimeout(resolve, 1000));
    version = await currentVersion(env);
  }
  return { changed: version > since, version, serverTime: nowIso() };
}

async function createReport(request, env) {
  const body = await readJson(request, {});
  const ts = nowIso();
  const deviceId = cleanString(body.deviceId, 80) || "unknown";
  const raw = JSON.stringify(body).slice(0, 4096);
  const result = await env.DB.prepare(
    `INSERT INTO device_reports
      (device_id, firmware_version, local_version, battery_percent, wifi_rssi,
       task_total, task_done, raw_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
  ).bind(
    deviceId,
    cleanString(body.firmwareVersion, 40),
    Math.trunc(Number(body.localVersion || 0)),
    body.batteryPercent == null ? null : Math.trunc(Number(body.batteryPercent)),
    body.wifiRssi == null ? null : Math.trunc(Number(body.wifiRssi)),
    body.taskTotal == null ? null : Math.trunc(Number(body.taskTotal)),
    body.taskDone == null ? null : Math.trunc(Number(body.taskDone)),
    raw,
    ts
  ).run();
  return { id: result.meta?.last_row_id || null, deviceId, createdAt: ts };
}

async function listReports(request, env) {
  const url = new URL(request.url);
  const limit = Math.min(100, Math.max(1, Math.trunc(Number(url.searchParams.get("limit") || 20))));
  const res = await env.DB.prepare(
    "SELECT * FROM device_reports ORDER BY created_at DESC LIMIT ?"
  ).bind(limit).all();
  return (res.results || []).map(r => ({
    id: r.id,
    deviceId: r.device_id,
    firmwareVersion: r.firmware_version,
    localVersion: r.local_version,
    batteryPercent: r.battery_percent,
    wifiRssi: r.wifi_rssi,
    taskTotal: r.task_total,
    taskDone: r.task_done,
    createdAt: r.created_at,
    raw: safeJson(r.raw_json)
  }));
}

function safeJson(s) {
  try { return JSON.parse(s || "{}"); } catch { return {}; }
}

function html() {
  return new Response(INDEX_HTML, {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "no-store"
    }
  });
}

function skillResponse(request) {
  const baseUrl = new URL(request.url).origin + `/api/${API_VERSION}`;
  const body = SKILL_MD.replaceAll("{{BASE_URL}}", baseUrl);
  return new Response(body, {
    headers: {
      "content-type": "text/markdown; charset=utf-8",
      "content-disposition": 'attachment; filename="eva-todo-skill.md"',
      "cache-control": "no-store"
    }
  });
}

const SKILL_MD = `# EVA Todo API Skill

Use this API to control the EVA Todo badge and its web task list.

## Connection

- Base URL: {{BASE_URL}}
- Admin credential: set \`EVA_TODO_ADMIN_TOKEN\`
- Device credential: set \`EVA_TODO_DEVICE_TOKEN\`

Send the selected credential as \`Authorization: Bearer <token>\`.

## Permissions

- \`ADMIN_TOKEN\`: list, create, edit, complete, reopen, delete tasks, and read reports.
- \`DEVICE_TOKEN\`: badge \`/sync\`, \`/events\`, and \`/report\`; it cannot change tasks through the admin CRUD endpoints.
- Never ask the user to paste a token into a public prompt, commit it, or print it.

## Common calls

\`GET /health\`

\`GET /tasks\`

\`POST /tasks\` with JSON \`{"title":"SHORT TITLE","notes":"中文详情","priority":1,"urgent":false}\`

\`PATCH /tasks/{id}\` to edit a task.

\`POST /tasks/{id}/complete\` or \`POST /tasks/{id}/reopen\`.

\`DELETE /tasks/{id}\` soft-deletes it. Deleted tasks are physically purged after 24 hours.

After every write, call \`GET /tasks\` and report the resulting task id/status. Keep badge-visible titles short; put long Chinese instructions in \`notes\`.
`;

const INDEX_HTML = `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EVA TODO CONTROL</title>
<style>
:root{--bg:#000;--green:#95ef5e;--yellow:#ffdc00;--red:#ff3232;--dim:#4b4b2b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--yellow);font-family:Arial,"Microsoft YaHei",sans-serif;letter-spacing:.02em}
.wrap{max-width:980px;margin:0 auto;padding:18px}.top{display:grid;grid-template-columns:1fr auto;gap:12px;align-items:stretch}
.brand{background:var(--green);color:#000;padding:12px 16px;font-weight:900;font-size:28px;line-height:1}.brand small{display:block;font-size:15px;margin-top:5px}
.internal{border:2px solid var(--yellow);padding:8px 52px 8px 12px;position:relative;font-size:22px;font-weight:900}.internal small{display:block;font-size:13px}.slash{position:absolute;right:12px;top:8px;width:28px;height:12px;background:var(--red);transform:skew(-24deg)}.slash.s2{top:24px}.slash.s3{top:40px}
.bar{border:2px solid var(--yellow);margin-top:12px;padding:12px;display:flex;gap:12px;align-items:center;flex-wrap:wrap}.bar input,.bar select,.bar button,.card input,.card textarea,.card select{background:#050505;color:var(--yellow);border:1px solid var(--yellow);padding:8px;border-radius:0}.bar button,.card button{cursor:pointer;font-weight:800}.bar button:hover,.card button:hover{background:var(--yellow);color:#000}
.grid{display:grid;grid-template-columns:1fr 310px;gap:12px;margin-top:12px}.panel{border:2px solid var(--yellow);padding:12px;min-height:260px}.panel h2{margin:0 0 10px;font-size:18px}
.task{border:1px solid var(--yellow);padding:10px;margin:8px 0;display:grid;grid-template-columns:24px 1fr auto;gap:10px;align-items:start}.task.done{color:var(--green);border-color:var(--green)}.task.urgent{color:var(--red);border-color:var(--red)}
.box{width:18px;height:18px;border:2px solid currentColor;margin-top:2px}.done .box{background:var(--green)}.urgent .box{background:var(--red);color:#000;display:grid;place-items:center;font-weight:900}.meta{font-size:12px;color:#aaa;margin-top:4px}.actions{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}.actions button{font-size:12px;padding:6px}
.card label{display:block;font-size:12px;margin:10px 0 4px}.card input,.card textarea,.card select{width:100%}.card textarea{min-height:72px}.muted{color:#aaa}.stats{font-size:28px;font-weight:900}.danger{color:var(--red)}@media(max-width:760px){.grid{grid-template-columns:1fr}.top{grid-template-columns:1fr}.internal{display:none}}
</style>
</head>
<body><div class="wrap">
  <div class="top"><div class="brand">任务リスト<small>TASK LIST</small></div><div class="internal">内部<small>INTERNAL</small><i class="slash"></i><i class="slash s2"></i><i class="slash s3"></i></div></div>
  <div class="bar">
    <input id="token" type="password" placeholder="ADMIN TOKEN">
     <button id="saveToken">SAVE TOKEN</button><button id="copySkill">AI SKILL</button><span id="skillStatus" class="muted"></span>
    <input id="q" placeholder="SEARCH / TAG / TITLE">
    <select id="filter"><option value="">ALL</option><option value="todo">TODO</option><option value="doing">DOING</option><option value="done">DONE</option><option value="archived">ARCHIVED</option></select>
    <button id="refresh">REFRESH</button>
    <span id="health" class="muted"></span>
  </div>
  <div class="grid">
    <div class="panel"><h2>任务 / TASKS <span id="stats" class="stats"></span></h2><div id="list"></div></div>
    <div class="panel card">
      <h2><span id="formTitle">新增 / ADD</span></h2>
      <input id="editId" type="hidden">
      <label>TITLE (badge-friendly English preferred)</label><input id="title" maxlength="120" placeholder="SERVER MAINT">
      <label>NOTES / 中文备注</label><textarea id="notes" maxlength="500" placeholder="服务器维护"></textarea>
      <label>STATUS</label><select id="status"><option value="todo">todo</option><option value="doing">doing</option><option value="done">done</option></select>
      <label>PRIORITY</label><select id="priority"><option value="1">normal</option><option value="2">high</option><option value="3">urgent</option><option value="0">low</option></select>
      <label>DUE AT</label><input id="dueAt" type="datetime-local">
      <label>TAGS</label><input id="tags" placeholder="daily,ops">
      <p><label><input id="urgent" type="checkbox"> URGENT / 红色警示</label></p>
      <button id="add">ADD TASK</button>
      <button id="cancelEdit" type="button">CANCEL</button>
      <p class="muted">ESP32 徽章端优先显示 TITLE；中文可作为 notes 同步给网页/API。</p>
    </div>
  </div>
</div>
<script>
const $ = id => document.getElementById(id);
const api = path => '/api/v1' + path;
let tasks = [];
function token(){ return $('token').value || localStorage.evaTodoToken || ''; }
function headers(){ return {'content-type':'application/json','authorization':'Bearer '+token()}; }
async function req(path, opts={}){
  const r = await fetch(api(path), { ...opts, headers: { ...headers(), ...(opts.headers||{}) } });
  const j = await r.json().catch(()=>({ok:false,error:{message:r.statusText}}));
  if(!r.ok || !j.ok) throw new Error(j.error?.message || r.statusText);
  return j;
}
function esc(s){ return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function meta(t){ return [t.status, 'P'+t.priority, t.urgent?'URGENT':'', t.dueAt||'', (t.tags||[]).join(',')].filter(Boolean).join(' · '); }
async function load(){
  try{
    const q = encodeURIComponent($('q').value.trim());
    const st = $('filter').value;
    const j = await req('/tasks?'+(q?'q='+q+'&':'')+(st?'status='+st+'&':''));
    tasks = j.tasks || [];
    render(j.version);
    const h = await req('/health');
    $('health').textContent = 'VER '+h.version+' · '+new Date(h.serverTime).toLocaleString();
  }catch(e){ $('list').innerHTML = '<p class="danger">'+esc(e.message)+'</p>'; }
}
function render(version){
  const done = tasks.filter(t=>t.status==='done').length;
  $('stats').textContent = String(done).padStart(2,'0')+' / '+String(tasks.length).padStart(2,'0')+' · v'+version;
  $('list').innerHTML = tasks.map(t => '<div class="task '+(t.status==='done'?'done ':'')+(t.urgent?'urgent':'')+'">'+
    '<div class="box">'+(t.urgent?'!':'')+'</div><div><b>'+esc(t.title)+'</b><div>'+esc(t.notes)+'</div><div class="meta">'+esc(meta(t))+'</div></div>'+
    '<div class="actions">'+
    '<button data-act="edit" data-id="'+esc(t.id)+'">EDIT</button>'+
    '<button data-act="done" data-id="'+esc(t.id)+'">'+(t.status==='done'?'REOPEN':'DONE')+'</button>'+
    '<button data-act="urgent" data-id="'+esc(t.id)+'">'+(t.urgent?'UNFLAG':'URGENT')+'</button>'+
    '<button data-act="del" data-id="'+esc(t.id)+'">DELETE</button></div></div>').join('') || '<p class="muted">NO TASKS</p>';
}
document.addEventListener('click', async e => {
  const id = e.target?.dataset?.id; const act = e.target?.dataset?.act;
  if(act && id){
    const t = tasks.find(x=>x.id===id);
    if(!t) return;
    if(act==='edit'){ startEdit(t); return; }
    try{
      if(act==='done') await req('/tasks/'+encodeURIComponent(id)+'/'+(t.status==='done'?'reopen':'complete'), {method:'POST'});
      if(act==='urgent') await req('/tasks/'+encodeURIComponent(id), {method:'PATCH', body:JSON.stringify({urgent:!t.urgent, priority:!t.urgent?3:(t.priority>=3?1:t.priority)})});
      if(act==='del' && confirm('Delete task?')) await req('/tasks/'+encodeURIComponent(id), {method:'DELETE'});
      await load();
    }catch(err){ alert(err.message); }
  }
});
$('saveToken').onclick = () => { localStorage.evaTodoToken = $('token').value; load(); };
$('copySkill').onclick = async () => {
  try {
    const r = await fetch('/skill.md');
    const text = await r.text();
    await navigator.clipboard.writeText(text);
    $('skillStatus').textContent = 'SKILL COPIED · token required';
  } catch (e) {
    $('skillStatus').textContent = 'OPEN /skill.md TO DOWNLOAD';
  }
};
$('refresh').onclick = load; $('q').oninput = () => clearTimeout(window._q) || (window._q=setTimeout(load,250)); $('filter').onchange = load;
function localDateInput(s){
  if(!s) return '';
  const d = new Date(s);
  if(Number.isNaN(d.getTime())) return String(s).slice(0,16);
  return new Date(d.getTime()-d.getTimezoneOffset()*60000).toISOString().slice(0,16);
}
function resetForm(){
  $('editId').value=''; ['title','notes','tags','dueAt'].forEach(id=>$(id).value='');
  $('status').value='todo'; $('priority').value='1'; $('urgent').checked=false;
  $('formTitle').textContent='新增 / ADD'; $('add').textContent='ADD TASK';
}
function startEdit(t){
  $('editId').value=t.id; $('title').value=t.title||''; $('notes').value=t.notes||'';
  $('status').value=t.status||'todo'; $('priority').value=String(t.priority ?? 1);
  $('dueAt').value=localDateInput(t.dueAt); $('tags').value=(t.tags||[]).join(',');
  $('urgent').checked=!!t.urgent; $('formTitle').textContent='编辑 / EDIT'; $('add').textContent='SAVE TASK';
}
function formPayload(){
  return {title:$('title').value,notes:$('notes').value,status:$('status').value,
    priority:Number($('priority').value),urgent:$('urgent').checked,
    dueAt:$('dueAt').value||null,tags:$('tags').value};
}
$('add').onclick = async () => {
  try{
    const id = $('editId').value;
    await req(id?'/tasks/'+encodeURIComponent(id):'/tasks',{method:id?'PATCH':'POST',body:JSON.stringify(formPayload())});
    resetForm(); await load();
  }catch(e){ alert(e.message); }
};
$('cancelEdit').onclick = resetForm;
const preload = new URL(location.href).searchParams.get('token'); if(preload){ localStorage.evaTodoToken=preload; history.replaceState(null,'','/'); }
$('token').value = localStorage.evaTodoToken || ''; load();
</script></body></html>`;
