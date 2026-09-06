package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Task struct {
	ID          string   `json:"id"`
	Title       string   `json:"title"`
	Notes       string   `json:"notes"`
	Status      string   `json:"status"`
	Priority    int      `json:"priority"`
	Urgent      bool     `json:"urgent"`
	DueAt       *string  `json:"dueAt"`
	Tags        []string `json:"tags"`
	SortOrder   int      `json:"sortOrder"`
	CompletedAt *string  `json:"completedAt"`
	Version     int      `json:"version"`
	CreatedAt   string   `json:"createdAt"`
	UpdatedAt   string   `json:"updatedAt"`
	DeletedAt   *string  `json:"deletedAt"`
}

type Report struct {
	ID              int64          `json:"id"`
	DeviceID        string         `json:"deviceId"`
	FirmwareVersion string         `json:"firmwareVersion"`
	LocalVersion    int            `json:"localVersion"`
	BatteryPercent  *int           `json:"batteryPercent"`
	WifiRssi        *int           `json:"wifiRssi"`
	TaskTotal       *int           `json:"taskTotal"`
	TaskDone        *int           `json:"taskDone"`
	Raw             map[string]any `json:"raw"`
	CreatedAt       string         `json:"createdAt"`
}

type Store struct {
	sync.Mutex
	Version      int             `json:"version"`
	NextReportID int64           `json:"nextReportId"`
	Tasks        map[string]Task `json:"tasks"`
	Reports      []Report        `json:"reports"`
	file         string
	changed      chan struct{}
}

func main() {
	store := loadStore(os.Getenv("DATA_FILE"))
	store.purgeExpired()
	go func() {
		ticker := time.NewTicker(time.Hour)
		defer ticker.Stop()
		for range ticker.C {
			store.purgeExpired()
		}
	}()
	mux := http.NewServeMux()
	mux.HandleFunc("/", webHandler)
	mux.HandleFunc("/index.html", webHandler)
	mux.HandleFunc("/skill.md", skillHandler)
	mux.HandleFunc("/api/v1/health", health(store))
	mux.HandleFunc("/api/v1/tasks", taskCollection(store))
	mux.HandleFunc("/api/v1/tasks/", taskResource(store))
	mux.HandleFunc("/api/v1/sync", syncHandler(store))
	mux.HandleFunc("/api/v1/events", eventsHandler(store))
	mux.HandleFunc("/api/v1/report", reportHandler(store))
	mux.HandleFunc("/api/v1/reports", reportsHandler(store))
	addr := os.Getenv("LISTEN_ADDR")
	if addr == "" {
		addr = ":8080"
	}
	log.Printf("eva-todo-server listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, cors(mux)))
}

func loadStore(file string) *Store {
	if file == "" {
		file = "data/eva-todo.json"
	}
	s := &Store{Tasks: map[string]Task{}, Reports: []Report{}, file: file, changed: make(chan struct{})}
	data, err := os.ReadFile(file)
	if err == nil && json.Unmarshal(data, s) == nil && s.Tasks != nil {
		if s.changed == nil {
			s.changed = make(chan struct{})
		}
		s.file = file
		return s
	}
	return s
}

func (s *Store) saveLocked() error {
	if dir := strings.TrimSuffix(s.file, "/"+lastPart(s.file)); dir != "" {
		_ = os.MkdirAll(dir, 0755)
	}
	b, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	tmp := s.file + ".tmp"
	if err = os.WriteFile(tmp, b, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, s.file)
}

func lastPart(path string) string {
	path = strings.ReplaceAll(path, "\\", "/")
	i := strings.LastIndex(path, "/")
	if i >= 0 {
		return path[i+1:]
	}
	return path
}

func (s *Store) bumpLocked() int {
	s.Version++
	old := s.changed
	s.changed = make(chan struct{})
	close(old)
	if err := s.saveLocked(); err != nil {
		log.Printf("persist failed: %v", err)
	}
	return s.Version
}

func (s *Store) purgeExpired() {
	cutoff := time.Now().UTC().Add(-24 * time.Hour)
	s.Lock()
	removed := false
	for id, task := range s.Tasks {
		if task.DeletedAt == nil {
			continue
		}
		deletedAt, err := time.Parse(time.RFC3339Nano, *task.DeletedAt)
		if err != nil {
			deletedAt, err = time.Parse(time.RFC3339, *task.DeletedAt)
		}
		if err == nil && deletedAt.Before(cutoff) {
			delete(s.Tasks, id)
			removed = true
		}
	}
	if removed {
		s.bumpLocked()
	}
	s.Unlock()
}

func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET,POST,PATCH,DELETE,OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Authorization,Content-Type,X-Device-Token,X-Admin-Token")
		if r.Method == "OPTIONS" {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func token(r *http.Request) string {
	a := strings.TrimSpace(r.Header.Get("Authorization"))
	if len(a) >= 7 && strings.EqualFold(a[:7], "Bearer ") {
		return strings.TrimSpace(a[7:])
	}
	if v := r.Header.Get("X-Admin-Token"); v != "" {
		return v
	}
	if v := r.Header.Get("X-Device-Token"); v != "" {
		return v
	}
	return r.URL.Query().Get("token")
}
func authorized(r *http.Request, admin bool) bool {
	want := os.Getenv("ADMIN_TOKEN")
	if admin {
		return want != "" && token(r) == want
	}
	device := os.Getenv("DEVICE_TOKEN")
	return want != "" && token(r) == want || device != "" && token(r) == device
}
func deny(w http.ResponseWriter, message string) {
	writeJSON(w, http.StatusUnauthorized, map[string]any{"ok": false, "error": map[string]string{"code": "unauthorized", "message": message}})
}
func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
func now() string { return time.Now().UTC().Format(time.RFC3339Nano) }
func errorJSON(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, map[string]any{"ok": false, "error": map[string]string{"code": code, "message": message}})
}

func health(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		s.Lock()
		v := s.Version
		s.Unlock()
		writeJSON(w, 200, map[string]any{"ok": true, "name": "eva-todo-api", "apiVersion": "v1", "serverTime": now(), "version": v})
	}
}

func decodeBody(r *http.Request, v any) error {
	dec := json.NewDecoder(io.LimitReader(r.Body, 32<<10))
	return dec.Decode(v)
}
func cleanStatus(v, fallback string) string {
	x := strings.ToLower(strings.TrimSpace(v))
	if x != "todo" && x != "doing" && x != "done" && x != "archived" {
		return fallback
	}
	return x
}
func cleanPriority(v int) int {
	if v < 0 {
		return 0
	}
	if v > 3 {
		return 3
	}
	return v
}
func cleanTags(tags []string) []string {
	out := []string{}
	for _, t := range tags {
		t = strings.TrimSpace(t)
		if t != "" {
			out = append(out, t)
		}
	}
	return out
}
func validTitle(t string) bool { return strings.TrimSpace(t) != "" }

func listLocked(s *Store, includeDeleted bool, q, status string) []Task {
	out := []Task{}
	for _, t := range s.Tasks {
		if !includeDeleted && (t.DeletedAt != nil || t.Status == "archived") {
			continue
		}
		if status != "" && t.Status != status {
			continue
		}
		needle := strings.ToLower(q)
		if needle != "" && !strings.Contains(strings.ToLower(t.Title+" "+t.Notes+" "+strings.Join(t.Tags, " ")), needle) {
			continue
		}
		out = append(out, t)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].SortOrder != out[j].SortOrder {
			return out[i].SortOrder < out[j].SortOrder
		}
		return out[i].CreatedAt < out[j].CreatedAt
	})
	return out
}

func taskCollection(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(r, r.Method != "GET") {
			deny(w, "Admin token required")
			return
		}
		if r.Method == "GET" {
			s.Lock()
			out, v := listLocked(s, r.URL.Query().Get("includeDeleted") == "1", r.URL.Query().Get("q"), cleanStatus(r.URL.Query().Get("status"), "")), s.Version
			s.Unlock()
			writeJSON(w, 200, map[string]any{"ok": true, "version": v, "tasks": out})
			return
		}
		if r.Method != "POST" {
			errorJSON(w, 405, "method_not_allowed", "Method not allowed")
			return
		}
		var input Task
		if decodeBody(r, &input) != nil || !validTitle(input.Title) {
			errorJSON(w, 400, "missing_title", "title is required")
			return
		}
		t := normalizeTask(input, "", now())
		s.Lock()
		t.ID = newID()
		s.Tasks[t.ID] = t
		t.Version = s.bumpLocked()
		s.Tasks[t.ID] = t
		s.Unlock()
		writeJSON(w, 201, map[string]any{"ok": true, "task": t})
	}
}

func normalizeTask(in Task, existingID, ts string) Task {
	in.ID = existingID
	in.Title = strings.TrimSpace(in.Title)
	if in.Notes == "" {
		in.Notes = ""
	}
	in.Status = cleanStatus(in.Status, "todo")
	in.Priority = cleanPriority(in.Priority)
	in.Urgent = in.Urgent && in.Status != "done"
	in.Tags = cleanTags(in.Tags)
	in.CreatedAt = ts
	in.UpdatedAt = ts
	if in.Status == "done" {
		in.CompletedAt = &ts
	}
	return in
}
func newID() string { return "task-" + strconv.FormatInt(time.Now().UnixNano(), 10) }

func taskResource(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(r, r.Method != "GET") {
			deny(w, "Bearer token required")
			return
		}
		parts := strings.Split(strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/v1/tasks/"), "/"), "/")
		if len(parts) == 0 || parts[0] == "" {
			errorJSON(w, 404, "not_found", "Task not found")
			return
		}
		id := parts[0]
		s.Lock()
		existing, ok := s.Tasks[id]
		s.Unlock()
		if !ok {
			errorJSON(w, 404, "not_found", "Task not found")
			return
		}
		if r.Method == "GET" && len(parts) == 1 {
			writeJSON(w, 200, map[string]any{"ok": true, "task": existing})
			return
		}
		if len(parts) == 2 && r.Method == "POST" && (parts[1] == "complete" || parts[1] == "reopen") {
			done := parts[1] == "complete"
			s.Lock()
			existing.Status = "todo"
			existing.CompletedAt = nil
			if done {
				existing.Status = "done"
				ts := now()
				existing.CompletedAt = &ts
				existing.Urgent = false
			}
			existing.UpdatedAt = now()
			s.Tasks[id] = existing
			existing.Version = s.bumpLocked()
			s.Tasks[id] = existing
			s.Unlock()
			writeJSON(w, 200, map[string]any{"ok": true, "task": existing})
			return
		}
		if r.Method == "PATCH" {
			var input map[string]any
			if decodeBody(r, &input) != nil {
				errorJSON(w, 400, "bad_json", "Request body must be valid JSON")
				return
			}
			s.Lock()
			applyPatch(&existing, input)
			existing.UpdatedAt = now()
			s.Tasks[id] = existing
			existing.Version = s.bumpLocked()
			s.Tasks[id] = existing
			s.Unlock()
			writeJSON(w, 200, map[string]any{"ok": true, "task": existing})
			return
		}
		if r.Method == "DELETE" {
			ts := now()
			existing.DeletedAt = &ts
			existing.Status = "archived"
			existing.UpdatedAt = ts
			s.Lock()
			s.Tasks[id] = existing
			existing.Version = s.bumpLocked()
			s.Tasks[id] = existing
			s.Unlock()
			writeJSON(w, 200, map[string]any{"ok": true, "task": existing})
			return
		}
		errorJSON(w, 405, "method_not_allowed", "Method not allowed")
	}
}

func applyPatch(t *Task, m map[string]any) {
	if v, ok := m["title"].(string); ok && validTitle(v) {
		t.Title = strings.TrimSpace(v)
	}
	if v, ok := m["notes"].(string); ok {
		t.Notes = v
	}
	if v, ok := m["status"].(string); ok {
		t.Status = cleanStatus(v, t.Status)
	}
	if v, ok := m["priority"].(float64); ok {
		t.Priority = cleanPriority(int(v))
	}
	if v, ok := m["urgent"].(bool); ok {
		t.Urgent = v
	}
	if v, ok := m["dueAt"].(string); ok {
		t.DueAt = &v
	} else if v, ok := m["dueAt"]; ok && v == nil {
		t.DueAt = nil
	}
	if v, ok := m["tags"].([]any); ok {
		tags := []string{}
		for _, x := range v {
			if z, ok := x.(string); ok {
				tags = append(tags, z)
			}
		}
		t.Tags = cleanTags(tags)
	}
	if v, ok := m["sortOrder"].(float64); ok {
		t.SortOrder = int(v)
	}
	if v, ok := m["deletedAt"].(string); ok {
		t.DeletedAt = &v
	}
	if t.Status == "done" {
		t.Urgent = false
	}
}

func syncHandler(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(r, false) {
			deny(w, "Bearer token required")
			return
		}
		var body struct {
			SinceVersion int `json:"sinceVersion"`
			Mutations    []struct {
				ClientMutationID any    `json:"clientMutationId"`
				Operation        string `json:"operation"`
				Task             Task   `json:"task"`
			} `json:"mutations"`
		}
		if decodeBody(r, &body) != nil {
			errorJSON(w, 400, "bad_json", "Request body must be valid JSON")
			return
		}
		accepted := []any{}
		for _, m := range body.Mutations {
			op := m.Operation
			if op == "" {
				op = "upsert"
			}
			if op != "upsert" && op != "complete" && op != "reopen" && op != "delete" {
				errorJSON(w, 400, "bad_operation", "Unsupported operation: "+op)
				return
			}
			t, err := mutateSync(s, m.Task, op)
			if err != nil {
				errorJSON(w, 400, "missing_title", err.Error())
				return
			}
			accepted = append(accepted, map[string]any{"clientMutationId": m.ClientMutationID, "operation": op, "task": t})
		}
		s.Lock()
		tasks, v := listLocked(s, false, "", ""), s.Version
		s.Unlock()
		writeJSON(w, 200, map[string]any{"ok": true, "serverTime": now(), "version": v, "accepted": accepted, "tasks": tasks})
	}
}

func mutateSync(s *Store, input Task, op string) (Task, error) {
	id := input.ID
	if id == "" {
		id = newID()
		input.ID = id
	}
	s.Lock()
	old, exists := s.Tasks[id]
	s.Unlock()
	if op == "delete" {
		if !exists {
			return Task{}, nil
		}
		ts := now()
		old.DeletedAt = &ts
		old.Status = "archived"
		old.UpdatedAt = ts
		s.Lock()
		s.Tasks[id] = old
		old.Version = s.bumpLocked()
		s.Tasks[id] = old
		s.Unlock()
		return old, nil
	}
	if op == "complete" || op == "reopen" {
		if !exists {
			return Task{}, errors.New("Task not found")
		}
		input = old
		input.Status = map[bool]string{true: "done", false: "todo"}[op == "complete"]
	}
	if input.Title == "" && exists {
		input.Title = old.Title
	}
	if !validTitle(input.Title) {
		return Task{}, errors.New("Synced task title is required")
	}
	if exists {
		input.CreatedAt = old.CreatedAt
	}
	input = normalizeTask(input, id, now())
	s.Lock()
	s.Tasks[id] = input
	input.Version = s.bumpLocked()
	s.Tasks[id] = input
	s.Unlock()
	return input, nil
}

func eventsHandler(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(r, false) {
			deny(w, "Bearer token required")
			return
		}
		since, _ := strconv.Atoi(r.URL.Query().Get("sinceVersion"))
		timeout, _ := strconv.Atoi(r.URL.Query().Get("timeoutMs"))
		if timeout < 0 {
			timeout = 0
		}
		if timeout > 25000 {
			timeout = 25000
		}
		deadline := time.NewTimer(time.Duration(timeout) * time.Millisecond)
		defer deadline.Stop()
		for {
			s.Lock()
			v, signal := s.Version, s.changed
			s.Unlock()
			if v > since {
				writeJSON(w, 200, map[string]any{"ok": true, "changed": true, "version": v, "serverTime": now()})
				return
			}
			select {
			case <-signal:
			case <-deadline.C:
				writeJSON(w, 200, map[string]any{"ok": true, "changed": false, "version": v, "serverTime": now()})
				return
			case <-r.Context().Done():
				return
			}
		}
	}
}

func reportHandler(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(r, false) {
			deny(w, "Bearer token required")
			return
		}
		var in map[string]any
		if decodeBody(r, &in) != nil {
			errorJSON(w, 400, "bad_json", "Request body must be valid JSON")
			return
		}
		getInt := func(k string) *int {
			if n, ok := in[k].(float64); ok {
				x := int(n)
				return &x
			}
			return nil
		}
		raw := map[string]any{}
		for k, v := range in {
			raw[k] = v
		}
		ts := now()
		s.Lock()
		s.NextReportID++
		rep := Report{ID: s.NextReportID, DeviceID: fmt.Sprint(in["deviceId"]), FirmwareVersion: fmt.Sprint(in["firmwareVersion"]), LocalVersion: intValue(in["localVersion"]), BatteryPercent: getInt("batteryPercent"), WifiRssi: getInt("wifiRssi"), TaskTotal: getInt("taskTotal"), TaskDone: getInt("taskDone"), Raw: raw, CreatedAt: ts}
		s.Reports = append(s.Reports, rep)
		_ = s.saveLocked()
		s.Unlock()
		writeJSON(w, 201, map[string]any{"ok": true, "report": rep})
	}
}
func intValue(v any) int { n, _ := v.(float64); return int(n) }
func reportsHandler(s *Store) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !authorized(r, true) {
			deny(w, "Admin token required")
			return
		}
		limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
		if limit <= 0 {
			limit = 20
		}
		if limit > 100 {
			limit = 100
		}
		s.Lock()
		start := len(s.Reports) - limit
		if start < 0 {
			start = 0
		}
		out := append([]Report(nil), s.Reports[start:]...)
		s.Unlock()
		sort.Slice(out, func(i, j int) bool { return out[i].CreatedAt > out[j].CreatedAt })
		writeJSON(w, 200, map[string]any{"ok": true, "reports": out})
	}
}
