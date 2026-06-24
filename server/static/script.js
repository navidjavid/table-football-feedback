// Public dashboard.
//
// Renders one of two modes:
//   - on `/`        → live tables + players list + modals
//   - on `/player/<uid>` → standalone player profile (window.PLAYER_UID set)
//
// Real-time updates come via SSE on /events. If the SSE stream drops we
// fall back to polling /api/tables every 5 seconds (still update via SSE
// once it reconnects).

(function () {
  "use strict";

  const isPlayerPage = !!window.PLAYER_UID;

  if (isPlayerPage) {
    renderPlayerPage(window.PLAYER_UID);
    return;
  }

  // ----- Dashboard mode --------------------------------------------------

  const tablesEl = document.getElementById("tables");
  const playersBodyEl = document.querySelector("#players-table tbody");
  const connDot = document.getElementById("conn-dot");
  const connText = document.getElementById("conn-text");
  const lastUpdateEl = document.getElementById("last-update");

  const state = {
    tables: new Map(),    // table_id -> snapshot
    balls:  new Map(),    // table_id -> {x, y} last known ball coords
    goalFlash: new Map(), // table_id -> {ts, side, name} - active goal banner
    players: [],
    es: null,
    pollTimer: null,
  };
  const GOAL_FLASH_MS = 2000;

  // ---- Initial load ----
  fetchTables();
  fetchPlayers();
  setInterval(fetchPlayers, 3000);
  connectSSE();

  // Re-arm SSE if the user comes back to the tab.
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && (!state.es || state.es.readyState === 2)) {
      connectSSE();
    }
  });

  // Modal close on backdrop click or [data-close] button.
  document.querySelectorAll(".modal").forEach((m) => {
    m.addEventListener("click", (e) => {
      if (e.target === m || e.target.matches("[data-close]")) closeModals();
    });
  });

  document.getElementById("register-form").addEventListener("submit", submitRegistration);

  // ---- Networking ----

  function connectSSE() {
    if (state.es) { try { state.es.close(); } catch {} }
    setConn("connecting", "Connecting...");
    try {
      const es = new EventSource("/events");
      state.es = es;

      es.addEventListener("open", () => {
        setConn("ok", "Live");
        stopPollFallback();
      });
      es.addEventListener("error", () => {
        setConn("bad", "Reconnecting...");
        startPollFallback();
      });

      es.addEventListener("table_state", (ev) => {
        try {
          const snap = JSON.parse(ev.data);
          const prev = state.tables.get(snap.table_id);
          if (prev && prev.state === "GAME_PLAYING") {
            if (snap.score_a > prev.score_a) triggerGoal(snap.table_id, "A", snap.team_a);
            if (snap.score_b > prev.score_b) triggerGoal(snap.table_id, "B", snap.team_b);
          }
          state.tables.set(snap.table_id, snap);
          if (snap.state !== "GAME_PLAYING") {
            state.balls.delete(snap.table_id);
          }
          if (snap.state === "WAITING" || snap.state === "PLAYERS_REGISTERING") {
            state.goalFlash.delete(snap.table_id);
          }
          renderTables();
          touchUpdate();
        } catch (e) { console.warn("bad table_state event", e); }
      });

      es.addEventListener("ball_position", (ev) => {
        try {
          const b = JSON.parse(ev.data);
          state.balls.set(b.table_id, {x: b.x, y: b.y});
          moveBall(b.table_id, b.x, b.y);
        } catch {}
      });

      es.addEventListener("player_update", () => {
        fetchPlayers();
      });

      es.addEventListener("pico_status", () => {
        // Pico-status changes don't move the dashboard, but they're
        // visible in the admin panel — nothing to do here.
      });
    } catch (e) {
      console.error("EventSource failed", e);
      setConn("bad", "Offline");
      startPollFallback();
    }
  }

  function startPollFallback() {
    if (state.pollTimer) return;
    state.pollTimer = setInterval(fetchTables, 5000);
  }
  function stopPollFallback() {
    if (state.pollTimer) { clearInterval(state.pollTimer); state.pollTimer = null; }
  }

  async function fetchTables() {
    try {
      const r = await fetch("/api/tables");
      if (!r.ok) return;
      const data = await r.json();
      state.tables.clear();
      data.forEach((s) => state.tables.set(s.table_id, s));
      renderTables();
      touchUpdate();
    } catch (e) { console.warn("fetchTables failed", e); }
  }

  async function fetchPlayers() {
    try {
      const r = await fetch("/api/players");
      if (!r.ok) return;
      state.players = await r.json();
      renderPlayers();
    } catch (e) { console.warn("fetchPlayers failed", e); }
  }

  // ---- Rendering ----

  function setConn(kind, label) {
    connText.textContent = label;
    connDot.className = "dot " + (kind === "ok" ? "dot-ok" : kind === "bad" ? "dot-bad" : "dot-warn");
  }
  function touchUpdate() {
    const now = new Date();
    lastUpdateEl.textContent = "updated " + now.toLocaleTimeString();
  }

  function renderTables() {
    const ids = [...state.tables.keys()].sort((a, b) => a - b);
    tablesEl.innerHTML = ids.map((id) => tableCard(state.tables.get(id))).join("");
  }

  function triggerGoal(tableId, side, team) {
    const name = (team && team.length)
      ? team.map((p) => p.name || "?").join(" + ")
      : ("Side " + side);
    state.goalFlash.set(tableId, { ts: Date.now(), side, name });
    // Force a re-render once the flash expires even if no further
    // table_state event arrives (e.g. the winning goal of the match).
    setTimeout(() => {
      const f = state.goalFlash.get(tableId);
      if (f && Date.now() - f.ts >= GOAL_FLASH_MS - 50) {
        state.goalFlash.delete(tableId);
      }
      renderTables();
    }, GOAL_FLASH_MS + 50);
  }

  function tableCard(s) {
    const stateBadge = stateBadgeFor(s.state);
    const flash = state.goalFlash.get(s.table_id);
    const flashActive = !!flash && (Date.now() - flash.ts) < GOAL_FLASH_MS;
    // Keep the field visible for the flash duration even after GAME_OVER,
    // so the final goal's banner still has somewhere to render.
    const showField = s.state === "GAME_PLAYING" || (s.state === "GAME_OVER" && flashActive);
    const fastest = s.fastest ? s.fastest.toFixed(1) : "0.0";
    const mode = s.mode ? `Mode: ${s.mode}` : "&nbsp;";

    let banner = "";
    if (s.state === "GAME_OVER") {
      const team = s.winner_side === "A" ? s.team_a : s.winner_side === "B" ? s.team_b : [];
      const names = team.map((p) => escapeHTML(p.name || "—")).join(" + ") || "—";
      banner = `<div class="winner-banner">🏆 ${names} won ${s.score_a}-${s.score_b}</div>`;
    } else if (s.state === "ABANDONED") {
      banner = `<div class="abandoned-banner">Game abandoned</div>`;
    }

    return `
    <article class="table-card" data-table-id="${s.table_id}">
      <header>
        <h3>${escapeHTML(s.name || ("Table " + s.table_id))}</h3>
        <span class="badge ${stateBadge.cls}">${stateBadge.label}</span>
      </header>

      <div class="score-row">
        <div class="side">
          <h4 class="side-name side-name-a">${sideNames(s.team_a, "Side A")}</h4>
          <div class="score score-a">${s.score_a}</div>
        </div>
        <div class="divider">:</div>
        <div class="side">
          <h4 class="side-name side-name-b">${sideNames(s.team_b, "Side B")}</h4>
          <div class="score score-b">${s.score_b}</div>
        </div>
      </div>

      ${showField ? fieldHTML(s.table_id, flashActive ? flash : null) : ""}

      ${banner}

      <div class="meta-line">
        <span>${mode}</span>
        <span>Best: <span class="fastest">${fastest} km/h</span></span>
        <span>Max: ${s.max_score}</span>
      </div>
    </article>`;
  }

  function fieldHTML(tableId, flash) {
    // Reuse the last known ball coords so re-rendering the card (which
    // happens on every score/state update) doesn't snap the ball back
    // to the centre for ~100ms until the next ball_position arrives.
    const last = state.balls.get(tableId);
    const left = last ? Math.max(0, Math.min(100, (last.x / 1000) * 100)) : 50;
    const top  = last ? Math.max(0, Math.min(100, (last.y / 500)  * 100)) : 50;

    let flashHTML = "";
    if (flash) {
      // animation-delay carries a negative offset equal to elapsed time,
      // so if this card gets torn down and recreated mid-flash (every
      // table_state re-render does that), the CSS animation keeps
      // playing from where it left off instead of restarting.
      const elapsedS = (Date.now() - flash.ts) / 1000;
      flashHTML = `<div class="goal-flash goal-flash-${flash.side.toLowerCase()}"
                        style="animation-delay:-${elapsedS}s">
        <span class="goal-flash-text">GOAL!</span>
        <span class="goal-flash-name">${escapeHTML(flash.name)}</span>
      </div>`;
    }

    return `<div class="field" id="field-${tableId}">
      <div class="goal goal-a"></div>
      <div class="goal goal-b"></div>
      ${flashHTML}
      <div class="ball" id="ball-${tableId}" style="left:${left}%; top:${top}%"></div>
    </div>`;
  }

  function sideNames(players, fallback) {
    if (!players || !players.length) return fallback;
    return players.map((p) => escapeHTML(p.name || "?")).join(" + ");
  }

  function stateBadgeFor(s) {
    switch (s) {
      case "GAME_PLAYING": return { cls: "playing", label: "Playing" };
      case "PLAYERS_REGISTERING": return { cls: "registering", label: "Registering" };
      case "GAME_OVER": return { cls: "over", label: "Game Over" };
      case "ABANDONED": return { cls: "abandoned", label: "Abandoned" };
      default: return { cls: "waiting", label: "Waiting" };
    }
  }

  function renderPlayers() {
    if (!playersBodyEl) return;
    playersBodyEl.innerHTML = state.players.map((p) => {
      const badge = p.is_guest
        ? `<span class="badge guest">Guest</span>`
        : `<span class="badge registered">Registered</span>`;
      const statusDot = p.status === "Active" ? "dot-ok"
        : p.status === "Waiting" ? "dot-warn" : "dot-bad";
      return `
        <tr data-id="${p.id}">
          <td>${escapeHTML(p.name)} ${badge}</td>
          <td class="uid">${escapeHTML(p.rfid_uid)}</td>
          <td class="num">${p.games}</td>
          <td class="num">${p.wins}</td>
          <td class="num">${p.losses}</td>
          <td class="num">${p.best_shot.toFixed(1)} km/h</td>
          <td><span class="dot ${statusDot}"></span> ${p.status}</td>
        </tr>`;
    }).join("");

    playersBodyEl.querySelectorAll("tr[data-id]").forEach((tr) => {
      tr.addEventListener("click", () => openPlayerProfile(tr.dataset.id));
    });
  }

  function moveBall(tableId, x, y) {
    const field = document.getElementById("field-" + tableId);
    const ball = document.getElementById("ball-" + tableId);
    if (!field || !ball) return;
    // x: 0..1000, y: 0..500 → percentages.
    const left = Math.max(0, Math.min(100, (x / 1000) * 100));
    const top = Math.max(0, Math.min(100, (y / 500) * 100));
    ball.style.left = left + "%";
    ball.style.top = top + "%";
  }

  // ---- Modals ----

  async function openPlayerProfile(playerId) {
    const modal = document.getElementById("profile-modal");
    const body = document.getElementById("profile-body");
    body.innerHTML = `<div class="loading">Loading...</div>`;
    modal.classList.remove("hidden");
    try {
      const r = await fetch("/api/player/" + playerId);
      if (!r.ok) {
        body.innerHTML = `<div class="loading">Player not found.</div>`;
        return;
      }
      const p = await r.json();
      body.innerHTML = profileHTML(p);
      const regBtn = body.querySelector("[data-register]");
      if (regBtn) regBtn.addEventListener("click", () => openRegisterDialog(p));
    } catch {
      body.innerHTML = `<div class="loading">Could not load profile.</div>`;
    }
  }

  function profileHTML(p) {
    const badge = p.is_guest
      ? `<span class="badge guest">Guest</span>`
      : `<span class="badge registered">Registered</span>`;
    const rows = (p.matches || []).map(matchRow).join("")
      || `<tr><td colspan="6" class="loading">No matches yet</td></tr>`;
    const regBtn = p.is_guest
      ? `<button class="btn btn-primary" data-register>Register This Player</button>`
      : "";
    return `
      <div class="profile-head">
        <div>
          <div class="name">${escapeHTML(p.name)}</div>
          <div class="uid">${escapeHTML(p.rfid_uid)} &nbsp;${badge}</div>
        </div>
        <div style="margin-left:auto">${regBtn}</div>
      </div>
      <div class="stat-row">
        <div class="stat"><div class="v">${p.games}</div><div class="l">Games</div></div>
        <div class="stat"><div class="v">${p.wins}</div><div class="l">Wins</div></div>
        <div class="stat"><div class="v">${p.losses}</div><div class="l">Losses</div></div>
        <div class="stat"><div class="v">${(p.best_shot||0).toFixed(1)}</div><div class="l">Best km/h</div></div>
      </div>
      <h3>Match history</h3>
      <table class="history-table">
        <thead><tr>
          <th>Date</th><th>Table</th><th>Match</th><th>Score</th><th>Result</th><th>Fastest</th>
        </tr></thead>
        <tbody>${rows}</tbody>
      </table>`;
  }

  function matchRow(m) {
    const mine = (m.teammates || []).length
      ? `you + ${m.teammates.map(escapeHTML).join(" + ")}`
      : "you";
    const opp = (m.opponents || []).map(escapeHTML).join(" + ") || "—";
    const resultCls = m.result === "Win" ? "result-win"
      : m.result === "Loss" ? "result-loss"
      : "result-abandoned";
    return `<tr>
      <td>${formatDate(m.date)}</td>
      <td>${m.table_id}</td>
      <td>${mine} vs ${opp}</td>
      <td>${escapeHTML(m.score)}</td>
      <td class="${resultCls}">${m.result}</td>
      <td>${(m.fastest_shot||0).toFixed(1)} km/h</td>
    </tr>`;
  }

  function openRegisterDialog(player) {
    document.getElementById("reg-uid").value = player.rfid_uid;
    document.getElementById("reg-current").value = player.name;
    document.getElementById("reg-new").value = "";
    document.getElementById("reg-error").textContent = "";
    document.getElementById("register-form").dataset.playerId = player.id;
    document.getElementById("register-modal").classList.remove("hidden");
    setTimeout(() => document.getElementById("reg-new").focus(), 30);
  }

  async function submitRegistration(e) {
    e.preventDefault();
    const form = e.currentTarget;
    const id = form.dataset.playerId;
    const name = document.getElementById("reg-new").value.trim();
    const errEl = document.getElementById("reg-error");
    errEl.textContent = "";
    if (!name) { errEl.textContent = "Name required"; return; }
    try {
      const r = await fetch("/api/register-player", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({player_id: Number(id), name}),
      });
      const data = await r.json().catch(() => ({}));
      if (!r.ok) {
        errEl.textContent = data.error || "Could not register";
        return;
      }
      closeModals();
      fetchPlayers();
      // If the profile modal is still open with this player, refresh it.
      const profile = document.getElementById("profile-modal");
      if (!profile.classList.contains("hidden")) openPlayerProfile(id);
    } catch {
      errEl.textContent = "Network error";
    }
  }

  function closeModals() {
    document.querySelectorAll(".modal").forEach((m) => m.classList.add("hidden"));
  }

  // ---- Utils ----

  function escapeHTML(s) {
    return String(s ?? "").replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;",
    }[c]));
  }
  function formatDate(iso) {
    if (!iso) return "—";
    // "2026-06-22 14:30:00" → "22 Jun 2026 14:30"
    const d = new Date(iso.replace(" ", "T") + "Z");
    if (isNaN(d.getTime())) return iso;
    const months = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
    const pad = (n) => n.toString().padStart(2, "0");
    return `${pad(d.getDate())} ${months[d.getMonth()]} ${d.getFullYear()} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  // ----- Standalone player profile mode --------------------------------

  function renderPlayerPage(uid) {
    const card = document.getElementById("profile-card");
    fetch("/api/player/by-uid/" + encodeURIComponent(uid))
      .then((r) => r.ok ? r.json() : Promise.reject(r))
      .then((p) => {
        card.innerHTML = playerPageHTML(p);
        renderShotChart(p);
      })
      .catch(() => {
        card.innerHTML = `<div class="loading">No player for UID ${escapeHTML(uid)}.</div>`;
      });
  }

  function playerPageHTML(p) {
    const badge = p.is_guest
      ? `<span class="badge guest">Guest</span>`
      : `<span class="badge registered">Registered</span>`;
    const rows = (p.matches || []).map(matchRow).join("")
      || `<tr><td colspan="6" class="loading">No matches yet</td></tr>`;
    return `
      <div class="profile-head">
        <div>
          <div class="name">${escapeHTML(p.name)}</div>
          <div class="uid">${escapeHTML(p.rfid_uid)} &nbsp;${badge}</div>
        </div>
      </div>
      <div class="stat-row">
        <div class="stat"><div class="v">${p.games}</div><div class="l">Games</div></div>
        <div class="stat"><div class="v">${p.wins}</div><div class="l">Wins</div></div>
        <div class="stat"><div class="v">${p.losses}</div><div class="l">Losses</div></div>
        <div class="stat"><div class="v">${(p.best_shot||0).toFixed(1)}</div><div class="l">Best km/h</div></div>
      </div>
      <h3>Fastest shot per match</h3>
      <canvas id="shot-chart" height="120"></canvas>
      <h3 style="margin-top:18px">Match history</h3>
      <table class="history-table">
        <thead><tr><th>Date</th><th>Table</th><th>Match</th><th>Score</th><th>Result</th><th>Fastest</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>`;
  }

  function renderShotChart(p) {
    const el = document.getElementById("shot-chart");
    if (!el || !window.Chart) return;
    const matches = (p.matches || []).slice().reverse();
    if (matches.length === 0) { el.style.display = "none"; return; }
    new Chart(el, {
      type: "line",
      data: {
        labels: matches.map((m) => formatDate(m.date)),
        datasets: [{
          label: "Fastest shot (km/h)",
          data: matches.map((m) => m.fastest_shot || 0),
          borderColor: "#b48bff",
          backgroundColor: "rgba(180,139,255,0.18)",
          fill: true,
          tension: 0.25,
        }],
      },
      options: {
        plugins: { legend: { labels: { color: "#9aa3b2" } } },
        scales: {
          x: { ticks: { color: "#9aa3b2", maxRotation: 0, autoSkip: true }, grid: { color: "#2c3340" } },
          y: { ticks: { color: "#9aa3b2" }, grid: { color: "#2c3340" } },
        },
      },
    });
  }
})();
