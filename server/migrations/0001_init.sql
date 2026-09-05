CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

INSERT OR IGNORE INTO meta (key, value) VALUES ('version', '5');

CREATE TABLE IF NOT EXISTS tasks (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  notes TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'todo' CHECK (status IN ('todo', 'doing', 'done', 'archived')),
  priority INTEGER NOT NULL DEFAULT 1 CHECK (priority BETWEEN 0 AND 3),
  urgent INTEGER NOT NULL DEFAULT 0 CHECK (urgent IN (0, 1)),
  due_at TEXT,
  tags TEXT NOT NULL DEFAULT '[]',
  sort_order INTEGER NOT NULL DEFAULT 0,
  completed_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  deleted_at TEXT,
  version INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_tasks_version ON tasks(version);
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_deleted_at ON tasks(deleted_at);
CREATE INDEX IF NOT EXISTS idx_tasks_sort ON tasks(sort_order, created_at);

CREATE TABLE IF NOT EXISTS device_reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  firmware_version TEXT,
  local_version INTEGER,
  battery_percent INTEGER,
  wifi_rssi INTEGER,
  task_total INTEGER,
  task_done INTEGER,
  raw_json TEXT NOT NULL,
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_reports_device_created ON device_reports(device_id, created_at DESC);

INSERT OR IGNORE INTO tasks
  (id, title, notes, status, priority, urgent, due_at, tags, sort_order, completed_at,
   created_at, updated_at, deleted_at, version)
VALUES
  ('task-yaoshan-mw', 'YAO-SHAN MW TRANS.', '巡检尧山微波传输链路', 'done', 1, 0, NULL, '["ops","network"]', 10, '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', NULL, 1),
  ('task-tailscale-cert', 'TAILSCALE CERT.', '续签 Tailscale 节点证书', 'todo', 2, 0, NULL, '["ops","cert"]', 20, NULL, '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', NULL, 2),
  ('task-go-api-test', 'GO API TEST', '编写 Go 后端 API 测试脚本', 'todo', 2, 0, NULL, '["dev","api"]', 30, NULL, '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', NULL, 3),
  ('task-smart-grid-audit', 'SMART GRID AUD.', '审核年度智能配电监控方案', 'todo', 3, 1, NULL, '["ops","audit"]', 40, NULL, '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', NULL, 4),
  ('task-backup-records', 'BACKUP RECORDS', '备份本地任务清单', 'done', 1, 0, NULL, '["office","sync"]', 50, '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', '2026-09-05T00:00:00.000Z', NULL, 5);
