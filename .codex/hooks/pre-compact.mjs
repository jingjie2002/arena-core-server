import { join } from "path";
import { changedFilesFromStatus, git, projectRoot, readState, writeText } from "./common.mjs";

const state = readState();
const changed = changedFilesFromStatus();
const branch = git(["rev-parse", "--abbrev-ref", "HEAD"]) || "非 git 目录";
const content = [
  "# PreCompact Snapshot",
  "",
  `时间: ${new Date().toISOString()}`,
  `分支: ${branch}`,
  `Harness: mode=${state.mode}, phase=${state.phase}`,
  "",
  "## 未提交变更",
  ...(changed.length > 0 ? changed.slice(0, 30).map((file) => `- ${file}`) : ["- 无"]),
  changed.length > 30 ? `- ...以及其他 ${changed.length - 30} 个文件` : "",
  "",
].filter(Boolean).join("\n");

const path = join(".codex", "context", "pre-compact-snapshot.md");
writeText(join(projectRoot, path), content);
process.stdout.write(`[Codex Harness PreCompact]\n已保存快照: ${path}\n分支: ${branch}\nHarness: mode=${state.mode}, phase=${state.phase}\n[/Codex Harness PreCompact]\n`);
