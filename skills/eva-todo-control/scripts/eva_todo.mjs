#!/usr/bin/env node

const baseUrl = (process.env.EVA_TODO_BASE_URL || "").replace(/\/$/, "");
const token = process.env.EVA_TODO_ADMIN_TOKEN || "";

if (!baseUrl || !token) {
  console.error("Set EVA_TODO_BASE_URL and EVA_TODO_ADMIN_TOKEN first.");
  process.exit(1);
}

const [command, ...rest] = process.argv.slice(2);
const args = parseArgs(rest);

function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith("--")) {
      out._.push(a);
      continue;
    }
    const key = a.slice(2);
    const next = argv[i + 1];
    out[key] = next && !next.startsWith("--") ? argv[++i] : "true";
  }
  return out;
}

function bool(v) {
  return v === true || v === "true" || v === "1" || v === 1;
}

function tags(v) {
  return String(v || "").split(",").map(x => x.trim()).filter(Boolean);
}

async function request(path, init = {}) {
  const res = await fetch(baseUrl + path, {
    ...init,
    headers: {
      "authorization": "Bearer " + token,
      "content-type": "application/json",
      ...(init.headers || {})
    }
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok || body.ok === false) {
    throw new Error(body.error?.message || res.statusText);
  }
  return body;
}

function print(data) {
  console.log(JSON.stringify(data, null, 2));
}

async function main() {
  if (!command || command === "help") {
    console.log("Commands: list, add, update, done, reopen, delete, reports, health");
    return;
  }

  if (command === "health") {
    print(await request("/health"));
    return;
  }

  if (command === "list") {
    const p = new URLSearchParams();
    if (args.status) p.set("status", args.status);
    if (args.q) p.set("q", args.q);
    if (args.tag) p.set("tag", args.tag);
    print(await request("/tasks" + (p.size ? "?" + p.toString() : "")));
    return;
  }

  if (command === "add") {
    if (!args.title) throw new Error("--title is required");
    print(await request("/tasks", {
      method: "POST",
      body: JSON.stringify({
        title: args.title,
        notes: args.notes || "",
        status: args.status || "todo",
        priority: args.priority == null ? 1 : Number(args.priority),
        urgent: bool(args.urgent),
        dueAt: args["due-at"] || null,
        tags: tags(args.tags),
        sortOrder: args["sort-order"] == null ? undefined : Number(args["sort-order"])
      })
    }));
    return;
  }

  const id = args._[0];
  if (["update", "done", "reopen", "delete"].includes(command) && !id) {
    throw new Error("task id is required");
  }

  if (command === "update") {
    const patch = {};
    for (const key of ["title", "notes", "status"]) {
      if (args[key] != null) patch[key] = args[key];
    }
    if (args.priority != null) patch.priority = Number(args.priority);
    if (args.urgent != null) patch.urgent = bool(args.urgent);
    if (args["due-at"] != null) patch.dueAt = args["due-at"] || null;
    if (args.tags != null) patch.tags = tags(args.tags);
    if (args["sort-order"] != null) patch.sortOrder = Number(args["sort-order"]);
    print(await request("/tasks/" + encodeURIComponent(id), { method: "PATCH", body: JSON.stringify(patch) }));
    return;
  }

  if (command === "done") {
    print(await request("/tasks/" + encodeURIComponent(id) + "/complete", { method: "POST" }));
    return;
  }

  if (command === "reopen") {
    print(await request("/tasks/" + encodeURIComponent(id) + "/reopen", { method: "POST" }));
    return;
  }

  if (command === "delete") {
    print(await request("/tasks/" + encodeURIComponent(id), { method: "DELETE" }));
    return;
  }

  if (command === "reports") {
    const limit = args.limit || "20";
    print(await request("/reports?limit=" + encodeURIComponent(limit)));
    return;
  }

  throw new Error("unknown command: " + command);
}

main().catch(err => {
  console.error(err.message);
  process.exit(1);
});
