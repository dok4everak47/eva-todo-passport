package main

import (
	"encoding/json"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Task struct {
	ID string `json:"id"`; Title string `json:"title"`; Notes string `json:"notes"`
	Status string `json:"status"`; Priority int `json:"priority"`; Urgent bool `json:"urgent"`
	Tags []string `json:"tags"`; SortOrder int `json:"sortOrder"`; Version int `json:"version"`
	CreatedAt string `json:"createdAt"`; UpdatedAt string `json:"updatedAt"`; DeletedAt *string `json:"deletedAt"`
}
type Store struct { sync.Mutex; Version int; Tasks map[string]Task }

func main() {
	store := &Store{Tasks: map[string]Task{}}
	addr := os.Getenv("LISTEN_ADDR"); if addr == "" { addr = ":8080" }
	mux := http.NewServeMux(); mux.HandleFunc("/api/v1/health", health(store)); mux.HandleFunc("/api/v1/tasks", tasks(store)); mux.HandleFunc("/api/v1/", taskAction(store))
	log.Printf("eva-todo-server listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, cors(mux)))
}
func cors(next http.Handler) http.Handler { return http.HandlerFunc(func(w http.ResponseWriter,r *http.Request) { w.Header().Set("Access-Control-Allow-Origin","*"); w.Header().Set("Access-Control-Allow-Headers","Authorization,Content-Type"); if r.Method=="OPTIONS" { w.WriteHeader(204); return }; next.ServeHTTP(w,r) }) }
func auth(r *http.Request) bool { want:=os.Getenv("DEVICE_TOKEN"); if want=="" { want=os.Getenv("ADMIN_TOKEN") }; return want=="" || strings.TrimPrefix(r.Header.Get("Authorization"),"Bearer ")==want }
func writeJSON(w http.ResponseWriter, status int, v any) { w.Header().Set("Content-Type","application/json"); w.WriteHeader(status); _=json.NewEncoder(w).Encode(v) }
func health(s *Store) http.HandlerFunc { return func(w http.ResponseWriter,r *http.Request) { s.Lock(); v:=s.Version; s.Unlock(); writeJSON(w,200,map[string]any{"ok":true,"name":"eva-todo-api","apiVersion":"v1","serverTime":time.Now().UTC().Format(time.RFC3339Nano),"version":v}) } }
func tasks(s *Store) http.HandlerFunc { return func(w http.ResponseWriter,r *http.Request) { if !auth(r) { writeJSON(w,401,map[string]any{"ok":false,"error":map[string]string{"code":"unauthorized","message":"Bearer token required"}}); return }; if r.Method=="GET" { s.Lock(); out:=[]Task{}; for _,t:=range s.Tasks { if t.DeletedAt==nil { out=append(out,t) } }; v:=s.Version; s.Unlock(); writeJSON(w,200,map[string]any{"ok":true,"version":v,"tasks":out}); return }; if r.Method!="POST" { http.NotFound(w,r); return }; var t Task; if json.NewDecoder(r.Body).Decode(&t)!=nil || strings.TrimSpace(t.Title)=="" { writeJSON(w,400,map[string]any{"ok":false,"error":map[string]string{"code":"missing_title","message":"title is required"}}); return }; s.Lock(); defer s.Unlock(); s.Version++; if t.ID=="" { t.ID="task-"+strconv.FormatInt(time.Now().UnixNano(),10) }; now:=time.Now().UTC().Format(time.RFC3339Nano); t.Version=s.Version; t.CreatedAt=now; t.UpdatedAt=now; if t.Status=="" { t.Status="todo" }; s.Tasks[t.ID]=t; writeJSON(w,201,map[string]any{"ok":true,"task":t}) } }
func taskAction(s *Store) http.HandlerFunc { return func(w http.ResponseWriter,r *http.Request) { if !auth(r) { writeJSON(w,401,map[string]any{"ok":false,"error":"unauthorized"}); return }; p:=strings.TrimPrefix(r.URL.Path,"/api/v1/tasks/"); parts:=strings.Split(strings.Trim(p,"/"),"/"); if len(parts)==0 || parts[0]=="" { http.NotFound(w,r); return }; id:=parts[0]; s.Lock(); defer s.Unlock(); t,ok:=s.Tasks[id]; if !ok { http.NotFound(w,r); return }; if len(parts)==2 && r.Method=="POST" { if parts[1]=="complete" { t.Status="done"; t.Urgent=false }; if parts[1]=="reopen" { t.Status="todo" }; s.Version++; t.Version=s.Version; t.UpdatedAt=time.Now().UTC().Format(time.RFC3339Nano); s.Tasks[id]=t; writeJSON(w,200,map[string]any{"ok":true,"task":t}); return }; if r.Method=="DELETE" { now:=time.Now().UTC().Format(time.RFC3339Nano); t.DeletedAt=&now; s.Version++; t.Version=s.Version; s.Tasks[id]=t; writeJSON(w,200,map[string]any{"ok":true,"task":t}); return }; http.NotFound(w,r) } }
