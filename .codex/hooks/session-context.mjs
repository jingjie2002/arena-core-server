import { existsSync, readdirSync, readFileSync } from "fs";
import { join } from "path";
import { changedFilesFromStatus, git, projectRoot, readState, reviewDir } from "./common.mjs";

const state = readState();
const branch = git(["rev-parse", "--abbrev-ref", "HEAD"]) || "非 git 目录";
const lastCommit = git(["log", "-1", "--oneline"]);
const changed = changedFilesFromStatus();
const lines = [
  "[Codex Harness SessionStart]",
  `分支: ${branch}`,
  `Harness: mode=${state.mode}, phase=${state.phase}, since=${state.since || "未记录"}`,
];

if (lastCommit) lines.push(`最近提交: ${lastCommit}`);
if (changed.length > 0) {
  lines.push(`未提交变更: ${changed.length} 个文件`);
  lines.push(...changed.slice(0, 12).map((file) => `  ${file}`));
  if (changed.length > 12) lines.push(`  ...以及其他 ${changed.length - 12} 个文件`);
} else {
  lines.push("未提交变更: 无");
}

const statusPath = join(projectRoot, ".codex", "context", "current-status.md");
if (existsSync(statusPath)) {
  const status = readFileSync(statusPath, "utf8")
    .split(/\r?\n/)
    .slice(0, 18)
    .join("\n")
    .trim();
  if (status) lines.push("", "当前状态摘要:", status);
}

if (existsSync(reviewDir)) {
  const latest = readdirSync(reviewDir)
    .filter((file) => file.endsWith(".md"))
    .sort()
    .reverse()
    .slice(0, 3);
  if (latest.length > 0) {
    lines.push("", "最近 Harness 自动审查:");
    lines.push(...latest.map((file) => `  ${file}`));
  }
}

lines.push("[/Codex Harness SessionStart]");
process.stdout.write(`${lines.join("\n")}\n`);
