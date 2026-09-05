# EVA Todo API

Cloudflare Worker service for the ESP32 badge, Web UI, and AI agents.

Base URL after deployment:

~~~text
https://eva-todo-api.<your-subdomain>.workers.dev/api/v1
~~~

Authentication:

~~~http
Authorization: Bearer <ADMIN_TOKEN or DEVICE_TOKEN>
~~~

- ADMIN_TOKEN: full read/write access, used by the web UI and AI agents.
- DEVICE_TOKEN: device sync/report access, intended for the ESP32 firmware.
- Tokens are Worker Secrets. Do not put them in Git or final answers.
- Wi-Fi SSID/password are firmware-only values in main/firmware_private.h; the server never stores Wi-Fi credentials.

## Data Model

~~~json
{
  "id": "task-smart-grid-audit",
  "title": "SMART GRID AUD.",
  "notes": "审核年度智能配电监控方案",
  "status": "todo",
  "priority": 3,
  "urgent": true,
  "dueAt": null,
  "tags": ["ops", "audit"],
  "sortOrder": 40,
  "completedAt": null,
  "createdAt": "2026-09-05T00:00:00.000Z",
  "updatedAt": "2026-09-05T00:00:00.000Z",
  "deletedAt": null,
  "version": 4
}
~~~

Status values: todo, doing, done, archived.

Priority values: 0 low, 1 normal, 2 high, 3 urgent.

Badge display rule: keep title short and ASCII-friendly for reliable on-device rendering. Put Chinese details or long instructions in notes.

## Endpoints

### Health

~~~http
GET /api/v1/health
~~~

Returns service name, API version, server time, and global task version. No auth required.

### List Tasks

~~~http
GET /api/v1/tasks?status=todo&q=server&tag=ops&sinceVersion=0&includeDeleted=0
~~~

Optional filters: status, q, tag, sinceVersion, includeDeleted=1.

Requires ADMIN_TOKEN or DEVICE_TOKEN.

### Create Task

~~~http
POST /api/v1/tasks
Content-Type: application/json
~~~

~~~json
{
  "title": "GO API TEST",
  "notes": "编写 Go 后端 API 测试脚本",
  "priority": 2,
  "urgent": false,
  "dueAt": "2026-09-06T10:00:00+08:00",
  "tags": ["dev", "api"]
}
~~~

Requires ADMIN_TOKEN.

### Patch Task

~~~http
PATCH /api/v1/tasks/{id}
Content-Type: application/json
~~~

~~~json
{
  "status": "doing",
  "priority": 3,
  "urgent": true
}
~~~

Requires ADMIN_TOKEN.

### Complete / Reopen

~~~http
POST /api/v1/tasks/{id}/complete
POST /api/v1/tasks/{id}/reopen
~~~

Requires ADMIN_TOKEN.

### Delete Task

~~~http
DELETE /api/v1/tasks/{id}
~~~

Soft-deletes the task by setting deletedAt.

Requires ADMIN_TOKEN.

### Sync

~~~http
POST /api/v1/sync
Content-Type: application/json
~~~

~~~json
{
  "deviceId": "ai-passport-001",
  "sinceVersion": 5,
  "mutations": [
    {
      "clientMutationId": "local-1",
      "operation": "complete",
      "task": {
        "id": "task-smart-grid-audit"
      }
    }
  ]
}
~~~

Operations: upsert, complete, reopen, delete.

The ESP32 firmware currently sends complete/reopen operations. The response returns the complete active task list because the badge keeps only a tiny local cache.

### Long-Poll Events

~~~http
GET /api/v1/events?sinceVersion=9&timeoutMs=15000
~~~

Returns when the global version changes, or when the timeout expires. timeoutMs is capped at 25000.

Requires ADMIN_TOKEN or DEVICE_TOKEN.

### Device Report

~~~http
POST /api/v1/report
Content-Type: application/json
~~~

~~~json
{
  "deviceId": "ai-passport-001",
  "firmwareVersion": "1.0.0",
  "localVersion": 9,
  "wifiRssi": -55,
  "batteryPercent": 82,
  "taskTotal": 5,
  "taskDone": 3
}
~~~

Requires ADMIN_TOKEN or DEVICE_TOKEN.

### Reports

~~~http
GET /api/v1/reports?limit=20
~~~

Requires ADMIN_TOKEN.

## Web UI

Open the Worker root URL after deployment:

~~~text
https://eva-todo-api.<your-subdomain>.workers.dev/
~~~

Enter ADMIN_TOKEN in the password field. The token is stored in browser localStorage for convenience.

## Deployment

Cloudflare resources:

- Worker name: eva-todo-api
- D1 database name: eva_todo_db
- Binding name: DB
- Required Worker Secrets: ADMIN_TOKEN, DEVICE_TOKEN

Recommended deploy flow:

~~~powershell
cd server
npm install
npm run secrets:init
$env:CLOUDFLARE_API_TOKEN = "<your-cloudflare-api-token>"
npm run deploy:full
~~~

deploy:full creates or reuses the D1 database, applies migrations remotely, uploads Worker secrets from server/.dev.vars, deploys the Worker, and updates main/firmware_private.h with the deployed /api/v1 URL when possible.

## AI Skill

Project-local Skill:

~~~text
skills/eva-todo-control/
~~~

Expected environment:

~~~powershell
$env:EVA_TODO_BASE_URL = "https://eva-todo-api.<your-subdomain>.workers.dev/api/v1"
$env:EVA_TODO_ADMIN_TOKEN = "<ADMIN_TOKEN>"
~~~

Examples:

~~~powershell
node skills/eva-todo-control/scripts/eva_todo.mjs list
node skills/eva-todo-control/scripts/eva_todo.mjs add --title "WRITE DOCS" --notes "补 API 文档" --priority 2 --tags docs,ai
node skills/eva-todo-control/scripts/eva_todo.mjs done task-smart-grid-audit
~~~
