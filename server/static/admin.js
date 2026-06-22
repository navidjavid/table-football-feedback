// Admin panel JS.
// Polls every 3s for Picos and tables/players (no SSE here — refresh on action).

(function () {
  "use strict";

  const picosBody = document.querySelector("#picos-table tbody");
  const tablesList = document.getElementById("tables-list");
  const adminPlayersBody = document.querySelector("#admin-players-table tbody");

  let cachedTables = [];
  let cachedPlayers = [];

  refreshAll();
  setInterval(refreshAll, 3000);

  document.getElementById("logout-btn").addEventListener("click", async () => {
    await fetch("/api/admin/logout", {method: "POST"});
    window.location.href = "/admin/login";
  });

  document.getElementById("create-table-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const name = document.getElementById("new-table-name").value.trim();
    const location = document.getElementById("new-table-location").value.trim();
    if (!name) return;
    const r = await postJSON("/api/admin/table", {name, location});
    if (r.ok) {
      document.getElementById("new-table-name").value = "";
      document.getElementById("new-table-location").value = "";
      refreshAll();
    } else { alert(r.error || "Failed to create table"); }
  });

  // Generic prompt modal close handlers.
  document.querySelectorAll(".modal").forEach((m) => {
    m.addEventListener("click", (e) => {
      if (e.target === m || e.target.matches("[data-close]")) closeModals();
    });
  });

  async function refreshAll() {
    await Promise.all([refreshPicos(), refreshTables(), refreshPlayers()]);
  }

  // -------- Picos --------

  async function refreshPicos() {
    const picos = await getJSON("/api/admin/picos");
    if (!Array.isArray(picos)) return;
    picosBody.innerHTML = picos.map((p) => `
      <tr>
        <td>${esc(p.pico_id)}</td>
        <td>
          <select data-action="assign-table" data-pico="${esc(p.pico_id)}">
            <option value="">—</option>
            ${cachedTables.map((t) => `<option value="${t.table_id}" ${t.table_id === p.table_id ? "selected" : ""}>${esc(t.name)}</option>`).join("")}
          </select>
        </td>
        <td>
          <select data-action="assign-side" data-pico="${esc(p.pico_id)}">
            <option value="A" ${p.side === "A" ? "selected" : ""}>A</option>
            <option value="B" ${p.side === "B" ? "selected" : ""}>B</option>
          </select>
        </td>
        <td>
          <select data-action="assign-role" data-pico="${esc(p.pico_id)}">
            <option value="primary" ${p.role === "primary" ? "selected" : ""}>primary</option>
            <option value="secondary" ${p.role === "secondary" ? "selected" : ""}>secondary</option>
          </select>
        </td>
        <td>${esc(p.ip_address || "—")}</td>
        <td>${esc(p.firmware_version || "—")}</td>
        <td><span class="dot ${p.online ? "dot-ok" : "dot-bad"}"></span> ${p.online ? "online" : "offline"}</td>
        <td>${esc(p.last_seen || "—")}</td>
        <td class="admin-actions">
          <button class="btn" data-cmd="identify"      data-pico="${esc(p.pico_id)}">Identify</button>
          <button class="btn" data-cmd="sync_players"  data-pico="${esc(p.pico_id)}">Sync Players</button>
          <button class="btn" data-cmd="reset_match"   data-pico="${esc(p.pico_id)}">Reset Match</button>
          <button class="btn" data-cmd="clear_players" data-pico="${esc(p.pico_id)}">Clear Players</button>
          <button class="btn" data-cmd="show_message"  data-pico="${esc(p.pico_id)}">Message</button>
        </td>
      </tr>`).join("");

    picosBody.querySelectorAll("button[data-cmd]").forEach((b) => {
      b.addEventListener("click", () => sendPicoCmd(b.dataset.pico, b.dataset.cmd));
    });
    picosBody.querySelectorAll("select[data-action]").forEach((sel) => {
      sel.addEventListener("change", () => onAssignChange(sel));
    });
  }

  async function sendPicoCmd(picoId, cmd) {
    if (cmd === "show_message") {
      promptModal("Show message on " + picoId, "Message text", "", async (val) => {
        if (!val) return "Message required";
        const r = await postJSON(`/api/admin/pico/${encodeURIComponent(picoId)}/command`,
                                 {cmd, message: val});
        return r.ok ? null : (r.error || "Failed");
      });
      return;
    }
    if (cmd !== "identify" && cmd !== "sync_players"
        && !confirm(`Send "${cmd}" to ${picoId}?`)) return;
    const r = await postJSON(`/api/admin/pico/${encodeURIComponent(picoId)}/command`, {cmd});
    if (!r.ok) alert(r.error || "Failed");
  }

  async function onAssignChange(sel) {
    const row = sel.closest("tr");
    const picoId = sel.dataset.pico;
    const t = row.querySelector('select[data-action="assign-table"]').value;
    const s = row.querySelector('select[data-action="assign-side"]').value;
    const r = row.querySelector('select[data-action="assign-role"]').value;
    if (!t) return;
    const res = await postJSON(`/api/admin/pico/${encodeURIComponent(picoId)}/assign`,
                               {table_id: Number(t), side: s, role: r});
    if (!res.ok) alert(res.error || "Assign failed");
  }

  // -------- Tables --------

  async function refreshTables() {
    const tables = await getJSON("/api/tables");
    if (!Array.isArray(tables)) return;
    cachedTables = tables;
    tablesList.innerHTML = tables.map((t) => tableCard(t)).join("");

    tablesList.querySelectorAll("[data-action]").forEach((el) => {
      el.addEventListener("click", () => onTableAction(el));
    });
  }

  function tableCard(t) {
    const slotsA = [1, 2].map((slot) => slotHTML(t, "A", slot)).join("");
    const slotsB = [1, 2].map((slot) => slotHTML(t, "B", slot)).join("");
    return `
    <div class="admin-table-card" data-id="${t.table_id}">
      <header>
        <strong>${esc(t.name)}</strong>
        <span class="badge ${badgeCls(t.state)}">${t.state}</span>
        <span>Score: ${t.score_a} : ${t.score_b}</span>
        <span>Max: ${t.max_score}</span>
        <span style="margin-left:auto" class="admin-actions">
          <button class="btn" data-action="rename" data-id="${t.table_id}">Rename</button>
          <button class="btn" data-action="max"    data-id="${t.table_id}">Set Max</button>
          <button class="btn" data-action="start"  data-id="${t.table_id}">Start</button>
          <button class="btn btn-danger" data-action="stop"  data-id="${t.table_id}">Stop</button>
          <button class="btn btn-danger" data-action="reset" data-id="${t.table_id}">Reset</button>
        </span>
      </header>
      <div class="slots">
        <div>
          <div style="color: var(--team-a); font-size: 12px; margin-bottom: 4px;">Side A</div>
          ${slotsA}
        </div>
        <div>
          <div style="color: var(--team-b); font-size: 12px; margin-bottom: 4px;">Side B</div>
          ${slotsB}
        </div>
      </div>
    </div>`;
  }

  function slotHTML(t, side, slot) {
    const seated = (side === "A" ? t.team_a : t.team_b).find((p) => p.slot === slot);
    if (seated) {
      return `<div class="slot">
        <span>${esc(seated.name || "?")} ${seated.is_guest ? '<span class="badge guest">G</span>' : ""}</span>
        <button class="btn btn-ghost" data-action="rm"
                data-id="${t.table_id}" data-side="${side}" data-slot="${slot}">×</button>
      </div>`;
    }
    return `<div class="slot">
      <span class="empty">Slot ${slot}</span>
      <button class="btn" data-action="add"
              data-id="${t.table_id}" data-side="${side}" data-slot="${slot}">Add</button>
    </div>`;
  }

  function badgeCls(s) {
    if (s === "GAME_PLAYING") return "playing";
    if (s === "PLAYERS_REGISTERING") return "registering";
    if (s === "GAME_OVER") return "over";
    if (s === "ABANDONED") return "abandoned";
    return "waiting";
  }

  async function onTableAction(el) {
    const id = el.dataset.id;
    const action = el.dataset.action;
    if (action === "rename") {
      promptModal("Rename table", "New name", "", async (val) => {
        if (!val) return "Name required";
        const r = await postJSON(`/api/admin/table/${id}/rename`, {name: val});
        return r.ok ? null : (r.error || "Failed");
      });
    } else if (action === "max") {
      promptModal("Set max score", "Max score (1–99)", "10", async (val) => {
        const n = Number(val);
        if (!Number.isInteger(n) || n < 1 || n > 99) return "Must be 1..99";
        const r = await postJSON(`/api/admin/table/${id}/max_score`, {max_score: n});
        return r.ok ? null : (r.error || "Failed");
      });
    } else if (action === "start") {
      const r = await postJSON(`/api/admin/table/${id}/start`, {});
      if (!r.ok) alert(r.error || "Failed");
    } else if (action === "stop") {
      if (!confirm("Stop the current game on this table?")) return;
      const r = await postJSON(`/api/admin/table/${id}/stop`, {});
      if (!r.ok) alert(r.error || "Failed");
    } else if (action === "reset") {
      if (!confirm("Reset this table to WAITING and clear players?")) return;
      const r = await postJSON(`/api/admin/table/${id}/reset`, {});
      if (!r.ok) alert(r.error || "Failed");
    } else if (action === "rm") {
      const r = await postJSON(`/api/admin/table/${id}/remove_player`,
                               {side: el.dataset.side, slot: Number(el.dataset.slot)});
      if (!r.ok) alert(r.error || "Failed");
    } else if (action === "add") {
      const list = cachedPlayers
        .map((p) => `${p.id}: ${p.name}${p.is_guest ? " (Guest)" : ""}`)
        .join("\n");
      const raw = prompt("Player ID to add to " + el.dataset.side + " slot " + el.dataset.slot + ":\n\n" + list);
      const pid = Number(raw);
      if (!Number.isInteger(pid)) return;
      const r = await postJSON(`/api/admin/table/${id}/add_player`,
                               {side: el.dataset.side, slot: Number(el.dataset.slot), player_id: pid});
      if (!r.ok) alert(r.error || "Failed");
    }
    refreshAll();
  }

  // -------- Players --------

  async function refreshPlayers() {
    const players = await getJSON("/api/players");
    if (!Array.isArray(players)) return;
    cachedPlayers = players;
    adminPlayersBody.innerHTML = players.map((p) => `
      <tr>
        <td>${esc(p.name)}</td>
        <td><code>${esc(p.rfid_uid)}</code></td>
        <td>${p.is_guest ? '<span class="badge guest">Guest</span>' : '<span class="badge registered">Registered</span>'}</td>
        <td class="num">${p.games}</td>
        <td class="num">${p.wins}</td>
        <td class="admin-actions">
          <button class="btn" data-rename="${p.id}">Rename / Register</button>
          <a class="btn btn-ghost" href="/player/${encodeURIComponent(p.rfid_uid)}" target="_blank">Profile</a>
        </td>
      </tr>`).join("");

    adminPlayersBody.querySelectorAll("button[data-rename]").forEach((b) => {
      b.addEventListener("click", () => {
        const id = b.dataset.rename;
        const current = cachedPlayers.find((p) => String(p.id) === id);
        promptModal("Rename / register player",
                    "Player name",
                    current ? current.name : "",
                    async (val) => {
                      const r = await postJSON(`/api/admin/player/${id}/rename`, {name: val});
                      return r.ok ? null : (r.error || "Failed");
                    });
      });
    });
  }

  // -------- Prompt modal --------

  function promptModal(title, label, initial, onSubmit) {
    const modal = document.getElementById("prompt-modal");
    document.getElementById("prompt-title").textContent = title;
    document.getElementById("prompt-label").textContent = label;
    const input = document.getElementById("prompt-input");
    const err = document.getElementById("prompt-error");
    input.value = initial || "";
    err.textContent = "";
    modal.classList.remove("hidden");
    setTimeout(() => input.focus(), 30);

    const form = document.getElementById("prompt-form");
    const handler = async (e) => {
      e.preventDefault();
      err.textContent = "";
      const msg = await onSubmit(input.value.trim());
      if (msg) { err.textContent = msg; return; }
      form.removeEventListener("submit", handler);
      closeModals();
      refreshAll();
    };
    form.addEventListener("submit", handler, {once: false});
    // Reset listener on close to avoid leaking handlers across opens.
    modal.dataset.cleanup = "true";
    modal.addEventListener("click", function once(e) {
      if (e.target === modal || e.target.matches("[data-close]")) {
        form.removeEventListener("submit", handler);
        modal.removeEventListener("click", once);
      }
    });
  }

  function closeModals() {
    document.querySelectorAll(".modal").forEach((m) => m.classList.add("hidden"));
  }

  // -------- HTTP helpers --------

  async function getJSON(url) {
    try {
      const r = await fetch(url);
      if (r.status === 401) { window.location.href = "/admin/login"; return null; }
      if (!r.ok) return null;
      return await r.json();
    } catch { return null; }
  }
  async function postJSON(url, body) {
    try {
      const r = await fetch(url, {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body),
      });
      if (r.status === 401) { window.location.href = "/admin/login"; return {ok: false, error: "Login required"}; }
      const data = await r.json().catch(() => ({}));
      return {ok: r.ok, ...data};
    } catch (e) {
      return {ok: false, error: "Network error"};
    }
  }

  function esc(s) {
    return String(s ?? "").replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;",
    }[c]));
  }
})();
