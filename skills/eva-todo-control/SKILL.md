---
name: eva-todo-control
description: Control the EVA Todo Cloudflare API for listing, creating, updating, completing, reopening, deleting badge tasks, and reading device reports. Use when the user asks an AI agent to manage tasks for the ESP32/FoloToy EVA Todo badge.
---

# EVA Todo Control

Use this skill to operate the EVA Todo API used by the ESP32 badge and web UI.

## Required Configuration

Load these values from environment variables, a local ignored env file, or the user's secret manager:

- EVA_TODO_BASE_URL: API base URL ending in /api/v1.
- EVA_TODO_ADMIN_TOKEN: admin token for full task control.

Never print tokens, commit tokens, or include them in generated documentation.

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
