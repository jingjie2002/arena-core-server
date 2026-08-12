import { existsSync, readFileSync } from "fs";
import { join } from "path";
import {
  changedFilesFromStatus,
  ensureDirs,
  git,
  localTimestampForFile,
  projectRoot,
  readState,
  reviewDir,
  writeText,
} from "./common.mjs";

ensureDirs();

const state = readState();
const changed = Array.from(new Set(changedFilesFromStatus()));
const diff = `${git(["diff", "--unified=0"], 10000)}\n${git(["diff", "--cached", "--unified=0"], 10000)}`;
const stat = `${git(["diff", "--numstat"], 10000)}\n${git(["diff", "--cached", "--numstat"], 10000)}`;

let insertions = 0;
let deletions = 0;
for (const line of stat.split(/\r?\n/)) {
  const [add, del] = line.trim().split(/\s+/);
  const addNum = Number.parseInt(add, 10);
  const delNum = Number.parseInt(del, 10);
  if (Number.isFinite(addNum)) insertions += addNum;
  if (Number.isFinite(delNum)) deletions += delNum;
}

const sensitive = changed.filter((file) => {
  const lower = file.toLowerCase();
  return lower.includes(".env") || lower.includes("secret") || lower.includes("credential") || lower.endsWith(".pem") || lower.endsWith(".key");
});

const debugPatterns = [
  { pattern: /\bconsole\.\w+\s*\(/, label: "console.* 调试输出" },
  { pattern: /\bdebugger\b/, label: "debugger 语句" },
  { pattern: /\bTODO\b/, label: "TODO 标记" },
  { pattern: /\bFIXME\b/, label: "FIXME 标记" },
];
const debugHits = debugPatterns.filter((item) => item.pattern.test(diff)).map((item) => item.label);

const depFiles = ["package.json", "pyproject.toml", "go.mod", "Cargo.toml", "Gemfile", "pom.xml", "build.gradle"];
const lockFiles = ["package-lock.json", "pnpm-lock.yaml", "yarn.lock", "bun.lock", "poetry.lock", "go.sum", "Cargo.lock", "Gemfile.lock"];
const depChanged = changed.some((file) => depFiles.some((dep) => file.endsWith(dep)));
const lockChanged = changed.some((file) => lockFiles.some((lock) => file.endsWith(lock)));

const maxFiles = state.phase === "fix" ? 5 : state.phase === "design" ? 20 : 10;
const maxLines = state.mode === "hotfix" || state.mode === "tweak" ? Infinity : 500;
const totalLines = insertions + deletions;
const flags = [];

if (sensitive.length > 0) flags.push("涉及敏感文件，请确认没有写入真实密钥或生产配置。");
if (changed.length > maxFiles) flags.push(`改动文件数 ${changed.length} 超过当前阈值 ${maxFiles}。`);
if (totalLines > maxLines) flags.push(`改动行数 ${totalLines} 超过当前阈值 ${maxLines}。`);
if (debugHits.length > 0 && state.phase !== "design" && state.mode !== "tweak") flags.push(`变更中包含调试/待办残留：${debugHits.join(", ")}。`);
if (depChanged && !lockChanged && state.mode !== "tweak") flags.push("依赖声明文件发生变化，但未检测到对应 lock 文件变化。");

let agentsStatus = "未检查";
const agentsPath = join(projectRoot, "AGENTS.md");
if (existsSync(agentsPath)) {
  const agents = readFileSync(agentsPath, "utf8");
  agentsStatus = agents.includes("Harness") && agents.includes("验证要求") ? "已包含 Harness 与验证规则" : "建议检查 AGENTS.md 是否完整";
}

const report = [
  "# Codex Harness 自动审查",
  "",
  `时间: ${new Date().toISOString()}`,
  `Harness: mode=${state.mode}, phase=${state.phase}`,
  "",
  "## 改动统计",
  `- 文件数: ${changed.length}`,
  `- 行数: +${insertions}/-${deletions}`,
  "",
  "## 规则检查",
  ...(flags.length > 0 ? flags.map((flag) => `- 注意: ${flag}`) : ["- 未发现自动检查问题。"]),
  "",
  "## 改动文件",
  ...(changed.length > 0 ? changed.map((file) => `- ${file}`) : ["- 无"]),
  "",
  "## 敏感文件",
  ...(sensitive.length > 0 ? sensitive.map((file) => `- ${file}`) : ["- 无"]),
  "",
  "## AGENTS.md 状态",
  `- ${agentsStatus}`,
  "",
  "## 提醒",
  "- 本报告由 hook 自动生成，只作为正式报告和人工验收的辅助材料。",
  "- 未运行构建、测试或业务验证；正式结论仍以本轮收尾报告为准。",
  "",
].join("\n");

const file = `${localTimestampForFile()}-harness-review.md`;
writeText(join(reviewDir, file), report);
process.stdout.write(`Harness 自动审查已写入 .codex/harness-reviews/${file}\n`);
