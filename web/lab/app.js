"use strict";

const LAB_API = "/api/lab/v1";
const WRITE_HEADERS = Object.freeze({
  "Content-Type": "application/json",
  "X-Arena-Lab": "1"
});
const ACTIVE_STATES = new Set([
  "validating", "starting", "running", "verifying", "cleaning",
  "cancelling", "timingout", "failing"
]);
const STATE_LABELS = Object.freeze({
  validating: "正在校验",
  starting: "正在启动",
  running: "运行中",
  verifying: "正在验证",
  cleaning: "正在清理",
  cancelling: "正在取消",
  timingout: "正在超时收尾",
  failing: "正在记录失败",
  passed: "通过",
  failed: "失败",
  cancelled: "已取消",
  timedout: "已超时",
  interrupted: "已中断"
});
const ENUM_LABELS = Object.freeze({
  uniform: "统一 MMR",
  stair: "阶梯 MMR",
  before_queue: "进入队列前",
  after_match: "匹配成功后",
  after_join: "加入房间后",
  queue_match: "排队匹配",
  ready_room: "房间准备",
  get_rank: "查询排名",
  get_match_result: "查询比赛结果",
  exact: "完全相同的结果",
  conflicting: "相互冲突的结果",
  disconnect: "断开连接",
  latency: "增加延迟",
  timeout: "制造超时",
  reset_peer: "重置连接"
});
const FIELD_HELP = Object.freeze({
  player_count: "本次创建的真实客户端数量；1v1 要求使用偶数。",
  mmr_mode: "统一值用于稳定配对；阶梯值用于覆盖匹配分差。",
  mmr_base: "所有玩家 MMR 的起点，范围由服务端约束。",
  mmr_stride: "阶梯模式下，相邻玩家 MMR 的间隔。",
  connect_interval_ms: "相邻客户端开始连接的时间间隔。",
  ready_delay_ms: "客户端进入房间后，等待多久再发送 Ready。",
  player_index: "选择本场 1v1 中哪一名玩家执行断线。",
  disconnect_stage: "选择在排队、匹配或进房链路的哪个阶段断开。",
  reconnect_delay_ms: "断开后等待多久重新连接，用于覆盖恢复窗口内外。",
  message: "选择要用相同 request id 重放的玩家消息。",
  repeat_count: "同一请求或结果总共发送多少次。",
  mode: "验证完全相同的幂等重放，或验证冲突结果被拒绝。",
  fault_kind: "只允许 Lab 预先定义的 Redis 网络故障。",
  fault_value_ms: "延迟或超时类故障的受控时间参数。",
  fault_duration_ms: "故障保持多久；Run 收尾时仍会主动恢复代理。"
});
const FORBIDDEN_PARAMETER = /(^|_)(host|hostname|port|token|secret|password|credential|path|file|directory|env|environment|sql|shell|command|lua|protobuf|endpoint|url)($|_)/i;
const SENSITIVE_KEY = /(token|secret|password|authorization|credential)/i;
const RUN_ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$/;

let pollTimer = 0;
let pollGeneration = 0;

class ApiError extends Error {
  constructor(message, status, payload) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.payload = payload;
  }
}

function byId(id) {
  return document.getElementById(id);
}

function normalizeState(value) {
  return String(value || "").replace(/[\s_-]/g, "").toLowerCase();
}

function stateLabel(value) {
  const normalized = normalizeState(value);
  return STATE_LABELS[normalized] || String(value || "未知");
}

function isActiveState(value) {
  return ACTIVE_STATES.has(normalizeState(value));
}

function isTerminalState(value) {
  return ["passed", "failed", "cancelled", "timedout", "interrupted"].includes(normalizeState(value));
}

function safeStorageGet(key) {
  try {
    return window.localStorage.getItem(key) || "";
  } catch (_error) {
    return "";
  }
}

function safeStorageSet(key, value) {
  try {
    window.localStorage.setItem(key, value);
  } catch (_error) {
    // 本机偏好保存失败不影响真实 Run。
  }
}

function safeObject(value, depth = 0) {
  if (depth > 5) return "[内容过深]";
  if (Array.isArray(value)) return value.map((item) => safeObject(item, depth + 1));
  if (!value || typeof value !== "object") return value;
  const copy = {};
  Object.entries(value).forEach(([key, item]) => {
    copy[key] = SENSITIVE_KEY.test(key) ? "[已隐藏]" : safeObject(item, depth + 1);
  });
  return copy;
}

function displayValue(value) {
  if (value === undefined || value === null || value === "") return "—";
  if (typeof value === "boolean") return value ? "是" : "否";
  if (typeof value === "object") return JSON.stringify(safeObject(value), null, 2);
  return String(value);
}

function formatTime(value) {
  if (!value) return "—";
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) return String(value);
  return new Intl.DateTimeFormat("zh-CN", {
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
    hour12: false
  }).format(parsed);
}

function formatDuration(milliseconds) {
  const value = Number(milliseconds);
  if (!Number.isFinite(value) || value < 0) return "—";
  if (value < 1000) return `${Math.round(value)} ms`;
  if (value < 60_000) return `${(value / 1000).toFixed(value < 10_000 ? 2 : 1)} 秒`;
  const minutes = Math.floor(value / 60_000);
  const seconds = Math.round((value % 60_000) / 1000);
  return `${minutes} 分 ${seconds} 秒`;
}

function apiErrorMessage(error) {
  if (error instanceof ApiError) {
    const payload = error.payload || {};
    const serverMessage = payload.message || payload.detail || payload.error_description;
    const code = payload.code || payload.error;
    if (serverMessage) return String(serverMessage);
    if (code) return `服务端拒绝了请求：${code}`;
    if (error.status === 404) return "没有找到对应记录，请检查地址或 Run ID。";
    if (error.status === 409) return "当前已有活动 Run，或请求与现有状态冲突。";
    if (error.status === 422 || error.status === 400) return "参数没有通过服务端校验，请检查后重试。";
    if (error.status === 503) return "Lab 或其依赖暂时不可用，请稍后重试。";
  }
  if (error && error.name === "AbortError") return "等待本机 Lab 响应超时。";
  if (error instanceof Error && error.name !== "TypeError" && error.message) return error.message;
  return "无法连接本机 Lab。请确认使用 Lab profile 启动，而不是正式服务 profile。";
}

async function api(path, options = {}) {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 10_000);
  try {
    const response = await fetch(`${LAB_API}${path}`, {
      cache: "no-store",
      credentials: "same-origin",
      ...options,
      signal: controller.signal
    });
    const raw = await response.text();
    let payload = null;
    if (raw) {
      try {
        payload = JSON.parse(raw);
      } catch (_error) {
        payload = { message: raw.slice(0, 500) };
      }
    }
    if (!response.ok) {
      throw new ApiError(response.statusText || "Lab API error", response.status, payload);
    }
    return payload || {};
  } finally {
    window.clearTimeout(timeout);
  }
}

function setStatus(element, kind, message) {
  if (!element) return;
  element.replaceChildren();
  const dot = document.createElement("span");
  dot.className = `status-dot ${kind || ""}`.trim();
  const copy = document.createElement("span");
  copy.textContent = message;
  element.append(dot, copy);
}

function showNotice(element, message, kind = "") {
  if (!element) return;
  element.hidden = false;
  element.className = `notice ${kind}`.trim();
  element.textContent = message;
}

function hideNotice(element) {
  if (!element) return;
  element.hidden = true;
  element.textContent = "";
}

function appendDataRow(list, label, value) {
  const item = document.createElement("li");
  const key = document.createElement("strong");
  const copy = document.createElement("span");
  key.textContent = label;
  copy.textContent = displayValue(value);
  item.append(key, copy);
  list.append(item);
}

function setFact(fact, label, value, note) {
  fact.replaceChildren();
  const labelNode = document.createElement("span");
  const valueNode = document.createElement("span");
  const noteNode = document.createElement("span");
  labelNode.className = "fact-label";
  valueNode.className = "fact-value";
  noteNode.className = "fact-note";
  labelNode.textContent = label;
  valueNode.textContent = displayValue(value);
  noteNode.textContent = note;
  fact.append(labelNode, valueNode, noteNode);
}

async function initOverview() {
  const status = byId("capability-status");
  const facts = Array.from(byId("service-facts").children);
  const message = byId("overview-message");
  try {
    const capabilities = await api("/capabilities");
    const service = capabilities.service || {};
    const scenarios = Array.isArray(capabilities.scenarios) ? capabilities.scenarios : [];
    const availableCount = scenarios.filter((item) => item && item.available !== false).length;
    setFact(facts[0], "服务", service.name || "—", "来自当前 Lab 实例");
    setFact(facts[1], "版本", service.version || capabilities.schema_version || "—", `能力 Schema ${capabilities.schema_version || "未报告"}`);
    const environment = capabilities.environment;
    const commit = environment && typeof environment === "object" ? environment.git_commit : "";
    setFact(facts[2], "运行环境", typeof environment === "string" ? environment : (environment ? "当前构建" : "—"), commit ? `提交 ${String(commit).slice(0, 12)}` : "由控制器配置决定");
    setFact(facts[3], "可运行场景", availableCount, scenarios.length ? `能力清单共 ${scenarios.length} 项` : "服务未公布场景");
    byId("service-facts").setAttribute("aria-busy", "false");
    setStatus(status, "ready", "已连接本机 Lab，信息来自真实接口");
    if (!scenarios.length) {
      showNotice(message, "Lab 已响应，但当前没有公布任何可运行场景。", "warning");
    } else {
      hideNotice(message);
    }
  } catch (error) {
    byId("service-facts").setAttribute("aria-busy", "false");
    setStatus(status, "error", "本机 Lab 未连接");
    showNotice(message, apiErrorMessage(error), "error");
  }
}

async function initIntegration() {
  const status = byId("integration-service-status");
  const label = byId("integration-service-label");
  try {
    const capabilities = await api("/capabilities");
    const service = capabilities.service || {};
    const text = [service.name, service.version].filter(Boolean).join(" · ");
    setStatus(status, "ready", text ? `当前实例：${text}` : "已连接当前 Lab 实例");
    label.textContent = text || "当前 Lab 已连接";
  } catch (error) {
    setStatus(status, "error", "当前 Lab 未连接；文档仍可阅读");
    label.textContent = "接入说明不包含任何运行凭证";
  }
}

function scenarioButton(scenario, selectedName, choose) {
  const button = document.createElement("button");
  const name = String(scenario.name || "");
  button.type = "button";
  button.className = `scenario-card${name === selectedName ? " selected" : ""}`;
  button.disabled = scenario.available === false || !name;
  button.dataset.scenario = name;

  const code = document.createElement("small");
  const title = document.createElement("h3");
  const description = document.createElement("p");
  const availability = document.createElement("span");
  code.textContent = name || "未命名场景";
  title.textContent = scenario.title || name || "未命名场景";
  description.textContent = scenario.description || "服务端没有提供此场景的说明。";
  availability.className = "availability";
  availability.textContent = button.disabled ? "当前不可用" : "可以运行";
  button.append(code, title, description, availability);
  if (!button.disabled) button.addEventListener("click", () => choose(name, true));
  return button;
}

function enumChoices(field) {
  const source = Array.isArray(field.enum) ? field.enum : [];
  return source.map((item) => {
    if (item && typeof item === "object") {
      return { value: item.value, label: item.label || ENUM_LABELS[String(item.value)] || displayValue(item.value) };
    }
    return { value: item, label: ENUM_LABELS[String(item)] || displayValue(item) };
  });
}

function parameterControl(field) {
  const wrapper = document.createElement("div");
  const label = document.createElement("label");
  const help = document.createElement("span");
  const name = String(field.name || "");
  const inputId = `parameter-${name.replace(/[^A-Za-z0-9_-]/g, "-")}`;
  const type = String(field.type || "string").toLowerCase();
  const choices = enumChoices(field);
  let input;

  wrapper.className = "field";
  wrapper.dataset.parameterField = name;
  label.htmlFor = inputId;
  label.textContent = field.label || name;
  help.className = "field-help";
  help.textContent = field.help || FIELD_HELP[name] || "范围和组合约束由服务端校验。";

  if (type === "boolean") {
    const checkboxWrap = document.createElement("div");
    input = document.createElement("input");
    input.type = "checkbox";
    input.checked = Boolean(field.default);
    checkboxWrap.className = "checkbox-field";
    checkboxWrap.append(input, document.createTextNode("启用"));
    wrapper.append(label, checkboxWrap, help);
  } else if (choices.length || type === "enum") {
    input = document.createElement("select");
    choices.forEach((choice, index) => {
      const option = document.createElement("option");
      option.value = String(index);
      option.textContent = choice.label;
      if (Object.is(choice.value, field.default) || String(choice.value) === String(field.default)) {
        option.selected = true;
      }
      input.append(option);
    });
    input._labChoices = choices;
    wrapper.append(label, input, help);
  } else {
    input = document.createElement("input");
    if (type === "integer" || type === "number") {
      input.type = "number";
      if (field.min !== undefined) input.min = String(field.min);
      if (field.max !== undefined) input.max = String(field.max);
      input.step = String(field.step !== undefined ? field.step : (type === "integer" ? 1 : "any"));
    } else {
      input.type = "text";
      if (field.min !== undefined) input.minLength = Number(field.min);
      if (field.max !== undefined) input.maxLength = Number(field.max);
    }
    input.value = field.default === undefined || field.default === null ? "" : String(field.default);
    wrapper.append(label, input, help);
  }

  input.id = inputId;
  input.name = name;
  input.dataset.valueType = type;
  input.dataset.defaultValue = JSON.stringify(field.default === undefined ? null : field.default);
  if (field.required === true) input.required = true;
  wrapper._labField = field;
  wrapper._labInput = input;
  return wrapper;
}

function readParameter(wrapper) {
  const field = wrapper._labField;
  const input = wrapper._labInput;
  const type = String(field.type || "string").toLowerCase();
  input.setAttribute("aria-invalid", "false");

  if (!input.checkValidity()) {
    input.setAttribute("aria-invalid", "true");
    throw new Error(`${field.label || field.name} 不在允许范围内。`);
  }
  if (type === "boolean") return input.checked;
  if (input._labChoices) {
    const selected = input._labChoices[Number(input.value)];
    if (!selected) throw new Error(`${field.label || field.name} 没有有效选项。`);
    return selected.value;
  }
  if (type === "integer") {
    const value = Number(input.value);
    if (!Number.isSafeInteger(value)) throw new Error(`${field.label || field.name} 必须是整数。`);
    return value;
  }
  if (type === "number") {
    const value = Number(input.value);
    if (!Number.isFinite(value)) throw new Error(`${field.label || field.name} 必须是有效数字。`);
    return value;
  }
  return input.value;
}

async function initScenarios() {
  const status = byId("scenario-service-status");
  const list = byId("scenario-list");
  const pageMessage = byId("scenario-message");
  const formMessage = byId("form-message");
  const form = byId("scenario-form");
  const startButton = byId("start-run");
  const resetButton = byId("reset-parameters");
  const fieldsRoot = byId("parameter-fields");
  const expectedRoot = byId("expected-list");
  let capabilities = null;
  let selectedName = "";
  let selectedSchema = null;

  function renderScenarioList() {
    list.replaceChildren();
    const scenarios = capabilities && Array.isArray(capabilities.scenarios) ? capabilities.scenarios : [];
    if (!scenarios.length) {
      const empty = document.createElement("div");
      empty.className = "panel empty-state";
      empty.style.gridColumn = "1/-1";
      const content = document.createElement("div");
      const title = document.createElement("h3");
      const copy = document.createElement("p");
      title.textContent = "当前服务没有公布场景";
      copy.textContent = "能力清单为空，因此页面不会提供固定的演示按钮。";
      content.append(title, copy);
      empty.append(content);
      list.append(empty);
    } else {
      scenarios.forEach((scenario) => list.append(scenarioButton(scenario, selectedName, chooseScenario)));
    }
    list.setAttribute("aria-busy", "false");
  }

  function renderExpected(items) {
    expectedRoot.replaceChildren();
    const expected = Array.isArray(items) ? items : [];
    if (!expected.length) {
      const item = document.createElement("li");
      item.textContent = "此 Schema 没有提供预期断言说明；运行后仍以真实断言记录为准。";
      expectedRoot.append(item);
      return;
    }
    expected.forEach((text) => {
      const item = document.createElement("li");
      item.textContent = displayValue(text);
      expectedRoot.append(item);
    });
  }

  function renderFields(schema) {
    const fields = Array.isArray(schema.fields) ? schema.fields : [];
    fieldsRoot.replaceChildren();
    const invalid = fields.find((field) => !field || !field.name || FORBIDDEN_PARAMETER.test(String(field.name)));
    if (invalid) {
      const message = invalid && invalid.name
        ? `Schema 包含页面禁止暴露的字段“${invalid.name}”，已阻止运行。`
        : "Schema 包含无效字段，已阻止运行。";
      showNotice(formMessage, message, "error");
      startButton.disabled = true;
      resetButton.disabled = true;
      return;
    }
    fields.forEach((field) => fieldsRoot.append(parameterControl(field)));
    if (!fields.length) {
      const notice = document.createElement("div");
      notice.className = "notice";
      notice.textContent = "此场景没有可调参数，将使用服务端定义的空参数对象。";
      fieldsRoot.append(notice);
    }
    hideNotice(formMessage);
    startButton.disabled = false;
    resetButton.disabled = false;
  }

  async function chooseScenario(name, userInitiated = false) {
    if (!name || selectedName === name && selectedSchema) return;
    selectedName = name;
    selectedSchema = null;
    startButton.disabled = true;
    resetButton.disabled = true;
    renderScenarioList();
    fieldsRoot.replaceChildren();
    const loading = document.createElement("div");
    loading.className = "notice";
    loading.textContent = "正在读取这个场景的版本化参数 Schema…";
    fieldsRoot.append(loading);
    hideNotice(formMessage);
    try {
      const schema = await api(`/scenarios/${encodeURIComponent(name)}/schema`);
      if (selectedName !== name) return;
      selectedSchema = schema;
      byId("selected-scenario-title").textContent = schema.title || name;
      byId("selected-scenario-description").textContent = schema.description || "服务端没有提供场景说明。";
      renderFields(schema);
      renderExpected(schema.expected);
      safeStorageSet("arenaLab.lastScenario", name);
      if (userInitiated) {
        byId("scenario-workspace").scrollIntoView({ behavior: "smooth", block: "start" });
      }
    } catch (error) {
      if (selectedName !== name) return;
      fieldsRoot.replaceChildren();
      showNotice(formMessage, apiErrorMessage(error), "error");
      byId("selected-scenario-title").textContent = name;
      byId("selected-scenario-description").textContent = "无法读取参数定义，因此不能创建 Run。";
      renderExpected([]);
    }
  }

  resetButton.addEventListener("click", () => {
    if (!selectedSchema) return;
    renderFields(selectedSchema);
  });

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!selectedSchema || !selectedName) return;
    hideNotice(formMessage);
    const parameters = {};
    try {
      Array.from(fieldsRoot.querySelectorAll(".field[data-parameter-field]")).forEach((wrapper) => {
        parameters[wrapper.dataset.parameterField] = readParameter(wrapper);
      });
    } catch (error) {
      showNotice(formMessage, error.message, "error");
      return;
    }

    startButton.disabled = true;
    resetButton.disabled = true;
    startButton.textContent = "正在创建真实 Run…";
    try {
      const created = await api("/runs", {
        method: "POST",
        headers: WRITE_HEADERS,
        body: JSON.stringify({
          schema_version: selectedSchema.schema_version || capabilities.schema_version,
          scenario: selectedName,
          parameters
        })
      });
      const runId = created.run_id || (created.run && created.run.run_id);
      if (!runId) throw new Error("服务端创建了响应，但没有返回 run_id。" );
      safeStorageSet("arenaLab.lastRunId", String(runId));
      window.location.assign(`runs.html?run_id=${encodeURIComponent(runId)}`);
    } catch (error) {
      showNotice(formMessage, apiErrorMessage(error), "error");
      startButton.disabled = false;
      resetButton.disabled = false;
      startButton.textContent = "创建并运行";
    }
  });

  try {
    capabilities = await api("/capabilities");
    setStatus(status, "ready", "已读取当前 Lab 的真实场景清单");
    hideNotice(pageMessage);
    renderScenarioList();
    const scenarios = Array.isArray(capabilities.scenarios) ? capabilities.scenarios : [];
    const remembered = safeStorageGet("arenaLab.lastScenario");
    const initial = scenarios.find((item) => item && item.available !== false && item.name === remembered)
      || scenarios.find((item) => item && item.available !== false);
    if (initial && initial.name) await chooseScenario(String(initial.name));
  } catch (error) {
    setStatus(status, "error", "无法连接本机 Lab");
    showNotice(pageMessage, apiErrorMessage(error), "error");
    capabilities = { scenarios: [] };
    renderScenarioList();
  }
}

function stateClass(status) {
  const normalized = normalizeState(status);
  if (ACTIVE_STATES.has(normalized)) return "active";
  if (normalized === "interrupted") return "failed";
  if (["passed", "failed", "cancelled", "timedout"].includes(normalized)) return normalized;
  return "";
}

function renderObjectList(list, value, emptyMessage) {
  list.replaceChildren();
  const entries = value && typeof value === "object" ? Object.entries(safeObject(value)) : [];
  if (!entries.length) {
    appendDataRow(list, "状态", emptyMessage);
    return;
  }
  entries.forEach(([key, item]) => appendDataRow(list, key, item));
}

function eventSequence(event, fallback) {
  const value = Number(event && (event.seq ?? event.sequence));
  return Number.isSafeInteger(value) && value >= 0 ? value : fallback;
}

function renderTimeline(eventMap) {
  const root = byId("run-timeline");
  const empty = byId("timeline-empty");
  root.replaceChildren();
  const events = Array.from(eventMap.values()).sort((left, right) => left._sequence - right._sequence);
  empty.hidden = events.length > 0;
  events.forEach((event) => {
    const item = document.createElement("li");
    const dot = document.createElement("span");
    const content = document.createElement("div");
    const title = document.createElement("h3");
    const description = document.createElement("p");
    const meta = document.createElement("div");
    item.className = "timeline-item";
    dot.className = "timeline-dot";
    title.textContent = event.message || event.type || event.phase || "运行事件";
    const details = event.detail === undefined || event.detail === null ? "" : displayValue(event.detail);
    description.textContent = details;
    description.hidden = !details;
    meta.className = "timeline-meta";
    meta.textContent = `#${event._sequence} · ${formatTime(event.timestamp || event.at)}`;
    content.className = "timeline-content";
    content.append(title, description);
    item.append(dot, content, meta);
    root.append(item);
  });
}

function renderAssertions(assertions) {
  const root = byId("run-assertions");
  const empty = byId("assertion-empty");
  root.replaceChildren();
  const items = Array.isArray(assertions) ? assertions : [];
  empty.hidden = items.length > 0;
  items.forEach((assertion) => {
    const item = document.createElement("li");
    const mark = document.createElement("span");
    const content = document.createElement("div");
    const title = document.createElement("h3");
    const details = document.createElement("p");
    const expected = document.createElement("span");
    const actual = document.createElement("span");
    const passed = assertion.passed;
    item.className = `assertion ${passed === false ? "failed" : passed === true ? "" : "pending"}`.trim();
    mark.className = "assertion-mark";
    mark.textContent = passed === true ? "✓" : passed === false ? "×" : "·";
    title.textContent = assertion.title || assertion.id || "自动断言";
    expected.textContent = `预期：${displayValue(assertion.expected)}`;
    actual.textContent = `实际：${displayValue(assertion.actual)}`;
    details.append(expected, actual);
    content.append(title, details);
    item.append(mark, content);
    root.append(item);
  });
}

function renderCleanup(cleanup) {
  const root = byId("run-cleanup");
  root.replaceChildren();
  if (!cleanup || typeof cleanup !== "object") {
    const notice = document.createElement("div");
    notice.className = "notice";
    notice.textContent = "服务端尚未返回清理记录。";
    root.append(notice);
    return;
  }
  const notice = document.createElement("div");
  const completed = cleanup.completed === true;
  notice.className = `notice ${completed ? "success" : "warning"}`;
  notice.textContent = completed
    ? `清理已完成${cleanup.detail ? `：${displayValue(cleanup.detail)}` : "。"}`
    : `清理尚未完成${cleanup.detail ? `：${displayValue(cleanup.detail)}` : "。"}`;
  root.append(notice);
}

function firstFailureText(value) {
  if (!value) return "无记录";
  if (typeof value === "string") return value;
  return displayValue(value);
}

async function initRuns() {
  const form = byId("run-lookup-form");
  const input = byId("run-id-input");
  const message = byId("run-message");
  const empty = byId("run-empty");
  const content = byId("run-content");
  const cancelButton = byId("cancel-run");
  const replayButton = byId("replay-run");
  const eventMap = new Map();
  let currentRun = null;
  let currentRunId = "";
  let afterSequence = 0;

  function validateRunId(value) {
    const trimmed = String(value || "").trim();
    if (!RUN_ID_PATTERN.test(trimmed)) {
      throw new Error("run_id 只能包含字母、数字、点、冒号、下划线和连字符，最长 128 个字符。" );
    }
    return trimmed;
  }

  function mergeEvents(events) {
    const items = Array.isArray(events) ? events : [];
    items.forEach((event, index) => {
      if (!event || typeof event !== "object") return;
      const sequence = eventSequence(event, afterSequence + index + 1);
      eventMap.set(sequence, { ...event, _sequence: sequence });
      afterSequence = Math.max(afterSequence, sequence);
    });
    renderTimeline(eventMap);
  }

  function renderRun(run) {
    currentRun = run;
    const status = run.status || "";
    const pill = byId("run-status-pill");
    byId("run-scenario-label").textContent = run.scenario || "未报告场景";
    byId("run-id-label").textContent = run.run_id || currentRunId;
    pill.className = `state-pill ${stateClass(status)}`.trim();
    pill.textContent = stateLabel(status);

    const facts = byId("run-core-facts");
    facts.replaceChildren();
    appendDataRow(facts, "开始时间", formatTime(run.started_at));
    appendDataRow(facts, "结束时间", formatTime(run.finished_at));
    appendDataRow(facts, "耗时", formatDuration(run.elapsed_ms));
    appendDataRow(facts, "最终 Seed", run.seed);
    appendDataRow(facts, "重放来源", run.replay_of || "首次运行");
    appendDataRow(facts, "首个失败", firstFailureText(run.first_failure));
    renderObjectList(byId("run-parameters"), run.parameters, "没有参数记录");
    renderAssertions(run.assertions);
    renderObjectList(byId("run-evidence"), run.evidence, "暂无业务证据");
    renderCleanup(run.cleanup);
    mergeEvents(run.events);
    cancelButton.disabled = !isActiveState(status);
    replayButton.disabled = !isTerminalState(status);
  }

  async function fetchNewEvents() {
    if (!currentRunId) return;
    const payload = await api(`/runs/${encodeURIComponent(currentRunId)}/events?after_seq=${encodeURIComponent(afterSequence)}`);
    mergeEvents(payload.items || payload.events);
    const next = Number(payload.next_seq);
    if (Number.isSafeInteger(next) && next >= afterSequence) afterSequence = next;
  }

  function stopPolling(label) {
    pollGeneration += 1;
    window.clearTimeout(pollTimer);
    pollTimer = 0;
    setStatus(byId("poll-status"), "", label || "轮询已停止");
  }

  function beginPolling() {
    const generation = ++pollGeneration;
    window.clearTimeout(pollTimer);
    const tick = async () => {
      if (generation !== pollGeneration || !currentRunId) return;
      try {
        const [run] = await Promise.all([
          api(`/runs/${encodeURIComponent(currentRunId)}`),
          fetchNewEvents()
        ]);
        if (generation !== pollGeneration) return;
        renderRun(run);
        hideNotice(message);
        if (isTerminalState(run.status)) {
          try { await fetchNewEvents(); } catch (_error) { /* 保留已获取事件。 */ }
          stopPolling("Run 已结束，事件记录已停止刷新");
          return;
        }
        setStatus(byId("poll-status"), "loading", "活动 Run：正在读取新增事件");
      } catch (error) {
        if (generation !== pollGeneration) return;
        showNotice(message, `刷新暂时失败：${apiErrorMessage(error)}`, "warning");
        setStatus(byId("poll-status"), "error", "刷新暂时中断，将自动重试");
      }
      pollTimer = window.setTimeout(tick, document.hidden ? 4_000 : 1_500);
    };
    setStatus(byId("poll-status"), "loading", "活动 Run：开始短轮询");
    pollTimer = window.setTimeout(tick, 1_000);
  }

  async function loadRun(id) {
    let validId;
    try {
      validId = validateRunId(id);
    } catch (error) {
      showNotice(message, error.message, "error");
      return;
    }
    stopPolling("正在读取 Run");
    currentRunId = validId;
    currentRun = null;
    eventMap.clear();
    afterSequence = 0;
    input.value = validId;
    safeStorageSet("arenaLab.lastRunId", validId);
    showNotice(message, "正在读取真实运行记录…");
    try {
      const run = await api(`/runs/${encodeURIComponent(validId)}`);
      if (currentRunId !== validId) return;
      empty.hidden = true;
      content.hidden = false;
      renderRun(run);
      try { await fetchNewEvents(); } catch (_error) { /* Run 主记录仍可显示。 */ }
      hideNotice(message);
      const url = new URL(window.location.href);
      url.searchParams.set("run_id", validId);
      window.history.replaceState(null, "", url);
      if (isActiveState(run.status)) beginPolling();
      else stopPolling("Run 已结束，当前显示保存的最终记录");
    } catch (error) {
      if (currentRunId !== validId) return;
      content.hidden = true;
      empty.hidden = false;
      showNotice(message, apiErrorMessage(error), "error");
    }
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    loadRun(input.value);
  });

  cancelButton.addEventListener("click", async () => {
    if (!currentRunId || !currentRun || !isActiveState(currentRun.status)) return;
    cancelButton.disabled = true;
    showNotice(message, "已发送取消请求；Run 仍会先执行清理。", "warning");
    try {
      const updated = await api(`/runs/${encodeURIComponent(currentRunId)}/cancel`, {
        method: "POST", headers: WRITE_HEADERS, body: "{}"
      });
      if (updated && updated.run_id) renderRun(updated);
      beginPolling();
    } catch (error) {
      showNotice(message, apiErrorMessage(error), "error");
      cancelButton.disabled = !isActiveState(currentRun.status);
    }
  });

  replayButton.addEventListener("click", async () => {
    if (!currentRunId || !currentRun || !isTerminalState(currentRun.status)) return;
    replayButton.disabled = true;
    showNotice(message, "正在使用原参数和原 Seed 创建新的 Run…");
    try {
      const created = await api(`/runs/${encodeURIComponent(currentRunId)}/replay`, {
        method: "POST", headers: WRITE_HEADERS, body: "{}"
      });
      const newId = created.run_id || (created.run && created.run.run_id);
      if (!newId) throw new Error("服务端没有返回新的 run_id。" );
      await loadRun(String(newId));
    } catch (error) {
      showNotice(message, apiErrorMessage(error), "error");
      replayButton.disabled = false;
    }
  });

  window.addEventListener("beforeunload", () => stopPolling("页面关闭"));

  const params = new URLSearchParams(window.location.search);
  const initialId = params.get("run_id") || safeStorageGet("arenaLab.lastRunId");
  if (initialId) loadRun(initialId);
}

document.addEventListener("DOMContentLoaded", () => {
  const page = document.body.dataset.page;
  if (page === "overview") initOverview();
  if (page === "scenarios") initScenarios();
  if (page === "runs") initRuns();
  if (page === "integration") initIntegration();
});
