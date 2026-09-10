"use strict";
// edenadmin browser shell. Stage 6.1: Connection panel only. The token arrives
// as the URL fragment (#t=...) so it never lands in a server log; we stash it in
// sessionStorage and send it as a header on every API call.

(function grabToken() {
  const m = location.hash.match(/(?:^|[#&])t=([^&]+)/);
  if (m) {
    sessionStorage.setItem("edenadmin-token", decodeURIComponent(m[1]));
    history.replaceState(null, "", location.pathname + location.search);
  }
})();

const TOKEN = sessionStorage.getItem("edenadmin-token") || "";

async function api(path, body) {
  const opts = {
    method: body === undefined ? "GET" : "POST",
    headers: { "X-Edenadmin-Token": TOKEN },
  };
  if (body !== undefined) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(path, opts);
  let env;
  try { env = await res.json(); }
  catch { throw new Error(`${path}: HTTP ${res.status}, non-JSON reply`); }
  if (!env.ok) throw new Error(env.error || `${path}: HTTP ${res.status}`);
  return env.data;
}

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  for (const k of kids) n.append(k);
  return n;
};

let STATE = { active: "", profiles: {} };

function fillProfileSelect() {
  const sel = $("#profile-select");
  sel.innerHTML = "";
  const names = Object.keys(STATE.profiles).sort();
  if (names.length === 0) {
    sel.append(el("option", { value: "", textContent: "(no profiles yet)" }));
  }
  for (const n of names) {
    const label = `${n} (${STATE.profiles[n].kind})`;
    sel.append(el("option", { value: n, textContent: label, selected: n === STATE.active }));
  }
  $("#active-profile").textContent = STATE.active
    ? `${STATE.active} — ${STATE.profiles[STATE.active]?.kind || "?"}`
    : "no active profile";
  syncEditor();
}

function syncEditor() {
  const name = $("#profile-select").value;
  const p = STATE.profiles[name];
  const form = $("#profile-form");
  if (p) {
    for (const field of form.elements) {
      if (!field.name) continue;
      if (field.name === "args") field.value = (p.args || []).join(" ");
      else if (field.name in p) field.value = p[field.name] ?? "";
    }
  }
  toggleKindFields(form.kind.value);
}

function toggleKindFields(kind) {
  document.querySelectorAll(".local-only").forEach(n => n.classList.toggle("hidden", kind !== "local"));
  document.querySelectorAll(".vps-only").forEach(n => n.classList.toggle("hidden", kind !== "vps"));
}

function renderProbes(probes) {
  const box = $("#test-results");
  box.innerHTML = "";
  if (!probes || probes.length === 0) { box.append(el("p", { className: "muted", textContent: "No probes ran." })); return; }
  for (const pr of probes) {
    const card = el("div", { className: "probe " + (pr.ok ? "ok" : "bad") });
    card.append(el("span", { className: "name", textContent: (pr.ok ? "✓ " : "✗ ") + pr.name }));
    if (pr.command) card.append(el("code", { className: "cmd", textContent: pr.command }));
    if (pr.detail) card.append(el("span", { className: "detail", textContent: pr.detail }));
    box.append(card);
  }
}

async function refresh() {
  STATE = await api("/api/state");
  fillProfileSelect();
}

document.addEventListener("DOMContentLoaded", () => {
  $("#profile-select").addEventListener("change", () => {
    syncEditor();
    LOG_LINES = [];
    if (CURRENT_PANEL === "logs") loadLogs(true);
    else if (CURRENT_PANEL === "audit") loadAudit();
    else if (CURRENT_PANEL === "bans") loadBans();
    else if (CURRENT_PANEL === "worlds") loadWorlds();
    else if (CURRENT_PANEL === "backups") loadBackups();
    else if (CURRENT_PANEL === "config") loadConfig();
    else pollNow();
  });
  $("#profile-form").kind.addEventListener("change", e => toggleKindFields(e.target.value));

  $("#activate-btn").addEventListener("click", async () => {
    const name = $("#profile-select").value;
    if (!name) return;
    try { await api("/api/profiles/activate", { name }); await refresh(); }
    catch (e) { alert(e.message); }
  });

  $("#test-btn").addEventListener("click", async () => {
    const name = $("#profile-select").value;
    if (!name) { alert("No profile selected."); return; }
    $("#test-btn").disabled = true;
    $("#test-btn").textContent = "Testing…";
    try {
      const out = await api("/api/connection/test", { name });
      renderProbes(out.probes);
    } catch (e) {
      renderProbes([{ name: "test failed", ok: false, detail: e.message }]);
    } finally {
      $("#test-btn").disabled = false;
      $("#test-btn").textContent = "Test connection";
    }
  });

  $("#profile-form").addEventListener("submit", async e => {
    e.preventDefault();
    const fd = new FormData(e.target);
    const p = { name: fd.get("name"), kind: fd.get("kind") };
    for (const [k, v] of fd.entries()) {
      if (k === "name" || k === "kind") continue;
      if (k === "args") { if (v.trim()) p.args = v.trim().split(/\s+/); continue; }
      if (v !== "") p[k] = v;
    }
    try {
      await api("/api/profiles/save", p);
      await refresh();
      $("#profile-select").value = p.name;
      syncEditor();
    } catch (err) { alert(err.message); }
  });

  $("#delete-btn").addEventListener("click", async () => {
    const name = $("#profile-form").name.value;
    if (!name || !confirm(`Delete profile "${name}"?`)) return;
    try { await api("/api/profiles/delete", { name }); await refresh(); }
    catch (e) { alert(e.message); }
  });

  document.querySelectorAll("#tabs button").forEach(b => {
    b.addEventListener("click", () => selectPanel(b.dataset.panel));
  });

  document.querySelectorAll("#power-row button").forEach(b => {
    b.addEventListener("click", () => doPower(b.dataset.power));
  });
  $("#say-btn").addEventListener("click", () => {
    const text = $("#say-input").value;
    if (!text.trim()) return;
    doPlayerAction({ verb: "say", text }, `Broadcast "${text}" to all players?`, "#players-msg")
      .then(ok => { if (ok) $("#say-input").value = ""; });
  });
  $("#ban-btn").addEventListener("click", () => {
    const token = $("#ban-input").value.trim();
    if (!token) return;
    if (!confirm(`Ban "${token}"? Any matching player is disconnected.`)) return;
    api("/api/bans" + activeProfileParam(), { action: "add", token })
      .then(d => { setMsg("#bans-msg", d); $("#ban-input").value = ""; loadBans(); })
      .catch(e => setMsg("#bans-msg", { ok: false, message: e.message }));
  });
  $("#log-filter").addEventListener("input", renderLogs);
  $("#log-clear").addEventListener("click", () => { LOG_LINES = []; renderLogs(); loadLogs(true); });
  $("#audit-filter").addEventListener("input", drawAudit);

  $("#backup-refresh").addEventListener("click", loadBackups);
  $("#backup-now").addEventListener("click", backupNow);

  $("#config-refresh").addEventListener("click", loadConfig);
  $("#config-form").addEventListener("submit", saveConfig);

  $("#worlds-refresh").addEventListener("click", loadWorlds);
  $("#up-btn").addEventListener("click", uploadBundle);
  $("#imp-project").addEventListener("click", () => runImport("project"));
  $("#imp-write").addEventListener("click", () => runImport("write"));

  refresh().catch(e => renderProbes([{ name: "could not load state", ok: false, detail: e.message }]));
  startPolling();
});

// --- panel switching + polling ---------------------------------------------

let CURRENT_PANEL = "connection";

function selectPanel(name) {
  CURRENT_PANEL = name;
  document.querySelectorAll("#tabs button").forEach(x => x.classList.toggle("active", x.dataset.panel === name));
  document.querySelectorAll(".panel").forEach(p => p.classList.toggle("hidden", p.id !== "panel-" + name));
  if (name === "logs") loadLogs(true);
  else if (name === "audit") loadAudit();
  else if (name === "bans") loadBans();
  else if (name === "worlds") loadWorlds();
  else if (name === "backups") loadBackups();
  else if (name === "config") loadConfig();
  else pollNow();
}

// --- write actions (stage 6.3) -------------------------------------------------

function setMsg(sel, d) {
  const n = $(sel);
  if (!n) return;
  n.textContent = (d.ok ? "✓ " : "✗ ") + (d.message || "");
  n.classList.toggle("bad-text", !d.ok);
}

async function doPower(action) {
  const prompts = {
    start: "Start the server?",
    stop: "Stop the server gracefully? The world is saved first.",
    restart: "Restart the server? Every player is disconnected.",
    kill: "Force-kill the server? Edits since the last autosave (up to ~15 s) are lost.",
  };
  if (!confirm(prompts[action] || action + "?")) return;
  const btns = document.querySelectorAll("#power-row button");
  btns.forEach(b => b.disabled = true);
  $("#power-msg").textContent = action + "…";
  try {
    const d = await api("/api/power" + activeProfileParam(), { action });
    setMsg("#power-msg", d);
    if (d.status) renderStatus(d.status);
  } catch (e) {
    setMsg("#power-msg", { ok: false, message: e.message });
  } finally {
    btns.forEach(b => b.disabled = false);
  }
}

async function doPlayerAction(body, confirmMsg, msgSel) {
  if (confirmMsg && !confirm(confirmMsg)) return false;
  try {
    const d = await api("/api/players/action" + activeProfileParam(), body);
    setMsg(msgSel || "#players-msg", d);
    await pollNow();
    return d.ok;
  } catch (e) {
    setMsg(msgSel || "#players-msg", { ok: false, message: e.message });
    return false;
  }
}

async function loadBans() {
  try {
    renderBans(await api("/api/bans" + activeProfileParam()));
  } catch (e) {
    $("#bans-body").innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function renderBans(d) {
  const box = $("#bans-body");
  box.innerHTML = "";
  if (d.note) { box.append(el("p", { className: "muted", textContent: d.note })); return; }
  if (!d.entries || d.entries.length === 0) { box.append(el("p", { className: "muted", textContent: "No bans." })); return; }
  const t = el("table", { className: "grid-table" });
  t.append(el("tr", {}, el("th", { textContent: "Token" }), el("th", { textContent: "Kind" }), el("th", { textContent: "" })));
  for (const b of d.entries) {
    const unban = el("button", { textContent: "Unban" });
    unban.addEventListener("click", async () => {
      if (!confirm(`Unban "${b.token}"?`)) return;
      try {
        setMsg("#bans-msg", await api("/api/bans" + activeProfileParam(), { action: "remove", token: b.token }));
      } catch (e) { setMsg("#bans-msg", { ok: false, message: e.message }); }
      loadBans();
    });
    t.append(el("tr", {}, el("td", { textContent: b.token }), el("td", { textContent: b.kind }), el("td", {}, unban)));
  }
  box.append(t);
}

function activeProfileParam() {
  const sel = $("#profile-select").value;
  return sel ? `?profile=${encodeURIComponent(sel)}` : "";
}

// apiURL joins a path with the active-profile query and any extra params.
function apiURL(path, extra = {}) {
  const q = new URLSearchParams();
  const sel = $("#profile-select").value;
  if (sel) q.set("profile", sel);
  for (const [k, v] of Object.entries(extra)) q.set(k, v);
  const s = q.toString();
  return s ? `${path}?${s}` : path;
}

let POLL_TIMER = null;
function startPolling() {
  const tick = async () => {
    if (!document.hidden) await pollNow();
    POLL_TIMER = setTimeout(tick, 3000);
  };
  POLL_TIMER = setTimeout(tick, 3000);
}

async function pollNow() {
  try {
    if (CURRENT_PANEL === "status") renderStatus(await api("/api/status" + activeProfileParam()));
    else if (CURRENT_PANEL === "players") renderPlayers(await api("/api/players" + activeProfileParam()));
    else if (CURRENT_PANEL === "bans") renderBans(await api("/api/bans" + activeProfileParam()));
    else if (CURRENT_PANEL === "logs") await loadLogs(false);
    else if (CURRENT_PANEL === "audit") renderAudit(await api("/api/audit" + activeProfileParam()));
  } catch (e) {
    const box = CURRENT_PANEL === "status" ? $("#status-body")
      : CURRENT_PANEL === "players" ? $("#players-body")
      : CURRENT_PANEL === "bans" ? $("#bans-body")
      : CURRENT_PANEL === "audit" ? $("#audit-body") : null;
    if (box) box.innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function bytes(n) {
  const u = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0; while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

function renderStatus(d) {
  const box = $("#status-body");
  box.innerHTML = "";
  const line = (k, v) => box.append(el("div", { className: "kv" }, el("span", { className: "k", textContent: k }), el("span", { textContent: v })));
  line("Profile", `${d.profile} (${d.kind})`);
  line("Server", d.running ? `running — pid ${d.pid}${d.started_at ? " since " + d.started_at : ""}` : "stopped");
  if (d.proc_note) line("", d.proc_note);
  line("Port", d.world.port);
  if (d.world.name) line("Name", d.world.name);
  line("Players", d.players_note ? d.players_note : d.players);
  if (d.disk) line("Disk", `${bytes(d.disk.free_bytes)} free of ${bytes(d.disk.total_bytes)} (${d.disk.used_pct}% used)`);
  if (d.region) line("REGION served", `${d.region.requests_served} requests, ${d.region.records_emitted} records`);
}

function renderPlayers(d) {
  const box = $("#players-body");
  box.innerHTML = "";
  if (d.note) { box.append(el("p", { className: "muted", textContent: d.note })); return; }
  if (!d.players || d.players.length === 0) { box.append(el("p", { className: "muted", textContent: "No players connected." })); return; }
  const t = el("table", { className: "grid-table" });
  t.append(el("tr", {}, el("th", { textContent: "Name" }), el("th", { textContent: "Type" }),
    el("th", { textContent: "IP" }), el("th", { textContent: "Position" }), el("th", { textContent: "Level" }),
    el("th", { textContent: "Actions" })));
  for (const p of d.players) {
    const name = el("td", {}, document.createTextNode(p.name));
    if (p.name_looks_like_ip) name.append(el("span", { className: "warn-tag", textContent: " IP-shaped" }));

    const act = el("td", { className: "row-actions" });
    const kick = el("button", { textContent: "Kick" });
    kick.addEventListener("click", () => {
      const reason = prompt(`Kick "${p.name}" — reason:`, "kicked by the operator");
      if (reason === null) return;
      doPlayerAction({ verb: "kick", name: p.name, reason });
    });
    const op = el("button", { textContent: "Set level" });
    op.addEventListener("click", () => {
      const lvl = prompt(`Operator level for "${p.name}" (0–2):`, String(p.level));
      if (lvl === null) return;
      const n = Number(lvl);
      if (n === 0) doPlayerAction({ verb: "deop", name: p.name }, `Clear operator level for "${p.name}"?`);
      else doPlayerAction({ verb: "op", name: p.name, level: n }, `Set "${p.name}" to level ${n}?`);
    });
    const ban = el("button", { className: "danger", textContent: "Ban" });
    ban.addEventListener("click", () =>
      doPlayerAction({ verb: "ban", name: p.name }, `Ban "${p.name}" and disconnect them?`));
    act.append(kick, op, ban);

    t.append(el("tr", {}, name, el("td", { textContent: "T" + p.type }), el("td", { textContent: p.ip }),
      el("td", { textContent: `${p.x}, ${p.y}, ${p.z}` }), el("td", { textContent: p.level }), act));
  }
  box.append(t);
}

// --- worlds (stage 6.4) -----------------------------------------------------

async function loadWorlds() {
  const box = $("#worlds-body");
  try {
    renderWorlds(await api("/api/worlds" + activeProfileParam()));
  } catch (e) {
    box.innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function renderWorlds(d) {
  const box = $("#worlds-body");
  box.innerHTML = "";
  if (d.root) box.append(el("p", { className: "muted", textContent: "world_root: " + d.root }));
  if (d.note) box.append(el("p", { className: "muted", textContent: d.note }));
  if (!d.worlds || d.worlds.length === 0) return;
  const t = el("table", { className: "grid-table" });
  t.append(el("tr", {}, el("th", { textContent: "World" }), el("th", { textContent: "Blocks" }),
    el("th", { textContent: "" }), el("th", { textContent: "Actions" })));
  for (const wld of d.worlds) {
    const flags = el("td", {});
    if (wld.active) flags.append(el("span", { className: "warn-tag", textContent: "active" }));
    const act = el("td", { className: "row-actions" });
    const dl = el("button", { textContent: "Download" });
    dl.addEventListener("click", () => downloadBundle(wld.name));
    act.append(dl);
    if (!wld.active) {
      const mk = el("button", { textContent: "Make active" });
      mk.addEventListener("click", () => activateWorld(wld.name, d.kind));
      act.append(mk);
    }
    t.append(el("tr", {},
      el("td", { textContent: wld.name }),
      el("td", { textContent: wld.blocks < 0 ? "?" : wld.blocks.toLocaleString() }),
      flags, act));
  }
  box.append(t);
}

async function downloadBundle(world) {
  setMsg("#worlds-msg", { ok: true, message: `packing ${world}…` });
  try {
    const res = await fetch(apiURL("/api/worlds/bundle", { world }),
      { headers: { "X-Edenadmin-Token": TOKEN } });
    if (!res.ok) {
      let msg = `HTTP ${res.status}`;
      try { const j = await res.json(); msg = j.error || msg; } catch {}
      throw new Error(msg);
    }
    const blob = await res.blob();
    const cd = res.headers.get("Content-Disposition") || "";
    const m = cd.match(/filename="([^"]+)"/);
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = m ? m[1] : world + ".tar";
    a.click();
    URL.revokeObjectURL(a.href);
    setMsg("#worlds-msg", { ok: true, message: `downloaded ${a.download}` });
  } catch (e) {
    setMsg("#worlds-msg", { ok: false, message: e.message });
  }
}

async function activateWorld(world, kind) {
  const target = kind === "vps"
    ? "rewrites EDEN_WORLD_DIR in /etc/edenserver.conf"
    : "rewrites the profile's --world flag";
  const restart = confirm(`Make "${world}" the active world?\n\nThis ${target}.\nOK also restarts the server now; Cancel applies on the next restart.`);
  try {
    const d = await api("/api/worlds/activate" + activeProfileParam(), { world, restart });
    let msg = `active world is now ${world}`;
    if (d.restarted) msg += " (server restarted)";
    else if (restart && d.restart_message) msg += " — restart: " + d.restart_message;
    setMsg("#worlds-msg", { ok: true, message: msg });
    loadWorlds();
    pollNow();
  } catch (e) {
    setMsg("#worlds-msg", { ok: false, message: e.message });
  }
}

async function uploadBundle() {
  const name = $("#up-name").value.trim();
  const file = $("#up-file").files[0];
  if (!name || !file) { setMsg("#worlds-msg", { ok: false, message: "need a world name and a .tar file" }); return; }
  if (!confirm(`Overwrite worlds/${name}/ with the contents of ${file.name}? The server must be stopped.`)) return;
  const fd = new FormData();
  fd.append("bundle", file);
  try {
    const res = await fetch(apiURL("/api/worlds/bundle", { world: name }),
      { method: "POST", headers: { "X-Edenadmin-Token": TOKEN }, body: fd });
    const env = await res.json();
    if (!env.ok) throw new Error(env.error || `HTTP ${res.status}`);
    setMsg("#worlds-msg", { ok: true, message: `uploaded ${env.data.bytes.toLocaleString()} bytes into ${name}` });
    loadWorlds();
  } catch (e) {
    setMsg("#worlds-msg", { ok: false, message: e.message });
  }
}

function importFormData() {
  const file = $("#imp-file").files[0];
  const fd = new FormData();
  if (file) fd.append("eden", file);
  fd.append("name", $("#imp-name").value.trim());
  fd.append("air_fill", $("#imp-airfill").value);
  fd.append("spawn", $("#imp-spawn").value.trim());
  fd.append("force", $("#imp-force").checked ? "true" : "");
  fd.append("strict", $("#imp-strict").checked ? "true" : "");
  fd.append("no_signs", $("#imp-nosigns").checked ? "true" : "");
  fd.append("set_active", $("#imp-active").checked ? "true" : "");
  fd.append("restart", $("#imp-restart").checked ? "true" : "");
  const mc = $("#imp-maxcells").value.trim();
  const mr = $("#imp-maxregion").value.trim();
  if (mc !== "") fd.append("max_world_cells", mc);
  if (mr !== "") fd.append("max_region_records", mr);
  return { fd, file };
}

async function runImport(mode) {
  const { fd, file } = importFormData();
  if (!file) { setMsg("#worlds-msg", { ok: false, message: "choose a .eden file" }); return; }
  if (!fd.get("name")) { setMsg("#worlds-msg", { ok: false, message: "give the world a name" }); return; }
  if (mode === "write" && !confirm(`Import into worlds/${fd.get("name")}/ now?`)) return;

  const btns = [$("#imp-project"), $("#imp-write")];
  btns.forEach(b => b.disabled = true);
  $("#imp-summary").innerHTML = `<p class="muted">${mode === "write" ? "importing" : "projecting"}…</p>`;
  try {
    const res = await fetch(apiURL("/api/worlds/import", { mode }),
      { method: "POST", headers: { "X-Edenadmin-Token": TOKEN }, body: fd });
    const env = await res.json();
    if (!env.ok) throw new Error(env.error || `HTTP ${res.status}`);
    renderImportResult(env.data);
    $("#imp-project").disabled = false;
    // Enable Import only after a clean projection of the same file.
    $("#imp-write").disabled = !(env.data.mode === "project" && env.data.ok);
    if (env.data.mode === "write" && env.data.ok) {
      setMsg("#worlds-msg", { ok: true, message: "imported " + fd.get("name") });
      loadWorlds();
    }
  } catch (e) {
    $("#imp-summary").innerHTML = `<p class="probe bad">${e.message}</p>`;
    $("#imp-project").disabled = false;
  }
}

function renderImportResult(d) {
  const box = $("#imp-summary");
  box.innerHTML = "";
  if (d.error) box.append(el("p", { className: "probe bad", textContent: d.error }));
  const s = d.summary || {};
  if (s.stdout) box.append(el("pre", { className: "logbox", textContent: s.stdout }));
  for (const v of (s.verdicts || [])) {
    box.append(el("p", { className: "probe " + (v.level === "error" ? "bad" : "ok"),
      textContent: `${v.level}: ${v.text}` }));
  }
  if (d.command) box.append(el("code", { className: "cmd", textContent: d.command }));
  if (s.refused) box.append(el("p", { className: "bad-text",
    textContent: "eden_import refused. Adjust the strategy, or raise a cap above and project again." }));
  if (d.set_active) {
    const ok = d.set_active.ok !== false;
    box.append(el("p", { className: ok ? "muted" : "bad-text",
      textContent: "set active: " + (ok ? (d.set_active.world + (d.set_active.restarted ? " (restarted)" : "")) : d.set_active.error) }));
  }
}

// --- config (stage 6.6) -------------------------------------------------

let CONFIG_STATE = null;

async function loadConfig() {
  const box = $("#config-diff");
  try {
    CONFIG_STATE = await api("/api/config" + activeProfileParam());
    renderConfig(CONFIG_STATE);
  } catch (e) {
    box.innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function renderConfig(d) {
  $("#config-source").textContent = d.source ? "source: " + d.source : "";
  $("#config-note").textContent = d.note || "";
  $("#config-raw").textContent = d.raw || "(empty)";
  const editable = new Set(d.editable || []);
  const form = $("#config-form");
  const s = d.settings || {};
  for (const field of form.elements) {
    if (!field.name) continue;
    field.value = s[field.name] ?? "";
    field.disabled = !editable.has(field.name);
  }
  $("#config-diff").innerHTML = "";
  $("#config-msg").textContent = "";
}

async function saveConfig(e) {
  e.preventDefault();
  // Read fields directly (not via FormData) so a disabled/read-only field on a
  // local profile still round-trips its current value instead of an empty one.
  const g = n => e.target.elements[n].value;
  const body = {
    port: Number(g("port")),
    name: g("name") || "",
    world_dir: g("world_dir") || "",
    max_world_cells: Number(g("max_world_cells")),
    password: g("password") || "",
    extra_args: g("extra_args") || "",
    restart: $("#config-restart").checked,
  };
  const warn = body.restart
    ? "Save this config AND restart the server now? Every connected player is dropped."
    : "Save this config? It takes effect on the next restart.";
  if (!confirm(warn)) return;
  $("#config-msg").textContent = "saving…";
  try {
    const d = await api("/api/config" + activeProfileParam(), body);
    setMsg("#config-msg", d);
    renderConfigDiff(d.diff);
    loadConfig();
    pollNow();
  } catch (err) {
    setMsg("#config-msg", { ok: false, message: err.message });
  }
}

function renderConfigDiff(diff) {
  const box = $("#config-diff");
  box.innerHTML = "";
  if (!diff || diff.length === 0) { box.append(el("p", { className: "muted", textContent: "no change" })); return; }
  const pre = el("pre", { className: "logbox" });
  for (const l of diff) {
    pre.append(el("span", { className: "logline " + (l.startsWith("+") ? "tag-Audit" : "tag-Control"), textContent: l + "\n" }));
  }
  box.append(pre);
}

// --- backups (stage 6.5) --------------------------------------------------

async function loadBackups() {
  const box = $("#backups-body");
  try {
    renderBackups(await api("/api/backups" + activeProfileParam()));
  } catch (e) {
    box.innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function renderBackups(d) {
  const box = $("#backups-body");
  box.innerHTML = "";
  if (d.dir) box.append(el("p", { className: "muted", textContent: "backup_dir: " + d.dir }));
  if (d.note) box.append(el("p", { className: "muted", textContent: d.note }));
  if (!d.backups || d.backups.length === 0) return;
  const t = el("table", { className: "grid-table" });
  t.append(el("tr", {}, el("th", { textContent: "Backup" }), el("th", { textContent: "Size" }),
    el("th", { textContent: "" }), el("th", { textContent: "Actions" })));
  for (const b of d.backups) {
    const flags = el("td", {});
    if (b.pre_restore) flags.append(el("span", { className: "warn-tag", textContent: "pre-restore" }));
    else if (!b.is_stamp) flags.append(el("span", { className: "warn-tag", textContent: "odd name" }));
    const act = el("td", { className: "row-actions" });
    if (b.has_model) {
      const rb = el("button", { className: "danger", textContent: "Restore" });
      rb.addEventListener("click", () => restoreBackup(b.stamp));
      act.append(rb);
    }
    t.append(el("tr", {},
      el("td", { textContent: b.stamp }),
      el("td", { textContent: b.bytes < 0 ? "?" : bytes(b.bytes) }),
      flags, act));
  }
  box.append(t);
}

async function backupNow() {
  if (!confirm("Make a backup of the current world now?")) return;
  $("#backup-now").disabled = true;
  setMsg("#backups-msg", { ok: true, message: "backing up…" });
  try {
    const d = await api("/api/backups/create" + activeProfileParam(), {});
    setMsg("#backups-msg", d);
    loadBackups();
  } catch (e) {
    setMsg("#backups-msg", { ok: false, message: e.message });
  } finally {
    $("#backup-now").disabled = false;
  }
}

async function restoreBackup(stamp) {
  if (!confirm(`Restore backup ${stamp}?\n\nThis STOPS the server, overwrites the current world files, then starts it again. A -prerestore safety copy of the current world is kept.`)) return;
  if (!confirm(`Really overwrite the live world with ${stamp}? This cannot be undone except from the safety copy.`)) return;
  $("#restore-trace").innerHTML = `<p class="muted">restoring ${stamp}…</p>`;
  try {
    const d = await api("/api/backups/restore" + activeProfileParam(), { stamp });
    renderRestoreTrace(d);
    setMsg("#backups-msg", d);
    loadBackups();
    pollNow();
  } catch (e) {
    $("#restore-trace").innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function renderRestoreTrace(d) {
  const box = $("#restore-trace");
  box.innerHTML = "";
  for (const s of (d.steps || [])) box.append(el("p", { className: "probe ok", textContent: "• " + s }));
  box.append(el("p", { className: d.ok ? "muted" : "bad-text", textContent: (d.ok ? "✓ " : "✗ ") + d.message }));
  if (d.server_note) box.append(el("p", { className: "muted", textContent: d.server_note }));
  if (d.safety_copy) box.append(el("p", { className: "muted", textContent: "safety copy: " + d.safety_copy }));
}

let LOG_LINES = [];
async function loadLogs(fromStart) {
  const d = await api(`/api/logs${activeProfileParam() ? activeProfileParam() + "&" : "?"}from=${fromStart ? "start" : "tail"}`);
  if (fromStart || d.reset) LOG_LINES = [];
  if (d.missing) { $("#logs-body").innerHTML = `<span class="muted">${d.path} — no log yet (server not started under edenadmin).</span>`; return; }
  for (const l of (d.lines || [])) LOG_LINES.push(l);
  if (LOG_LINES.length > 2000) LOG_LINES = LOG_LINES.slice(-2000);
  renderLogs();
}

function renderLogs() {
  const filter = $("#log-filter").value.toLowerCase();
  const box = $("#logs-body");
  box.innerHTML = "";
  const rows = LOG_LINES.filter(l => !filter || l.text.toLowerCase().includes(filter));
  for (const l of rows) {
    box.append(el("span", { className: "logline" + (l.tag ? " tag-" + l.tag : ""), textContent: l.text + "\n" }));
  }
  box.scrollTop = box.scrollHeight;
}

// --- audit (stage 6.7) --------------------------------------------------

let AUDIT_EVENTS = [];

async function loadAudit() {
  try {
    renderAudit(await api("/api/audit" + activeProfileParam()));
  } catch (e) {
    $("#audit-body").innerHTML = `<p class="probe bad">${e.message}</p>`;
  }
}

function renderAudit(d) {
  AUDIT_EVENTS = d.events || [];
  const src = d.source ? "source: " + d.source + (d.fallback ? " — fallback, no --audit-file" : "") : "";
  $("#audit-source").textContent = src;
  $("#audit-note").textContent = d.note || "";
  drawAudit();
}

function drawAudit() {
  const filter = $("#audit-filter").value.toLowerCase();
  const box = $("#audit-body");
  box.innerHTML = "";
  const rows = AUDIT_EVENTS.filter(e => !filter || (e.raw || "").toLowerCase().includes(filter));
  if (rows.length === 0) {
    box.append(el("p", { className: "muted",
      textContent: AUDIT_EVENTS.length ? "no events match the filter." : "no audit events." }));
    return;
  }
  const t = el("table", { className: "grid-table" });
  t.append(el("tr", {}, el("th", { textContent: "Time (UTC)" }), el("th", { textContent: "Actor" }),
    el("th", { textContent: "Action" })));
  for (const e of rows) {
    if (e.time || e.actor) {
      t.append(el("tr", {}, el("td", { textContent: e.time || "" }),
        el("td", { textContent: e.actor || "" }), el("td", { textContent: e.what || "" })));
    } else {
      const td = el("td", { textContent: e.raw });
      td.colSpan = 3;
      t.append(el("tr", {}, td));
    }
  }
  box.append(t);
  box.scrollTop = box.scrollHeight;
}
