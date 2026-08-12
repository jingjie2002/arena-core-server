---
name: codex-harness-init
description: Initialize, repair, or sync a project with the Codex Harness template. Use when the user asks to set up Codex project rules, migrate Harness-Starter or Claude Code harness files to Codex, combine existing AGENTS.md/.codex rules with Harness automation, refresh hooks/skills/state files, or audit whether a project has the expected Codex Harness structure.
---

# Codex Harness Init

## Purpose

Set up Codex Harness without overwriting the user's existing project rules. Treat `AGENTS.md` and `.codex/rules/` as the source of collaboration policy, then add Harness automation through `.codex/hooks.json`, `.codex/hooks/`, `.codex/harness-state.json`, and `.agents/skills/`.

## Workflow

1. Inspect existing project context.
   - Read `AGENTS.md` if present.
   - Read `.codex/context/current-status.md`, `.codex/rules/`, and `.codex/config.toml` if present.
   - Detect technology stack from files such as `package.json`, `pyproject.toml`, `go.mod`, `Cargo.toml`, `pom.xml`, `build.gradle`, `CMakeLists.txt`, or solution files.

2. Preserve user-owned rules.
   - Do not replace an existing `AGENTS.md` wholesale.
   - Merge Harness sections under existing workflow, scope, verification, and reporting rules.
   - Keep `.codex/` as an AI management area, not a business-document storage area.

3. Ensure required Codex Harness files exist.
   - `AGENTS.md`
   - `.codex/context/current-status.md`
   - `.codex/context/current-plan.md`
   - `.codex/context/project-map.md`
   - `.codex/rules/harness.md`
   - `.codex/rules/harness-safety.rules`
   - `.codex/templates/brief-report.md`
   - `.codex/templates/audit-report.md`
   - `.codex/config.toml`
   - `.codex/hooks.json`
   - `.codex/hooks/*.mjs`
   - `.codex/harness-state.json`
   - `.agents/skills/codex-harness-init/SKILL.md`
   - `.agents/skills/codex-harness-mode/SKILL.md`

4. Migrate Claude/Harness names to Codex names.
   - `CLAUDE.md` -> `AGENTS.md`
   - `.claude/settings.json` hooks -> `.codex/hooks.json`
   - `.claude/hooks/` -> `.codex/hooks/`
   - `.claude/skills/` -> `.agents/skills/`
   - `.claude/.harness-state` -> `.codex/harness-state.json`
   - `.claude/reviews/` -> `.codex/harness-reviews/`

5. Update project context.
   - Automatically put detected run/build/test commands into `.codex/context/current-status.md` or `.codex/context/project-map.md` when useful.
   - Leave unknown commands blank rather than inventing them.
   - If the project is already mid-task, preserve the current status and append only high-value updates.
   - Do not ask the user to manually maintain context files unless the needed project fact cannot be inferred.

6. Classify current workflow state.
   - Infer task mode as `从概念开始`, `按范围执行`, or `修复问题`.
   - Infer phase as `design`, `build`, or `fix`.
   - Keep `mode=full` by default.
   - Use `hotfix` only when the user clearly indicates urgent repair, production issue, regression, or emergency patch.
   - Use `tweak` only when the user clearly asks for tiny text, style, documentation, or cosmetic changes.

7. Validate.
   - Confirm JSON files parse.
   - Confirm `SKILL.md` files have `name` and `description`.
   - Run hook scripts with empty stdin to ensure they fail closed or no-op cleanly.
   - If Node is unavailable, report that hook scripts were not executed.

8. Report completion.
   - List files added or changed.
   - List preserved user rules.
   - List any manual review items, especially hook trust, missing Node, missing formatters, or commands that could not be inferred.

## Guardrails

- Do not edit secrets, `.env`, credentials, or production config values.
- Do not install dependencies without user approval.
- Do not delete user rules while migrating.
- Do not treat automatic hook reports as formal verification.
- Do not move formal project documents into `.codex/`; keep `.codex/` for AI collaboration state and audit artifacts only.
