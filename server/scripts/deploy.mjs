#!/usr/bin/env node
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const serverDir = resolve(here, "..");
const rootDir = resolve(serverDir, "..");
const wranglerToml = resolve(serverDir, "wrangler.toml");
const firmwarePrivate = resolve(rootDir, "main", "firmware_private.h");
const dbName = "eva_todo_db";
const dbIdPlaceholder = "00000000-0000-0000-0000-000000000000";
const npx = process.platform === "win32" ? "npx.cmd" : "npx";

if (!process.env.CLOUDFLARE_API_TOKEN) {
  fail("CLOUDFLARE_API_TOKEN is not set. Export it for this process; do not commit it.");
}
if (!existsSync(resolve(serverDir, ".dev.vars"))) {
  fail("server/.dev.vars is missing. Run: npm run secrets:init");
}

function fail(message) {
  console.error(message);
  process.exit(1);
}

function run(args, options = {}) {
  console.log("> npx wrangler " + args.join(" "));
  const res = spawnSync(npx, ["wrangler", ...args], {
    cwd: serverDir,
    env: process.env,
    encoding: "utf8",
    stdio: options.capture ? ["ignore", "pipe", "pipe"] : "inherit"
  });
  if (res.status !== 0 && !options.allowFailure) {
    if (options.capture && res.stderr) process.stderr.write(res.stderr);
    process.exit(res.status || 1);
  }
  return res;
}

function parseJsonOutput(text) {
  const trimmed = text.trim();
  if (!trimmed) return null;
  try {
    return JSON.parse(trimmed);
  } catch {
    const start = trimmed.indexOf("[");
    const end = trimmed.lastIndexOf("]");
    if (start >= 0 && end > start) return JSON.parse(trimmed.slice(start, end + 1));
    return null;
  }
}

function readConfiguredDbId() {
  const toml = readFileSync(wranglerToml, "utf8");
  const match = toml.match(/database_id\s*=\s*"([^"]+)"/);
  return match?.[1] || "";
}

function writeDbId(id) {
  const toml = readFileSync(wranglerToml, "utf8");
  writeFileSync(wranglerToml, toml.replace(/database_id\s*=\s*"[^"]+"/, 'database_id = "' + id + '"'));
}

function findDbIdFromList() {
  const res = run(["d1", "list", "--json"], { capture: true, allowFailure: true });
  if (res.status !== 0) return "";
  const data = parseJsonOutput(res.stdout);
  const list = Array.isArray(data) ? data : (data?.result || data?.databases || []);
  const db = list.find(item => item.name === dbName || item.database_name === dbName);
  return db?.uuid || db?.id || db?.database_id || "";
}

function createDb() {
  const res = run(["d1", "create", dbName], { capture: true });
  const output = res.stdout + "\n" + res.stderr;
  const match = output.match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i);
  if (!match) fail("D1 database was created but its database_id could not be parsed.");
  return match[0];
}

let dbId = readConfiguredDbId();
if (!dbId || dbId === dbIdPlaceholder) {
  dbId = findDbIdFromList() || createDb();
  writeDbId(dbId);
  console.log("Updated wrangler.toml database_id for " + dbName + ".");
} else {
  console.log("Using configured D1 database_id for " + dbName + ".");
}

run(["d1", "migrations", "apply", dbName, "--remote"]);
const deploy = run(["deploy", "--secrets-file", ".dev.vars"], { capture: true });
process.stdout.write(deploy.stdout);
process.stderr.write(deploy.stderr);

const urlMatch = deploy.stdout.match(/https:\/\/[^\s]+\.workers\.dev/i);
if (urlMatch && existsSync(firmwarePrivate)) {
  const apiBase = urlMatch[0].replace(/\/$/, "") + "/api/v1";
  const before = readFileSync(firmwarePrivate, "utf8");
  const after = before.replace(/#define TODO_API_BASE_URL "[^"]*"/, "#define TODO_API_BASE_URL " + JSON.stringify(apiBase));
  if (after !== before) {
    writeFileSync(firmwarePrivate, after);
    console.log("Updated main/firmware_private.h with deployed API base URL.");
  }
}
