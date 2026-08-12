import {
  block,
  getCommand,
  getFilePath,
  getToolName,
  isProtectedPath,
  payloadMentionsProtectedFile,
  readState,
  readStdinJson,
  stringifyPayload,
} from "./common.mjs";

const { raw, data } = readStdinJson();
if (!data) process.exit(0);

const state = readState();
const tool = getToolName(data);
const command = getCommand(data);
const filePath = getFilePath(data);
const serialized = stringifyPayload(data, raw);
const relaxed = state.mode === "tweak" || state.phase === "design";

if (isProtectedPath(filePath) || payloadMentionsProtectedFile(serialized)) {
  block("Harness 安全拦截：禁止 Codex 直接修改 .env、密钥或凭证类文件。请让用户手动处理，或先创建不含真实密钥的示例文件。", {
    tool,
    filePath,
  });
}

if (command) {
  const commandText = command.replace(/\s+/g, " ").trim();
  const checks = [
    {
      pattern: /\bgit\s+push\b.*\s--force(?!-with-lease)\b/i,
      reason: "Harness 安全拦截：禁止直接 git push --force。确需改写远端历史时，请先说明原因并改用 --force-with-lease。",
      always: true,
    },
    {
      pattern: /\bgit\s+push\b.*\s-f(\s|$)/i,
      reason: "Harness 安全拦截：禁止直接 git push -f。确需改写远端历史时，请先说明原因并改用 --force-with-lease。",
      always: true,
    },
    {
      pattern: /\brm\s+-rf\b/i,
      reason: "Harness 安全拦截：递归强制删除需要先说明清理目标、影响范围和可回滚方案。",
      always: false,
    },
    {
      pattern: /\bgit\s+reset\s+--hard\b/i,
      reason: "Harness 安全拦截：git reset --hard 会丢弃本地改动，必须先确认影响范围。",
      always: false,
    },
    {
      pattern: /\bRemove-Item\b.*\b-Recurse\b.*\b-Force\b/i,
      reason: "Harness 安全拦截：PowerShell 递归强制删除需要先确认目标和影响范围。",
      always: false,
    },
  ];

  for (const check of checks) {
    if (check.pattern.test(commandText) && (check.always || !relaxed)) {
      block(check.reason, { tool, command: commandText, mode: state.mode, phase: state.phase });
    }
  }
}

process.exit(0);
