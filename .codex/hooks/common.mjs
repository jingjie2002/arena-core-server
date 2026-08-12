import { execFileSync, spawnSync } from "child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "fs";
import { basename, dirname, extname, join, normalize, resolve } from "path";
import { fileURLToPath } from "url";

const hookDir = dirname(fileURLToPath(import.meta.url));
export const projectRoot = resolve(hookDir, "../..");
export const codexDir = join(projectRoot, ".codex");
export const statePath = join(codexDir, "harness-state.json");
export const reviewDir = join(codexDir, "harness-reviews");

export function readStdinJson() {
  let raw = "";
  try {
    raw = readFileSync(0, "utf8").trim();
  } catch {
    return { raw: "", data: null };
  }
  if (!raw) return { raw, data: null };
  try {
    return { raw, data: JSON.parse(raw) };
  } catch {
    return { raw, data: null };
  }
}

export function readState() {
  const fallback = { phase: "build", mode: "full", since: "" };
  if (!existsSync(statePath)) return fallback;
  try {
    return { ...fallback, ...JSON.parse(readFileSync(statePath, "utf8")) };
  } catch {
    return fallback;
  }
}

export function ensureDirs() {
  mkdirSync(codexDir, { recursive: true });
  mkdirSync(reviewDir, { recursive: true });
}

export function git(args, timeout = 5000) {
  try {
    return execFileSync("git", args, {
      cwd: projectRoot,
      encoding: "utf8",
      timeout,
      stdio: ["ignore", "pipe", "ignore"],
    }).trim();
  } catch {
    return "";
  }
}

export function spawnLocal(command, args, timeout = 10000) {
  return spawnSync(command, args, {
    cwd: projectRoot,
    encoding: "utf8",
    timeout,
    shell: process.platform === "win32",
    stdio: ["ignore", "pipe", "pipe"],
  });
}

export function stringifyPayload(payload, raw = "") {
  try {
    return JSON.stringify(payload ?? {});
  } catch {
    return raw || "";
  }
}

function firstString(...values) {
  for (const value of values) {
    if (typeof value === "string" && value.trim()) return value;
  }
  return "";
}

export function getToolName(payload) {
  return firstString(
    payload?.tool,
    payload?.tool_name,
    payload?.toolName,
    payload?.name,
    payload?.event?.tool,
    payload?.params?.tool,
    payload?.params?.tool_name
  );
}

export function getToolInput(payload) {
  return (
    payload?.input ??
    payload?.tool_input ??
    payload?.toolInput ??
    payload?.arguments ??
    payload?.args ??
    payload?.params?.input ??
    payload?.params?.arguments ??
    payload?.params ??
    {}
  );
}

export function getCommand(payload) {
  const input = getToolInput(payload);
  if (typeof input === "string") return input;
  return firstString(input?.command, input?.cmd, input?.script, payload?.command);
}

export function getFilePath(payload) {
  const input = getToolInput(payload);
  if (typeof input === "string") return "";
  return firstString(
    input?.file_path,
    input?.path,
    input?.filepath,
    input?.target_file,
    input?.targetPath,
    payload?.file_path,
    payload?.path
  );
}

export function normalizeRelativePath(filePath) {
  if (!filePath) return "";
  const resolved = resolve(projectRoot, filePath);
  return normalize(resolved);
}

export function isProtectedPath(filePath) {
  const full = normalizeRelativePath(filePath);
  if (!full) return false;
  const name = basename(full).toLowerCase();
  if ([".env.example", ".env.sample", ".env.template", ".env.defaults"].includes(name)) {
    return false;
  }
  return (
    name === ".env" ||
    name === ".env.local" ||
    name.startsWith(".env.") ||
    name.endsWith(".pem") ||
    name.endsWith(".key") ||
    name.includes("secret") ||
    name.includes("credential")
  );
}

export function payloadMentionsProtectedFile(serialized) {
  return /\*\*\*\s+(Add|Update|Delete)\s+File:\s+.*(^|[/\\])?\.env(?!\.(example|sample|template|defaults))(\.|$)/im.test(serialized) ||
    /(^|["'\s])\.env(?!\.(example|sample|template|defaults))(\.local|\.[A-Za-z0-9_-]+)?(["'\s]|$)/im.test(serialized);
}

export function block(reason, details = {}) {
  const payload = {
    decision: "block",
    block: true,
    reason,
    ...details,
  };
  process.stdout.write(`${JSON.stringify(payload)}\n`);
  process.stderr.write(`${reason}\n`);
  process.exit(2);
}

export function note(message) {
  process.stdout.write(`${message}\n`);
}

export function writeText(path, content) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, content, "utf8");
}

export function changedFilesFromStatus() {
  const status = git(["status", "--porcelain"]);
  if (!status) return [];
  return status
    .split(/\r?\n/)
    .map((line) => line.slice(3).trim())
    .filter(Boolean)
    .map((file) => file.replace(/^"|"$/g, ""));
}

export function localTimestampForFile() {
  const now = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}`;
}

export function fileExtension(filePath) {
  return extname(filePath || "").toLowerCase();
}
