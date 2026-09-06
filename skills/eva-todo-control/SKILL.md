---
name: eva-todo-control
description: Control the EVA Todo Cloudflare API for listing, creating, updating, completing, reopening, deleting badge tasks, and reading device reports. Use when the user asks an AI agent to manage tasks for the ESP32/FoloToy EVA Todo badge.
---

# EVA Todo Control

Use this skill to operate an EVA Todo API. The API is compatible with both the
Cloudflare Worker and the self-hosted Go server, and is also used by the ESP32
badge and the web UI.

## Required Configuration

Load these values from environment variables, a local ignored env file, or the user's secret manager:

- EVA_TODO_BASE_URL: API base URL ending in /api/v1.
- EVA_TODO_ADMIN_TOKEN: admin token for full task control.
- EVA_TODO_DEVICE_TOKEN: device token for reading, syncing, and reporting.

`EVA_TODO_ADMIN_TOKEN` is required for creating, editing, completing,
reopening, and deleting tasks, and for reading reports. The device token is
intended for firmware: it can read tasks, sync task state, and upload reports,
but cannot perform admin-only writes.

The API uses `Authorization: Bearer <token>`. Set `EVA_TODO_BASE_URL` to the
actual `/api/v1` URL of the deployment you want to control. For example:

```text
https://your-worker.example.workers.dev/api/v1
http://your-self-hosted-server:8080/api/v1
```

Do not assume a particular hostname; the same Skill works with custom domains,
Cloudflare Workers, and Docker deployments.

Keep both tokens in environment variables or a local secret store. Never print
tokens, commit tokens, or ask a user to paste a token into a public prompt.

## Preferred Tooling

Use scripts/eva_todo.mjs from this skill when available:

~~~powershell
node scripts/eva_todo.mjs list
node scripts/eva_todo.mjs add --title "WRITE DOCS" --notes "补 API 文档" --priority 2 --tags docs,ai
node scripts/eva_todo.mjs done task-id
node scripts/eva_todo.mjs reopen task-id
node scripts/eva_todo.mjs update task-id --priority 3 --urgent true
node scripts/eva_todo.mjs delete task-id
node scripts/eva_todo.mjs reports
~~~

If the helper script is not available, call the HTTP API directly with:

~~~http
Authorization: Bearer <EVA_TODO_ADMIN_TOKEN>
Content-Type: application/json
~~~

## Task Writing Rules

- Keep title short, uppercase, and ASCII-friendly when the task should appear clearly on the badge.
- Put Chinese details, longer instructions, URLs, or context in notes.
- Use priority: 3 and urgent: true only for genuinely time-sensitive tasks.
- Prefer stable tags such as daily, ops, docs, ai, errand, office.
- After any write, verify with list or GET /tasks/{id}.

## Common Operations

- List active tasks: GET /tasks
- Create task: POST /tasks
- Update fields: PATCH /tasks/{id}
- Mark done: POST /tasks/{id}/complete
- Reopen: POST /tasks/{id}/reopen
- Soft delete: DELETE /tasks/{id}
- Device reports: GET /reports?limit=20

See references/api.md for the API contract.
