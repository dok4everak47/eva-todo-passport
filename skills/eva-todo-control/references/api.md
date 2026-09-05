# EVA Todo API Reference

Base URL:

~~~text
<EVA_TODO_BASE_URL>
~~~

Auth:

~~~http
Authorization: Bearer <EVA_TODO_ADMIN_TOKEN>
~~~

Task fields:

- id: server UUID/string.
- title: short badge-visible task title.
- notes: details or Chinese text.
- status: todo, doing, done, or archived.
- priority: 0 low, 1 normal, 2 high, 3 urgent.
- urgent: boolean red-alert flag for badge display.
- dueAt: ISO date/time or null.
- tags: string array.
- sortOrder: integer display order.

Endpoints:

~~~http
GET    /health
GET    /tasks?status=todo&q=server&tag=ops&sinceVersion=0&includeDeleted=0
POST   /tasks
GET    /tasks/{id}
PATCH  /tasks/{id}
DELETE /tasks/{id}
POST   /tasks/{id}/complete
POST   /tasks/{id}/reopen
POST   /sync
GET    /events?sinceVersion=0&timeoutMs=15000
POST   /report
GET    /reports?limit=20
~~~

Create example:

~~~json
{
  "title": "SERVER MAINT",
  "notes": "服务器维护",
  "priority": 3,
  "urgent": true,
  "tags": ["ops"]
}
~~~
