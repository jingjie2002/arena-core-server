const byId = (id) => document.getElementById(id);
const endpoints = ["overview", "sessions", "tickets", "rooms", "rankings", "qa/runs"];
let refreshTimer;

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
  observer: "只读观察接口",
  tcp: "玩家连接入口",
  mysql: "比赛成绩数据库",
  redis: "排行榜缓存"
};

const healthDetails = {
  accepting: "正在正常接收玩家连接",
  "transaction source ready": "可以正常保存比赛成绩",
  "rank cache fresh": "排行榜数据已经是最新的",
  "repaired from MySQL; cache fresh": "已根据真实成绩自动恢复，排行榜是最新的",
  "simulated timeout; MySQL remains authoritative": "排行缓存暂时不可用，MySQL 结算事实仍然安全",
  "demo mode": "旧版本地演示模式",
  "read only": "当前页面只读取状态，不修改业务数据",
  "not configured": "当前运行配置未启用该外部依赖",
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
  "observer.started": "只读观察接口已启动",
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
  "both players ready; deterministic result frozen": "历史版本已冻结确定性结果。",
  "both players ready; awaiting trusted result": "双方已准备，房间正在等待可信战斗结果。",
  "MySQL committed before Redis rank update": "比赛成绩已先保存，再更新排行榜。",
  "room closed after settlement commit": "成绩保存成功后，本场比赛安全结束。",
  "8-player / 4-room demonstration passed": "8 人、4 场比赛的完整流程已通过。",
  "same fingerprint replay returned original receipt": "发现相同比赛结果再次提交，服务器返回第一次的结果。",
  "duplicate settlement did not double-apply score": "重复提交没有导致玩家再次加分。",
  "Redis update failed; dirty row retained": "排行榜更新失败，服务器保留了待修记录。",
  "repair worker replayed MySQL target scores": "自动修复程序正在根据真实成绩重建排行榜。",
  "dirty cleared after cache reached MySQL state": "排行榜追上真实成绩后，待修记录已经清除。",
  "Redis failure recovery passed": "排行榜故障恢复检查通过。",
  "observer API listening": "只读观察接口已经可以读取服务状态。"
};

function emptyRow(columns, text = "当前没有运行数据") {
  return `<tr><td class="empty" colspan="${columns}">${escapeHtml(text)}</td></tr>`;
}

function playerName(value) {
  const match = /^player-(\d+)$/.exec(String(value ?? ""));
  return match ? `玩家 ${match[1]}` : String(value || "等待玩家连接");
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
  if (hello) return `玩家 ${hello[1]} 已完成连接确认。`;
  const matched = /^player-(\d+) matched into room$/.exec(String(value ?? ""));
  if (matched) return `玩家 ${matched[1]} 已经匹配进入比赛房间。`;
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
    <p><code>内部追踪：${escapeHtml(room.trace_id)}</code></p></article>`).join("") : '<p class="empty">当前没有比赛房间</p>';
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
      ${item.idempotent_replay ? '<span class="replay">重复提交已识别</span>' : ""}</article>`).join("") : '<p class="empty">当前没有已提交比赛成绩</p>';
}

function renderQa(data) {
  const items = data.items || [];
  byId("qa-runs").innerHTML = items.length ? items.map((run) => `
    <article class="qa-card"><header><span class="qa-label">QA 检查</span><span class="passed">${run.status === "passed" ? "测试通过" : "测试未通过"}</span></header>
      <h3>${escapeHtml(qaNames[run.name] || run.name)}</h3><p>${escapeHtml(qaDetails[run.detail] || run.detail)}</p>
      <div class="qa-meta"><span>耗时 ${escapeHtml(run.duration_ms)} 毫秒</span><span>${escapeHtml(run.bug_id ? `关联问题 ${run.bug_id}` : "没有关联问题")}</span></div>
      <code class="qa-id">内部编号：${escapeHtml(run.run_id)}</code></article>`).join("") : '<p class="empty">当前进程尚未收录 QA 记录</p>';
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
    return snapshot;
  } catch (error) {
    byId("poll-status").className = "live-pill";
    byId("poll-status").innerHTML = "<i></i>暂时无法读取状态";
    console.error(error);
    throw error;
  }
}

byId("trace-filter").addEventListener("input", () => {
  clearTimeout(refreshTimer);
  refreshTimer = setTimeout(renderTimeline, 180);
});
refresh().catch(() => {});
setInterval(() => refresh().catch(() => {}), 2000);
