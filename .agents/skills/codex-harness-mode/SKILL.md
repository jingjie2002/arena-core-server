---
name: codex-harness-mode
description: Switch or inspect the current Codex Harness mode and development phase. Use when the user asks for full, hotfix, or tweak mode; design, build, or fix phase; wants to loosen or tighten Harness checks; asks for Harness status; or wants Codex to adjust workflow strictness for emergency fixes, small tweaks, or planning.
---

# Codex Harness Mode

## Purpose

Manage `.codex/harness-state.json`, which controls how strictly Codex should apply Harness review thresholds and workflow expectations.

## Supported State

Modes:
- `full`: normal strict mode; all rules and checks apply.
- `hotfix`: emergency fix mode; preserve safety checks, relax broad line-count warnings.
- `tweak`: small adjustment mode; keep sensitive-file and dangerous-command protection, relax most scope-size warnings.

Phases:
- `design`: planning and decision phase.
- `build`: implementation phase.
- `fix`: bug-fix phase.

Default:

```json
{
  "phase": "build",
  "mode": "full",
  "since": ""
}
```

## Workflow

1. Read `.codex/harness-state.json`; create it with default values if missing.
2. If the user did not explicitly name a mode or phase, infer it from the task:
   - concept or planning work -> `phase=design`
   - implementation work -> `phase=build`
   - bug, failure, regression, or exception work -> `phase=fix`
   - urgent repair or production incident -> `mode=hotfix`
   - tiny text/style/documentation tweak -> `mode=tweak`
   - otherwise keep the current mode, defaulting to `full`
3. Apply the requested or inferred change.
4. Update `since` to the current ISO timestamp.
5. Preserve unrelated JSON fields if present.
6. Summarize the new mode and its practical effect in Chinese.

Use the bundled script when available:

```bash
node .agents/skills/codex-harness-mode/scripts/set-harness-state.mjs --status
node .agents/skills/codex-harness-mode/scripts/set-harness-state.mjs --mode hotfix
node .agents/skills/codex-harness-mode/scripts/set-harness-state.mjs --phase design
node .agents/skills/codex-harness-mode/scripts/set-harness-state.mjs --mode full --phase build
```

## Mode Linkage

- Setting `--mode hotfix` also sets `phase` to `fix` unless the user explicitly supplies another phase.
- Setting `--mode tweak` also sets `phase` to `design` unless the user explicitly supplies another phase.
- Setting `--mode full` keeps the current phase unless the current phase is missing.

## Response Requirements

After changing state, say:

- 当前 mode 和 phase。
- 为什么切换。
- 哪些检查会变松或变严。
- 是否需要用户在本轮结束后切回 `full/build`。

Do not use mode changes to bypass user-confirmation rules for destructive, broad, or direction-changing work.
