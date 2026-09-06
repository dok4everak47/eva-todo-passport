# EVA Todo API

Cloudflare Worker and self-hosted Go service for the device, Web UI, and AI agents.

Base URL after deployment:

~~~text
<worker-or-custom-domain>/api/v1
~~~

The self-hosted Go server uses the same paths, JSON shapes, and two-token model:

~~~text
http://<host>:8080/api/v1
~~~

Authentication:

~~~http
Authorization: Bearer <ADMIN_TOKEN or DEVICE_TOKEN>
~~~

- ADMIN_TOKEN: full read/write access, used by the web UI and AI agents.
- DEVICE_TOKEN: device sync/report access, intended for the ESP32 firmware.
- Worker deployments store tokens as Worker Secrets; Go deployments read them
  from container environment variables. Do not put them in Git or final answers.
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

Optional filters: status, q, sinceVersion, includeDeleted=1. `q` searches title, notes, and tags.

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

### Get One Task

~~~http
GET /api/v1/tasks/{id}
~~~

Requires ADMIN_TOKEN or DEVICE_TOKEN.

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

Open the Worker root URL shown by `wrangler deploy` after deployment:

~~~text
<worker-or-custom-domain>/
~~~

Enter ADMIN_TOKEN in the password field. The token is stored in browser localStorage for convenience. The ESP32 configuration page stores only DEVICE_TOKEN; it never needs ADMIN_TOKEN.

The web UI `AI SKILL` button copies the same self-contained Skill document available
at `GET /skill.md`. The downloadable Markdown contains the current API base URL,
setup instructions for `ADMIN_TOKEN` and `DEVICE_TOKEN`, their permission boundaries,
and safe request examples. It never includes the actual secret values.

## Deployment

The API has two interchangeable deployment options. Both expose the same
`/api/v1` contract, web UI, and `/skill.md` download endpoint. Configure a
device with the base URL and `DEVICE_TOKEN` belonging to the backend it uses.

### Option 1: Cloudflare Worker + D1

Cloudflare resources:

- Worker name: eva-todo-api
- D1 database name: eva_todo_db
- Binding name: DB
- Required Worker Secrets: ADMIN_TOKEN, DEVICE_TOKEN

Deploy flow:

~~~powershell
cd server
npm install
npm run secrets:init
$env:CLOUDFLARE_API_TOKEN = "<your-cloudflare-api-token>"
npm run deploy:full
~~~

deploy:full creates or reuses the D1 database, applies migrations remotely, uploads Worker secrets from server/.dev.vars, deploys the Worker, and updates main/firmware_private.h with the deployed /api/v1 URL when possible.

### Option 2: Docker self-hosted Go server

The compatible Go implementation is in `server-go/`. It stores its JSON data in
the mounted `server-go/data` directory and does not require a Cloudflare account.
Set separate values for `ADMIN_TOKEN` and `DEVICE_TOKEN` in a local `.env` file:

~~~powershell
cd server-go
Copy-Item .env.example .env
# Edit .env and replace both example token values.
docker compose up -d --build
~~~

The container receives the same token names as environment variables. Mount
`/data` through the supplied Compose file so restarts do not lose tasks or
reports. Do not use the example token values in a public deployment.

The default endpoints are:

- API: `http://localhost:8080/api/v1`
- Web UI: `http://localhost:8080/`
- Skill: `http://localhost:8080/skill.md`

For public access, put the container behind an HTTPS reverse proxy and restrict
the Web UI and admin endpoints to trusted users. Do not commit `.env` or reuse
example token values in production.

## AI Skill

The service is designed for AI Agent control as well as human web management.
Both deployment types expose a generated Skill document from the current host:

- Cloudflare Worker: `https://<your-domain>/skill.md`
- Docker Go server: `http://<your-host>:8080/skill.md`

The document contains the matching `/api/v1` base URL, supported operations,
token roles, and safe request examples. It is generated from the request host,
so an agent can use a custom domain, a private LAN deployment, or a temporary
development URL without editing a hard-coded production address. The Web UI's
`AI SKILL` button copies this same document; the endpoint is also directly
downloadable and does not require a token. The AI Agent still needs
`ADMIN_TOKEN` for task-management requests.

Project-local Skill:

~~~text
skills/eva-todo-control/
~~~

Expected environment:

~~~powershell
$env:EVA_TODO_BASE_URL = "https://<worker-or-custom-domain>/api/v1"
$env:EVA_TODO_ADMIN_TOKEN = "<ADMIN_TOKEN>"
~~~

`EVA_TODO_ADMIN_TOKEN` is the credential an AI Agent uses for task management
and device report reads. `EVA_TODO_DEVICE_TOKEN` is intentionally narrower and
is reserved for the firmware's sync, event polling, and report upload. Never
give the device token to an agent that needs to create or modify tasks.

Recommended Agent workflow:

1. Download `/skill.md` from the selected deployment and load its current base URL.
2. Call `GET /health`, then `GET /tasks` before changing anything.
3. Use `POST /tasks`, `PATCH /tasks/{id}`, `POST /tasks/{id}/complete`,
   `POST /tasks/{id}/reopen`, or `DELETE /tasks/{id}` as appropriate.
4. After every write, call `GET /tasks` or `GET /tasks/{id}` and report the
   resulting task id and status to the user.
5. Ask for confirmation before destructive deletes when the user's request is
   ambiguous; DELETE is a soft delete but is purged after 24 hours.

Examples:

~~~powershell
node skills/eva-todo-control/scripts/eva_todo.mjs list
node skills/eva-todo-control/scripts/eva_todo.mjs add --title "WRITE DOCS" --notes "补 API 文档" --priority 2 --tags docs,ai
node skills/eva-todo-control/scripts/eva_todo.mjs done task-smart-grid-audit
~~~
