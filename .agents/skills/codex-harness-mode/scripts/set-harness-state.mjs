import { existsSync, mkdirSync, readFileSync, writeFileSync } from "fs";
import { dirname, join, resolve } from "path";

const VALID_MODES = new Set(["full", "hotfix", "tweak"]);
const VALID_PHASES = new Set(["design", "build", "fix"]);

function findRoot(start) {
  let current = resolve(start);
  while (true) {
    if (existsSync(join(current, "AGENTS.md")) || existsSync(join(current, ".codex"))) {
      return current;
    }
    const parent = dirname(current);
    if (parent === current) return resolve(start);
    current = parent;
  }
}

function parseArgs(argv) {
  const result = { mode: null, phase: null, status: false };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--status") result.status = true;
    else if (arg === "--mode") result.mode = argv[++i];
    else if (arg === "--phase") result.phase = argv[++i];
    else if (arg.startsWith("--mode=")) result.mode = arg.slice("--mode=".length);
    else if (arg.startsWith("--phase=")) result.phase = arg.slice("--phase=".length);
    else if (VALID_MODES.has(arg)) result.mode = arg;
    else if (VALID_PHASES.has(arg)) result.phase = arg;
  }
  return result;
}

const root = findRoot(process.cwd());
const codexDir = join(root, ".codex");
const statePath = join(codexDir, "harness-state.json");
const args = parseArgs(process.argv.slice(2));

let state = { phase: "build", mode: "full", since: "" };
if (existsSync(statePath)) {
  try {
    state = { ...state, ...JSON.parse(readFileSync(statePath, "utf8")) };
  } catch {
    process.stderr.write("无法解析 .codex/harness-state.json，将使用默认值重写。\n");
  }
}

if (args.status || (!args.mode && !args.phase)) {
  process.stdout.write(`当前 Harness 状态：mode=${state.mode}, phase=${state.phase}, since=${state.since || "未记录"}\n`);
  process.exit(0);
}

if (args.mode && !VALID_MODES.has(args.mode)) {
  process.stderr.write(`无效 mode：${args.mode}。可选：full / hotfix / tweak。\n`);
  process.exit(2);
}

if (args.phase && !VALID_PHASES.has(args.phase)) {
  process.stderr.write(`无效 phase：${args.phase}。可选：design / build / fix。\n`);
  process.exit(2);
}

if (args.mode) {
  state.mode = args.mode;
  if (!args.phase && args.mode === "hotfix") state.phase = "fix";
  if (!args.phase && args.mode === "tweak") state.phase = "design";
}

if (args.phase) state.phase = args.phase;
if (!VALID_PHASES.has(state.phase)) state.phase = "build";
if (!VALID_MODES.has(state.mode)) state.mode = "full";
state.since = new Date().toISOString();

mkdirSync(codexDir, { recursive: true });
writeFileSync(statePath, `${JSON.stringify(state, null, 2)}\n`, "utf8");
process.stdout.write(`已更新 Harness 状态：mode=${state.mode}, phase=${state.phase}, since=${state.since}\n`);
