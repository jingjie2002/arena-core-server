# Codex Harness Rules

## 目标

本文件说明从 Harness-Starter 迁移到 Codex 后保留的自动化护栏。它补充 `AGENTS.md`，不替代用户原有的范围锁定、验证和报告规则。

## 能力分层

Harness 在 Codex 中分为三层：

1. 提示层：`AGENTS.md` 和 `.codex/rules/*.md` 告诉 Codex 应如何协作。
2. 工作流层：`.agents/skills/` 提供初始化、模式切换等可复用流程。
3. 自动化层：`.codex/hooks.json`、`.codex/hooks/*.mjs` 和 `.codex/rules/*.rules` 做检查、提醒、审查和部分硬约束。

## 模式与阶段

状态文件：`.codex/harness-state.json`

默认状态：

```json
{
  "phase": "build",
  "mode": "full",
  "since": ""
}
```

模式：
- `full`：完整检查。适合常规开发、重构、功能落地。
- `hotfix`：紧急修复。适合线上问题、回归修复、局部补丁；仍需验证原问题路径。
- `tweak`：微调。适合文案、样式、小范围说明调整；不得借此扩大产品方向。

阶段：
- `design`：先讨论和锁定 v1，不直接实现。
- `build`：正常开发，遵循默认规则。
- `fix`：修复问题，收紧范围，优先最小修复。

## 自动化检查

Hooks 会尽量执行以下检查：

- `PreToolUse` / `PermissionRequest`
  - 保护 `.env`、`.env.local`、密钥类文件。
  - 对危险命令给出阻断或警告，例如强制推送、硬重置、递归删除。

- `PostToolUse`
  - 对被编辑的文件尝试运行本地格式化器。
  - 只使用项目已安装的格式化工具，不联网安装依赖。

- `SessionStart`
  - 输出当前 git 分支、未提交变更、最近提交、Harness 状态和最近自动审查摘要。

- `PreCompact`
  - 在上下文压缩前输出当前分支、未提交变更、Harness 状态和最近审查位置。

- `Stop`
  - 生成 `.codex/harness-reviews/` 自动审查记录。
  - 检查改动文件数、改动行数、敏感文件、调试残留、依赖和 lock 文件关系。

## 自动审查阈值

默认阈值：

- `full/build`：超过 10 个文件或 500 行变更时提醒。
- `fix`：超过 5 个文件时提醒。
- `design`：允许较宽的文档/方案变化，但不应改业务代码。
- `hotfix`：跳过行数阈值，但保留敏感文件和调试残留检查。
- `tweak`：只保留最基础的敏感文件、危险命令和依赖变更提醒。

## 与正式报告的关系

`.codex/harness-reviews/` 是自动审查记录，只能作为辅助证据。正式收尾仍应按 `.codex/rules/reporting.md` 和 `.codex/templates/` 写入 `.codex/reports/`。

## 不可替代事项

Harness 不替代：

- 用户确认范围。
- 真实构建、测试、运行验证。
- 人对产品方向和风险的判断。
- 正式报告和验收说明。
