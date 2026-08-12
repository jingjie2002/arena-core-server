const byId = (id) => document.getElementById(id);
const endpoints = ["overview", "sessions", "tickets", "rooms", "rankings", "qa/runs"];
let refreshTimer;
let scenarioRunning = false;
let selectedScenario = "";
let scenarioError = "";

const escapeHtml = (value) => String(value ?? "")
  .replaceAll("&", "&amp;")
  .replaceAll("<", "&lt;")
  .replaceAll(">", "&gt;")
  .replaceAll('"', "&quot;")
  .replaceAll("'", "&#039;");

const stateLabels = {
  Active: "在线",
  Closed: "比赛已结束",
  Committed: "成绩已保存",
  Matched: "已完成匹配",
  Queued: "等待匹配",
  Ready: "已准备",
  Settling: "正在保存成绩"
};

const healthNames = {
  observer: "当前演示页面",
  tcp: "玩家连接入口",
  mysql: "比赛成绩数据库",
  redis: "排行榜缓存"
};

const healthDetails = {
  accepting: "正在正常接收玩家连接",
  "transaction source ready": "可以正常保存比赛成绩",
  "rank cache fresh": "排行榜数据已经是最新的",
  "repaired from MySQL; cache fresh": "已根据真实成绩自动恢复，排行榜是最新的",
  "simulated timeout; MySQL remains authoritative": "正在模拟排行榜故障，真实比赛成绩仍然安全",
  "demo mode": "本地安全演示已开启",
  "read only": "当前页面只读取状态，不修改业务数据",
  "not configured": "当前演示未连接外部数据库",
  checking: "正在检查运行状态",
  draining: "正在安全停止",
  stopped: "已经停止"
};

const qaNames = {
  "8 人匹配 / 4 房间闭环": "8 人完成 4 场比赛",
  "重复结算幂等复测": "重复提交不会重复加分",
  "Redis 故障与 dirty 修复": "排行榜故障与自动修复"
};

const qaDetails = {
  "8 sessions → 8 tickets → 4 rooms → 4 settlements": "8 名玩家全部完成匹配，形成 4 场比赛，并保存了 4 份比赛成绩。",
  "same fingerprint returned the committed receipt; score changed once": "同一份比赛结果再次提交时，服务器返回原结果，玩家分数只计算了一次。",
  "dirty retained while unavailable and cleared after replay": "排行榜暂时不可用时，待修记录被保留；恢复后自动补齐并清除。"
};

const eventTypes = {
  "observer.started": "演示页面已启动",
  "session.connected": "玩家已连接",
  "session.active": "玩家连接已确认",
  "session.rejected": "玩家连接被拒绝",
  "session.disconnected": "玩家已断开",
  "session.closed": "玩家连接已关闭",
  "ticket.queued": "玩家进入匹配队列",
  "ticket.matched": "玩家匹配成功",
  "ticket.cancelled": "匹配已取消",
  "ticket.expired": "匹配等待超时",
  "match.created": "比赛房间已创建",
  "room.ready": "双方玩家已准备",
  "room.state_changed": "比赛状态已变化",
  "room.closed": "比赛房间已结束",
  "settlement.committed": "比赛成绩已保存",
  "settlement.idempotent": "重复提交已识别",
  "settlement.failed": "比赛成绩保存失败",
  "rank.cache_stale": "排行榜暂时不是最新",
  "rank.repair_started": "开始自动修复排行榜",
  "rank.repaired": "排行榜已经恢复",
  "scenario.passed": "自动检查通过"
};

const eventMessages = {
  "both players ready; deterministic result frozen": "双方玩家都已准备，服务器确定了本场比赛结果。",
  "MySQL committed before Redis rank update": "比赛成绩已先保存，再更新排行榜。",
  "room closed after settlement commit": "成绩保存成功后，本场比赛安全结束。",
  "8-player / 4-room demonstration passed": "8 人、4 场比赛的完整流程已通过。",
  "same fingerprint replay returned original receipt": "发现相同比赛结果再次提交，服务器返回第一次的结果。",
  "duplicate settlement did not double-apply score": "重复提交没有导致玩家再次加分。",
  "Redis update failed; dirty row retained": "模拟排行榜更新失败，服务器保留了待修记录。",
  "repair worker replayed MySQL target scores": "自动修复程序正在根据真实成绩重建排行榜。",
  "dirty cleared after cache reached MySQL state": "排行榜追上真实成绩后，待修记录已经清除。",
  "Redis failure recovery passed": "排行榜故障恢复检查通过。",
  "observer API listening": "演示页面已经可以读取服务器状态。"
};

function emptyRow(columns, text = "运行第 1 项后显示数据") {
  return `<tr><td class="empty" colspan="${columns}">${escapeHtml(text)}</td></tr>`;
}

function playerName(value) {
  const match = /^player-(\d+)$/.exec(String(value ?? ""));
  return match ? `模拟玩家 ${match[1]}` : String(value || "等待玩家连接");
}

function roomName(value) {
  const match = /^room-demo-(\d+)$/.exec(String(value ?? ""));
  return match ? `第 ${match[1]} 场比赛` : String(value || "比赛房间");
}

function settlementName(value) {
  const match = /^settlement-demo-(\d+)$/.exec(String(value ?? ""));
  return match ? `第 ${match[1]} 场比赛成绩` : "比赛成绩记录";
}

function translateState(value) {
  return stateLabels[value] || value || "未知";
}

function translateEventMessage(value) {
  if (eventMessages[value]) return eventMessages[value];
  const hello = /^player-(\d+) completed Hello$/.exec(String(value ?? ""));
  if (hello) return `模拟玩家 ${hello[1]} 已完成连接确认。`;
  const matched = /^player-(\d+) matched into room$/.exec(String(value ?? ""));
  if (matched) return `模拟玩家 ${matched[1]} 已经匹配进入比赛房间。`;
  return value || "服务器记录了一次状态变化。";
}

async function getJson(path) {
  const response = await fetch(`/api/${path}`, { cache: "no-store" });
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
  return response.json();
}

function renderOverview(data) {
  const counts = data.counts || {};
  byId("metric-sessions").textContent = counts.sessions_online ?? 0;
  byId("metric-tickets").textContent = counts.tickets_queued ?? 0;
  byId("metric-rooms").textContent = counts.rooms_total ?? 0;
  byId("metric-settlements").textContent = counts.settlements ?? 0;
  const time = String(data.generated_at || "").slice(11, 19);
  byId("updated-at").textContent = time ? `数据更新 ${time}` : "等待数据";
  const health = Object.entries(data.health || {});
  byId("health-list").innerHTML = health.length ? health.map(([name, value]) => `
    <article class="health-item ${value.healthy ? "healthy" : ""}">
      <div><strong>${escapeHtml(healthNames[name] || name)}</strong><span class="health-state"><i></i>${value.healthy ? "正常" : "需要注意"}</span></div>
      <p>${escapeHtml(healthDetails[value.detail] || value.detail)}</p>
    </article>`).join("") : '<p class="empty">暂无运行状态</p>';
}

function renderSessions(data) {
  const items = data.items || [];
  byId("sessions-body").innerHTML = items.length ? items.map((item) => `
    <tr><td>${escapeHtml(playerName(item.player_id))}</td><td><span class="state">${escapeHtml(translateState(item.state))}</span></td>
    <td>第 ${escapeHtml(item.generation)} 次</td><td><code>${escapeHtml(item.trace_id)}</code></td></tr>`).join("") : emptyRow(4);
}

function renderRooms(data) {
  const items = data.items || [];
  byId("room-cards").innerHTML = items.length ? items.map((room) => `
    <article class="room-card"><header><strong>${escapeHtml(roomName(room.room_id))}</strong><span class="state">${escapeHtml(translateState(room.state))}</span></header>
    <p>${escapeHtml((room.players || []).map(playerName).join(" 对战 "))}</p><p>获胜者：${escapeHtml(playerName(room.winner_id || ""))}</p>
    <p><code>内部追踪：${escapeHtml(room.trace_id)}</code></p></article>`).join("") : '<p class="empty">运行第 1 项后显示比赛房间</p>';
}

function renderRankings(data) {
  const board = (data.leaderboards || [])[0];
  const entries = board?.entries || [];
  const cache = byId("cache-state");
  const cacheLabels = { fresh: "数据已是最新", stale: "等待自动修复", unknown: "等待数据" };
  cache.textContent = cacheLabels[board?.cache_state || "unknown"] || "状态未知";
  cache.className = `tag ${board?.cache_state === "fresh" ? "fresh" : ""}`;
  byId("rankings-body").innerHTML = entries.length ? entries.map((entry) => `
    <tr><td>第 ${escapeHtml(entry.rank)} 名</td><td>${escapeHtml(playerName(entry.player_id))}</td><td>${escapeHtml(entry.score)} 分</td></tr>`).join("") : emptyRow(3);
  const settlements = data.settlements || [];
  byId("settlement-list").innerHTML = settlements.length ? settlements.slice(-6).reverse().map((item) => `
    <article class="settlement"><div><strong>${escapeHtml(settlementName(item.settlement_id))}</strong>
      <p>${escapeHtml(playerName(item.winner_id))} 获胜 · ${escapeHtml(translateState(item.status))}</p>
      <code>内部编号：${escapeHtml(item.settlement_id)}</code></div>
      ${item.idempotent_replay ? '<span class="replay">重复提交已识别</span>' : ""}</article>`).join("") : '<p class="empty">运行第 1 项后显示比赛成绩</p>';
}

function renderQa(data) {
  const items = data.items || [];
  byId("qa-runs").innerHTML = items.length ? items.map((run) => `
    <article class="qa-card"><header><span class="qa-label">演示自动检查</span><span class="passed">${run.status === "passed" ? "测试通过" : "测试未通过"}</span></header>
      <h3>${escapeHtml(qaNames[run.name] || run.name)}</h3><p>${escapeHtml(qaDetails[run.detail] || run.detail)}</p>
      <div class="qa-meta"><span>耗时 ${escapeHtml(run.duration_ms)} 毫秒</span><span>${escapeHtml(run.bug_id ? `关联问题 ${run.bug_id}` : "没有关联问题")}</span></div>
      <code class="qa-id">内部编号：${escapeHtml(run.run_id)}</code></article>`).join("") : '<p class="empty">运行演示后显示自动检查记录</p>';
}

async function renderTimeline() {
  const trace = byId("trace-filter").value.trim();
  const query = new URLSearchParams({ after_seq: "0", limit: "100" });
  if (trace) query.set("trace_id", trace);
  const data = await getJson(`events?${query}`);
  const items = data.items || [];
  byId("timeline").innerHTML = items.length ? items.slice().reverse().map((event) => `
    <article class="event ${escapeHtml(event.severity)}"><time>第 ${escapeHtml(event.sequence)} 条 · ${escapeHtml(event.timestamp.slice(11, 19))}</time>
      <div><strong>${escapeHtml(eventTypes[event.type] || event.type)}</strong><p>${escapeHtml(translateEventMessage(event.message))}</p></div>
      <code>${escapeHtml(event.trace_id || "系统事件")}</code></article>`).join("") : '<p class="empty">没有符合筛选条件的处理记录</p>';
}

function inferScenario(qa) {
  const scenarios = new Set((qa.items || []).map((item) => item.scenario_id));
  if (scenarios.has("redis-repair")) return "redis-repair";
  if (scenarios.has("duplicate-settlement")) return "duplicate-settlement";
  if (scenarios.has("eight-player-flow")) return "eight-player-flow";
  return "";
}

function resultFact(value, explanation) {
  return `<div class="result-fact"><strong>${escapeHtml(value)}</strong><span>${escapeHtml(explanation)}</span></div>`;
}

function setResultCard(mark, kicker, title, summary, facts, proof, state = "success") {
  const card = byId("scenario-result");
  card.className = `result-card ${state}`;
  card.innerHTML = `
    <div class="result-mark">${escapeHtml(mark)}</div>
    <div class="result-main">
      <p class="result-kicker">${escapeHtml(kicker)}</p>
      <h2>${escapeHtml(title)}</h2>
      <p class="result-summary">${escapeHtml(summary)}</p>
      ${facts.length ? `<div class="result-facts">${facts.join("")}</div>` : ""}
      ${proof ? `<p class="result-proof"><strong>这证明了：</strong>${escapeHtml(proof)}</p>` : ""}
    </div>`;
}

function renderScenarioResult(scenario, snapshot) {
  if (!scenario) return;
  const counts = snapshot.overview.counts || {};
  const settlements = snapshot.rankings.settlements || [];
  const qaItems = snapshot.qa.items || [];

  if (scenario === "eight-player-flow") {
    const passed = counts.sessions_online === 8 && counts.rooms_total === 4 && counts.settlements === 4;
    if (!passed) return renderUnexpectedResult();
    setResultCard("✓", "第 1 项完成 · 结果正确", "完整比赛演示通过",
      "刚才服务器让 8 名模拟玩家进入，自动组成 4 场一对一比赛，确定胜者，并保存了 4 份比赛成绩。",
      [
        resultFact(`${counts.sessions_online} 名玩家`, "已经连接到服务器"),
        resultFact(`${counts.rooms_total} 场比赛`, "4 个房间都已结束"),
        resultFact(`${counts.settlements} 份成绩`, "每场比赛保存 1 份")
      ],
      "服务器能够完成“玩家连接 → 匹配 → 比赛 → 保存成绩 → 更新排行”的完整流程。");
    return;
  }

  if (scenario === "duplicate-settlement") {
    const replayed = settlements.some((item) => item.idempotent_replay);
    const passed = counts.settlements === 4 && replayed;
    if (!passed) return renderUnexpectedResult();
    setResultCard("✓", "第 2 项完成 · 结果正确", "重复提交保护通过",
      "刚才服务器再次收到了第 1 场比赛的相同成绩。它识别出这是重复请求，没有再次加分，也没有新增第 5 份成绩。",
      [
        resultFact(`${counts.settlements} 份成绩`, "数量仍是 4，没有变成 5"),
        resultFact("只计分一次", "同一场比赛不会重复加分"),
        resultFact("返回原结果", "重复请求安全复用第一次结果")
      ],
      "即使网络重试或重复点击，玩家也不会利用重复请求获得两次分数。");
    return;
  }

  if (scenario === "redis-repair") {
    const redis = snapshot.overview.health?.redis;
    const board = (snapshot.rankings.leaderboards || [])[0];
    const qaPassed = qaItems.some((item) => item.scenario_id === "redis-repair" && item.status === "passed");
    const passed = redis?.healthy && board?.cache_state === "fresh" && qaPassed;
    if (!passed) return renderUnexpectedResult();
    setResultCard("✓", "第 3 项完成 · 结果正确", "排行榜故障已经自动恢复",
      "刚才程序模拟排行榜缓存短暂不可用。服务器没有丢失真实比赛成绩，并在缓存恢复后自动把排行榜修复到最新状态。",
      [
        resultFact("模拟一次故障", "排行榜曾短暂无法更新"),
        resultFact("真实成绩未丢失", "主数据库仍保存正确结果"),
        resultFact("排行榜已恢复", "当前数据重新是最新的")
      ],
      "排行榜临时故障不会破坏真实比赛成绩，系统能够在恢复后自动补齐数据。");
  }
}

function renderUnexpectedResult() {
  setResultCard("!", "结果需要检查", "本次结果与预期不一致",
    "你不需要排查原因。页面已经保留技术明细，交给 Codex 检查即可。", [], "", "error");
}

function renderLoading(button) {
  setResultCard("…", "正在运行", button.querySelector("strong").textContent,
    "服务器正在准备演示数据并完成自动检查，通常只需要一瞬间。", [], "", "loading");
}

async function refresh() {
  try {
    const [overview, sessions, , rooms, rankings, qa] = await Promise.all(endpoints.map(getJson));
    const snapshot = { overview, sessions, rooms, rankings, qa };
    renderOverview(overview);
    renderSessions(sessions);
    renderRooms(rooms);
    renderRankings(rankings);
    renderQa(qa);
    await renderTimeline();
    byId("poll-status").className = "live-pill online";
    byId("poll-status").innerHTML = "<i></i>服务器运行正常";
    if (!scenarioRunning && !scenarioError) {
      const scenario = selectedScenario || inferScenario(qa);
      renderScenarioResult(scenario, snapshot);
    }
    return snapshot;
  } catch (error) {
    byId("poll-status").className = "live-pill";
    byId("poll-status").innerHTML = "<i></i>暂时无法读取状态";
    console.error(error);
    throw error;
  }
}

async function runScenario(button) {
  const scenario = button.dataset.scenario;
  scenarioRunning = true;
  scenarioError = "";
  selectedScenario = scenario;
  document.querySelectorAll(".scenario").forEach((item) => { item.disabled = true; });
  renderLoading(button);
  try {
    const response = await fetch(`/api/demo/scenarios/${encodeURIComponent(scenario)}`, {
      method: "POST",
      headers: { "X-Arena-Demo": "1" }
    });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
    const snapshot = await refresh();
    renderScenarioResult(scenario, snapshot);
  } catch (error) {
    scenarioError = error.message;
    setResultCard("!", "演示没有完成", "服务器暂时无法完成这项操作",
      "你不需要解决这个问题，把当前页面交给 Codex 检查即可。", [], "", "error");
  } finally {
    scenarioRunning = false;
    document.querySelectorAll(".scenario").forEach((item) => { item.disabled = false; });
  }
}

document.querySelectorAll(".scenario").forEach((button) => button.addEventListener("click", () => runScenario(button)));
byId("trace-filter").addEventListener("input", () => {
  clearTimeout(refreshTimer);
  refreshTimer = setTimeout(renderTimeline, 180);
});
refresh().catch(() => {});
setInterval(() => refresh().catch(() => {}), 2000);
