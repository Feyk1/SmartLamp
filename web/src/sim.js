(() => {
  "use strict";

  const { t } = window.LampI18n;

  const API = window.LampConfig
    ? window.LampConfig.orchestrator()
    : (window.LAMP_CONFIG && window.LAMP_CONFIG.orchestrator) ||
      "http://localhost:8080";

  const $ = (id) => document.getElementById(id);

  let token = sessionStorage.getItem("lamp_sim_token") || null;
  const urlPin = new URLSearchParams(window.location.search).get("pin");
  let instance = null;
  let ws = null;
  let reconnectDelay = 1000;
  let reconnectTimer = null;
  let refreshTimer = null;
  let modbusTimer = null;
  let events = [];
  let registerMeta = null;
  let lastHealth = null;
  let powerState = null;
  let networkPolicy = {
    networks: [],
    passwords: {},
    allow_any: false,
    failure: "none",
  };

  const FAULT_KEYS = {
    led_overtemp_warn: "sim.faults.overtempWarn",
    led_overtemp: "sim.faults.overtemp",
    overcurrent: "sim.faults.overcurrent",
    led_open: "sim.faults.ledOpen",
    config_corrupt: "sim.faults.configCorrupt",
    heartbeat_lost: "sim.faults.heartbeatLost",
    ws_drop: "sim.faults.wsDrop",
  };

  const CODE_EFFECT = {
    0: "clear",
    1: "derate",
    2: "trip",
    3: "trip",
    4: "trip",
    5: "safe_mode",
    6: "report",
    7: "off",
  };
  const CODE_SEVERITY = {
    0: "info",
    1: "warning",
    2: "critical",
    3: "critical",
    4: "critical",
    5: "critical",
    6: "warning",
    7: "warning",
  };

  let lastVersions = [];
  let lastValues = null;
  let connKey = "conn.connecting";
  let connClass = "";

  function setConn(key, cls = "") {
    connKey = key;
    connClass = cls;
    const el = $("sim-conn");
    el.textContent = t(key);
    el.className = cls ? `badge ${cls}` : "badge";
  }

  function setStatus(id, text) {
    const el = $(id);
    if (el) el.textContent = text;
  }

  function setFaultStatus(text) {
    const el = $("fault-status");
    if (!el) return;
    el.textContent = text;
    el.classList.remove("hidden");
  }

  function hhmmss(iso) {
    const date = iso ? new Date(iso) : new Date();
    if (Number.isNaN(date.getTime())) return "--:--:--";
    return date.toTimeString().slice(0, 8);
  }

  async function api(path, options = {}) {
    const response = await fetch(`${API}${path}`, options);
    if (!response.ok) {
      throw new Error(`${path} -> ${response.status}`);
    }
    return response;
  }

  function authHeaders() {
    return instance && instance.session_token
      ? { Authorization: `Bearer ${instance.session_token}` }
      : {};
  }

  async function simApi(path, options = {}) {
    if (!token) {
      showPin();
      throw new Error("sin sesión de simulación");
    }
    const headers = { Authorization: `Bearer ${token}`,
                      ...(options.headers || {}) };
    const response = await fetch(`${API}${path}`, { ...options, headers });
    if (response.status === 401 || response.status === 403) {
      sessionStorage.removeItem("lamp_sim_token");
      token = null;
      showPin(t("sim.pin.expired"));
      throw new Error("SIM_ONLY");
    }
    if (!response.ok) {
      throw new Error(`${path} -> ${response.status}`);
    }
    return response;
  }

  async function getInstance() {
    const stored = sessionStorage.getItem("lamp_instance");
    if (stored) {
      try {
        return JSON.parse(stored);
      } catch {
        sessionStorage.removeItem("lamp_instance");
      }
    }
    return createInstance();
  }

  async function createInstance() {
    const response = await api("/api/v1/instances", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ profile: "lamp-v1", ttl_s: 3600 }),
    });
    const data = await response.json();
    sessionStorage.setItem("lamp_instance", JSON.stringify(data));
    return data;
  }

  async function ensureInstance() {
    if (instance && instance.id) {
      try {
        const response = await fetch(
          `${API}/api/v1/instances/${instance.id}`,
          { headers: authHeaders() },
        );
        if (response.status === 404 || response.status === 401) {
          sessionStorage.removeItem("lamp_instance");
          instance = null;
        } else if (response.ok) {
          return instance;
        }
      } catch {
        /* backend down: will retry */
      }
    }
    instance = await getInstance();
    $("sim-instance").textContent = instance.id;
    return instance;
  }

  function showPin(message) {
    $("pin").classList.remove("hidden");
    $("pin-error").textContent = message || "";
    $("pin-input").focus();
  }

  function hidePin() {
    $("pin").classList.add("hidden");
    $("pin-error").textContent = "";
  }

  async function login(pin) {
    await ensureInstance();
    const response = await fetch(
      `${API}/api/v1/instances/${instance.id}/sim/session`,
      {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ pin }),
      },
    );
    if (!response.ok) {
      throw new Error(
        response.status === 403 ? t("sim.pin.wrong") : t("common.error"),
      );
    }
    const data = await response.json();
    token = data.sim_token;
    sessionStorage.setItem("lamp_sim_token", token);
    hidePin();
    await bootPanel();
  }

  function eventKey(event) {
    return `${event.ts}|${event.op}|${JSON.stringify(event.payload)}`;
  }

  function addEvent(message) {
    const event = {
      ts: message.ts || new Date().toISOString(),
      op: message.op || message.type || "event",
      payload: message.payload || {},
    };
    if (events.some((item) => eventKey(item) === eventKey(event))) return;
    events.push(event);
    if (events.length > 300) events.shift();
    renderTimeline();
    renderLogs();
    renderFaults();
  }

  function renderTimeline() {
    const list = $("d-timeline");
    list.textContent = "";
    events.slice(-40).forEach((event) => {
      const item = document.createElement("li");
      const ts = document.createElement("span");
      ts.className = "ts";
      ts.textContent = hhmmss(event.ts);
      item.appendChild(ts);
      item.appendChild(
        document.createTextNode(
          `${event.op} ${JSON.stringify(event.payload)}`,
        ),
      );
      list.appendChild(item);
    });
    list.scrollTop = list.scrollHeight;
  }

  function logLevel(op) {
    if (op.includes("nack") || op.includes("fault")) return "warn";
    if (op.includes("error") || op.includes("offline")) return "error";
    return "info";
  }

  function renderLogs() {
    const filter = $("log-filter").value.trim().toLowerCase();
    const list = $("log-list");
    list.textContent = "";
    events.forEach((event) => {
      const line = `${hhmmss(event.ts)} ${event.op} ${JSON.stringify(
        event.payload,
      )}`;
      if (filter && !line.toLowerCase().includes(filter)) return;
      const item = document.createElement("li");
      item.className = logLevel(event.op);
      const ts = document.createElement("span");
      ts.className = "ts";
      ts.textContent = hhmmss(event.ts);
      const level = document.createElement("span");
      level.className = "level";
      level.textContent = event.op;
      item.append(ts, level, ` ${JSON.stringify(event.payload)}`);
      list.appendChild(item);
    });
    list.scrollTop = list.scrollHeight;
  }

  function renderFaults() {
    renderFaultHistory();
  }

  function faultMetaForCode(code) {
    const entries = registerMeta ? Object.values(registerMeta.faults) : [];
    return (
      entries.find((meta) => meta.code === code) || {
        severity: CODE_SEVERITY[code],
        effect: CODE_EFFECT[code],
      }
    );
  }

  function renderFaultHistory() {
    const container = $("fault-history");
    if (!container) return;
    const rows = [];

    events.forEach((event) => {
      if (event.op === "sim_fault") {
        const type = event.payload.type;
        const meta = (registerMeta && registerMeta.faults[type]) || {};
        rows.push({
          ts: event.ts,
          source: "sim",
          fault: t(FAULT_KEYS[type] || "sim.faults.unknown"),
          code: event.payload.code ?? "—",
          severity: meta.severity || "warning",
          effect: meta.effect || (type === "ws_drop" ? "network" : "report"),
        });
      } else if (event.op === "fault") {
        const code = Number(event.payload.code) || 0;
        const meta = faultMetaForCode(code);
        rows.push({
          ts: event.ts,
          source: "firmware",
          fault: code
            ? t(`fault.code.${code}`)
            : t("fault.cleared"),
          code: code || "—",
          severity: code ? meta.severity || "warning" : "info",
          effect: code ? meta.effect || "report" : "clear",
        });
      } else if (event.op === "sim_faults_cleared") {
        rows.push({
          ts: event.ts,
          source: "user",
          fault: t("fault.cleared"),
          code: "—",
          severity: "info",
          effect: "clear",
        });
      }
    });

    container.textContent = "";
    if (rows.length === 0) {
      const empty = document.createElement("p");
      empty.className = "muted";
      empty.textContent = t("sim.faults.historyEmpty");
      container.appendChild(empty);
      return;
    }

    const table = document.createElement("table");
    const head = document.createElement("thead");
    const headRow = document.createElement("tr");
    [
      "sim.faults.col.time",
      "sim.faults.col.source",
      "sim.faults.col.fault",
      "sim.faults.col.code",
      "sim.faults.col.severity",
      "sim.faults.col.effect",
    ].forEach((key) => {
      const th = document.createElement("th");
      th.textContent = t(key);
      headRow.appendChild(th);
    });
    head.appendChild(headRow);
    table.appendChild(head);

    const body = document.createElement("tbody");
    rows
      .reverse()
      .slice(0, 30)
      .forEach((row) => {
        const tr = document.createElement("tr");
        const time = document.createElement("td");
        time.dataset.label = t("sim.faults.col.time");
        time.textContent = hhmmss(row.ts);
        const source = document.createElement("td");
        source.dataset.label = t("sim.faults.col.source");
        source.textContent = t(`sim.faults.source.${row.source}`);
        const fault = document.createElement("td");
        fault.dataset.label = t("sim.faults.col.fault");
        fault.textContent = row.fault;
        const code = document.createElement("td");
        code.className = "num";
        code.dataset.label = t("sim.faults.col.code");
        code.textContent = row.code;
        const severity = document.createElement("td");
        severity.dataset.label = t("sim.faults.col.severity");
        severity.textContent = t(`sim.faults.severity.${row.severity}`);
        const effect = document.createElement("td");
        effect.dataset.label = t("sim.faults.col.effect");
        effect.textContent = t(`sim.faults.effect.${row.effect}`);
        tr.append(time, source, fault, code, severity, effect);
        body.appendChild(tr);
      });
    table.appendChild(body);
    container.appendChild(table);
  }

  function renderDevice() {
    const state = instance && instance.lastState ? instance.lastState : null;
    if (state) {
      const color = state.color || {};
      $("d-state").textContent = t("sim.device.state", {
        power: state.power ? t("state.on") : t("state.off"),
        h: color.h ?? 0,
        s: color.s ?? 0,
        v: color.v ?? 0,
        b: state.brightness ?? 0,
      });
    } else {
      $("d-state").textContent = t("sim.device.noState");
    }

    if (lastHealth) {
      const mem = lastHealth.mem || {};
      const parts = [
        `serial ${lastHealth.serial ?? "?"}`,
        `slot ${lastHealth.slot ?? "?"}`,
        `fw ${lastHealth.fw_version ?? "?"}`,
        `uptime ${lastHealth.uptime_s ?? "?"} s`,
        `heap ${mem.free_heap ? Math.round(mem.free_heap / 1024) : "?"} kB`,
        `temp ${lastHealth.led_temp_c ?? "?"} °C`,
      ];
      $("d-health").textContent = parts.join(" · ");
      const tasks = lastHealth.tasks || {};
      $("d-metrics").textContent = Object.entries(tasks)
        .map(([name, hwm]) => `${name.padEnd(10)} ${hwm} B`)
        .join("\n");
      renderFirmwareCurrent();
    }
  }

  async function refreshDevice() {
    try {
      const response = await api(`/api/v1/instances/${instance.id}`, {
        headers: authHeaders(),
      });
      const data = await response.json();
      instance.lastState = data.state;
      lastHealth = data.health;
      if (typeof data.device_online === "boolean") {
        setConn(
          data.device_online ? "sim.conn.connected" : "sim.conn.offline",
          data.device_online ? "online" : "offline",
        );
      }
      renderDevice();
    } catch {
      setStatus("d-health", t("sim.device.backendDown"));
    }
  }

  function versionTuple(value) {
    return String(value || "0.0.0")
      .split(".")
      .map((part) => Number(part) || 0);
  }

  function compareVersions(a, b) {
    const left = versionTuple(a);
    const right = versionTuple(b);
    for (let i = 0; i < 3; i += 1) {
      if ((left[i] || 0) !== (right[i] || 0)) {
        return (left[i] || 0) > (right[i] || 0) ? 1 : -1;
      }
    }
    return 0;
  }

  function currentFwVersion() {
    return lastHealth ? lastHealth.fw_version || null : null;
  }

  function renderFirmwareCurrent() {
    const label = $("fw-current");
    const hint = $("fw-mode-hint");
    if (label) {
      const version = currentFwVersion();
      label.textContent = version
        ? t("sim.firmware.currentState", {
            version,
            slot: (lastHealth && lastHealth.slot) || "?",
          })
        : t("sim.firmware.unknown");
    }
    if (hint) hint.textContent = t(`sim.firmware.mode.${$("fw-mode").value}`);
  }

  function renderVersions(versions) {
    lastVersions = versions;
    renderFirmwareCurrent();
    const container = $("fw-versions");
    container.textContent = "";
    if (!versions.length) {
      container.textContent = t("sim.firmware.none");
      return;
    }
    const current = currentFwVersion();
    const mode = $("fw-mode").value;
    if (
      mode === "legit" &&
      current &&
      versions.every((item) => compareVersions(item.version, current) <= 0)
    ) {
      $("fw-mode-hint").textContent = t("sim.firmware.noNewer");
    }
    const table = document.createElement("table");
    table.innerHTML = `<thead><tr><th>${t("sim.table.version")}</th><th>${t(
      "sim.table.size",
    )}</th><th>${t("sim.table.status")}</th><th></th></tr></thead>`;
    const body = document.createElement("tbody");
    versions.forEach((item) => {
      const cmp = current ? compareVersions(item.version, current) : 1;
      const row = document.createElement("tr");
      const version = document.createElement("td");
      version.textContent = item.version;
      const size = document.createElement("td");
      size.className = "num";
      size.textContent = `${Math.round(item.size / 1024)} kB`;
      const status = document.createElement("td");
      const badge = document.createElement("span");
      badge.className = "badge";
      const key =
        cmp === 0 ? "current" : cmp > 0 ? "newer" : "older";
      badge.textContent = t(`sim.firmware.${key}`);
      badge.dataset.tone =
        cmp === 0 ? "success" : cmp > 0 ? "info" : "muted";
      status.appendChild(badge);
      const action = document.createElement("td");
      const button = document.createElement("button");
      button.className = "secondary small";
      button.textContent = t("sim.table.deploy");
      const blocked =
        (mode === "legit" && cmp <= 0) ||
        (mode === "downgrade" && cmp >= 0);
      button.disabled = blocked;
      button.title = blocked
        ? t("sim.firmware.notApplicable")
        : t("sim.firmware.deployHint", { mode });
      button.addEventListener("click", () => deployOta(item.version));
      action.appendChild(button);
      row.append(version, size, status, action);
      body.appendChild(row);
    });
    table.appendChild(body);
    container.appendChild(table);
  }

  async function loadVersions() {
    try {
      const response = await api("/api/v1/ota/versions");
      const data = await response.json();
      renderVersions(data.versions || []);
    } catch {
      $("fw-versions").textContent = t("sim.firmware.listError");
    }
  }

  async function deployOta(version) {
    const mode = $("fw-mode").value;
    const before = {
      version: currentFwVersion(),
      slot: lastHealth ? lastHealth.slot : null,
    };
    setStatus(
      "fw-status",
      t("sim.firmware.waiting", { version, mode }),
    );
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/ota`,
        {
          method: "POST",
          headers: { ...authHeaders(), "Content-Type": "application/json" },
          body: JSON.stringify({ version, mode }),
        },
      );
      const data = await response.json().catch(() => ({}));
      if (!response.ok) {
        setStatus(
          "fw-status",
          t("sim.firmware.error", {
            code: data.error?.code || response.status,
            message: data.error?.message || "",
          }),
        );
        return;
      }
      if (data.type === "nack") {
        setStatus(
          "fw-status",
          t("sim.firmware.rejected", {
            code: data.payload?.code || data.type,
          }),
        );
        return;
      }
      setStatus("fw-status", t("sim.firmware.sent", { version, mode }));
      for (let i = 0; i < 18; i += 1) {
        await new Promise((resolve) => setTimeout(resolve, 1000));
        await refreshDevice();
        const afterVersion = currentFwVersion();
        const afterSlot = lastHealth ? lastHealth.slot : null;
        if (
          afterVersion &&
          (afterVersion !== before.version || afterSlot !== before.slot)
        ) {
          setStatus(
            "fw-status",
            t("sim.firmware.updated", {
              version: afterVersion,
              slot: afterSlot || "?",
            }),
          );
          loadVersions().catch(() => {});
          return;
        }
      }
      setStatus(
        "fw-status",
        t("sim.firmware.unchanged", {
          version: before.version || "?",
          slot: before.slot || "?",
        }),
      );
    } catch {
      setStatus("fw-status", t("sim.firmware.backendDown"));
    }
  }

  function renderRegisterTables(values) {
    const container = $("mb-tables");
    container.textContent = "";
    Object.entries(registerMeta.registers).forEach(([kind, rows]) => {
      const table = document.createElement("table");
      const head = document.createElement("thead");
      head.innerHTML = `<tr><th colspan="3">${kind}</th></tr>
        <tr><th>${t("sim.table.addr")}</th><th>${t(
          "sim.table.name",
        )}</th><th>${t("sim.table.value")}</th></tr>`;
      table.appendChild(head);
      const body = document.createElement("tbody");
      rows.forEach((row) => {
        const tr = document.createElement("tr");
        const addr = document.createElement("td");
        addr.textContent = row.addr;
        const name = document.createElement("td");
        name.textContent = row.name;
        const value = document.createElement("td");
        value.className = "num";
        value.textContent = values[kind]?.[row.addr] ?? "—";
        tr.append(addr, name, value);
        body.appendChild(tr);
      });
      table.appendChild(body);
      container.appendChild(table);
    });
  }

  async function loadRegisterMeta() {
    if (registerMeta) return;
    const response = await simApi(
      `/api/v1/instances/${instance.id}/sim/registers`,
    );
    registerMeta = await response.json();
    renderFaultCatalog();
  }

  async function readRegisters() {
    await loadRegisterMeta();
    const response = await simApi(
      `/api/v1/instances/${instance.id}/sim/modbus/snapshot`,
    );
    const values = await response.json();
    lastValues = values;
    renderRegisterTables(values);
    setStatus("mb-status", t("sim.modbus.readAt", { time: hhmmss() }));
  }

  function renderFaultCatalog() {
    const container = $("fault-list");
    container.textContent = "";
    Object.entries(registerMeta.faults).forEach(([type, meta]) => {
      const card = document.createElement("div");
      card.className = "fault-card";
      const title = document.createElement("strong");
      title.textContent = t(FAULT_KEYS[type] || type);
      const code = document.createElement("span");
      const effect = meta.effect
        ? ` · ${t(`sim.faults.effect.${meta.effect}`)}`
        : "";
      code.textContent = (meta.code
        ? t("sim.faults.code", { code: meta.code })
        : t("sim.faults.noCode")) + effect;
      const button = document.createElement("button");
      button.className = "secondary small";
      button.textContent = t("sim.faults.inject");
      button.addEventListener("click", () => injectFault(type));
      card.append(title, code, button);
      container.appendChild(card);
    });
  }

  async function injectFault(type) {
    const duration = Number($("fault-duration").value) || 0;
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/sim/fault`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ type, duration_s: duration }),
        },
      );
      const data = await response.json();
      setFaultStatus(
        t("sim.faults.injected", {
          type: t(FAULT_KEYS[data.type] || data.type),
          code: data.code ? ` (code ${data.code})` : "",
        }),
      );
    } catch (error) {
      setFaultStatus(
        t("sim.faults.notApplied", { error: error.message }),
      );
    }
  }

  async function clearFaults() {
    try {
      await simApi(`/api/v1/instances/${instance.id}/sim/faults/clear`, {
        method: "POST",
      });
      setFaultStatus(t("sim.faults.cleared"));
    } catch (error) {
      setFaultStatus(
        t("sim.faults.clearError", { error: error.message }),
      );
    }
  }

  function clearLocalFaultEvents() {
    events = events.filter(
      (event) =>
        !["fault", "sim_fault", "sim_faults_cleared"].includes(event.op),
    );
    renderTimeline();
    renderLogs();
    renderFaults();
  }

  async function clearHistory() {
    if (!window.confirm(t("sim.faults.confirmClearHistory"))) return;
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/faults/history`,
        { method: "DELETE" },
      );
      const data = await response.json().catch(() => ({}));
      clearLocalFaultEvents();
      setFaultStatus(
        t("sim.faults.historyCleared", { removed: data.removed ?? 0 }),
      );
    } catch (error) {
      setFaultStatus(
        t("sim.faults.clearError", { error: error.message }),
      );
    }
  }

  function renderPower() {
    const state = powerState || "—";
    setStatus("p-status", t("sim.supervisor", { state }));
    const running = state === "APP_RUNNING" || state === "BOOTING";
    $("p-on").disabled = running || state === "BOOTLOADER_MODE";
    $("p-off").disabled = state === "POWERED_OFF";
    $("p-reset").disabled = state === "POWERED_OFF";
    $("p-boot").textContent =
      state === "BOOTLOADER_MODE"
        ? t("sim.power.exitBootloader")
        : t("sim.power.enterBootloader");
  }

  async function refreshNetwork() {
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/provisioning`,
      );
      const data = await response.json();
      setStatus(
        "net-state",
        t(`sim.network.${data.state}`) || data.state,
      );
      let detail = data.ssid
        ? t("sim.network.detail", { ssid: data.ssid, ip: data.ip })
        : t("sim.network.none");
      if (data.cycles > 0 || data.state === "provisioned") {
        detail += ` · ${t("sim.network.cycles", {
          count: data.cycles || 0,
          required: data.cycles_required || 3,
        })}`;
      }
      setStatus("net-detail", detail);
      $("net-cancel").disabled = data.state !== "pairing";
    } catch {
      setStatus("net-state", "—");
    }
  }

  async function provisioningAction(path) {
    await simApi(path, { method: "POST" });
    await refreshNetwork();
  }

  function renderPolicy() {
    const list = $("net-list");
    list.textContent = "";
    networkPolicy.passwords = networkPolicy.passwords || {};
    if (!networkPolicy.networks.length) {
      const empty = document.createElement("span");
      empty.className = "muted";
      empty.textContent = t("sim.network.empty");
      list.appendChild(empty);
    } else {
      networkPolicy.networks.forEach((ssid) => {
        const chip = document.createElement("span");
        chip.className = "network-chip";
        const label = document.createElement("span");
        label.textContent = ssid;
        const pass = document.createElement("span");
        pass.className = "network-pass";
        pass.textContent = networkPolicy.passwords[ssid]
          ? `· ${networkPolicy.passwords[ssid]}`
          : `· ${t("sim.network.anyPassword")}`;
        const remove = document.createElement("button");
        remove.type = "button";
        remove.className = "chip-remove";
        remove.setAttribute("aria-label", t("sim.network.remove", { ssid }));
        remove.textContent = "×";
        remove.addEventListener("click", () => {
          networkPolicy.networks = networkPolicy.networks.filter(
            (value) => value !== ssid,
          );
          delete networkPolicy.passwords[ssid];
          savePolicy().catch((error) => {
            setStatus("net-policy-status", error.message);
          });
        });
        chip.append(label, pass, remove);
        list.appendChild(chip);
      });
    }
    $("net-any").checked = networkPolicy.allow_any;
    $("net-failure").value = networkPolicy.failure;
  }

  async function refreshPolicy() {
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/provisioning/policy`,
      );
      networkPolicy = await response.json();
      renderPolicy();
    } catch (error) {
      setStatus("net-policy-status", error.message);
    }
  }

  async function savePolicy() {
    const response = await simApi(
      `/api/v1/instances/${instance.id}/provisioning/policy`,
      {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(networkPolicy),
      },
    );
    networkPolicy = await response.json();
    renderPolicy();
    setStatus("net-policy-status", t("sim.network.saved"));
  }

  async function refreshPower() {
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/sim/power`,
      );
      const data = await response.json();
      powerState = data.state;
    } catch {
      powerState = "SUPERVISOR_ERROR";
    }
    renderPower();
  }

  async function supervisorPower(action) {
    if (
      action === "factory_reset" &&
      !window.confirm(t("sim.confirm.factory"))
    ) {
      return;
    }
    setStatus("p-status", t("sim.supervisor.action", { action }));
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/sim/power`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            action,
            confirm: action === "factory_reset",
          }),
        },
      );
      const data = await response.json();
      powerState = data.state;
      renderPower();
    } catch (error) {
      setStatus("p-status", `supervisor: ${error.message}`);
    }
  }

  async function supervisorBootloader() {
    const action = powerState === "BOOTLOADER_MODE" ? "exit" : "enter";
    setStatus(
      "p-status",
      t("sim.supervisor.action", { action: `bootloader ${action}` }),
    );
    try {
      const response = await simApi(
        `/api/v1/instances/${instance.id}/sim/bootloader`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ action }),
        },
      );
      const data = await response.json();
      powerState = data.state;
      renderPower();
    } catch (error) {
      setStatus("p-status", `supervisor: ${error.message}`);
    }
  }

  async function connect() {
    if (!instance) return;
    if (ws) {
      ws.onclose = null;
      ws.close();
    }
    let ticket;
    try {
      ticket = await window.LampConfig.wsTicket(instance, authHeaders());
    } catch {
      setConn("sim.conn.offline", "offline");
      try {
        await ensureInstance();
      } catch {
        /* retried by the timer */
      }
      clearTimeout(reconnectTimer);
      reconnectTimer = setTimeout(() => {
        reconnectDelay = Math.min(reconnectDelay * 2, 10000);
        connect();
      }, reconnectDelay);
      return;
    }
    ws = new WebSocket(window.LampConfig.wsUrl(instance, ticket));
    ws.onopen = () => {
      reconnectDelay = 1000;
      ws.send(
        JSON.stringify({
          type: "subscribe",
          channels: ["state", "events", "metrics"],
        }),
      );
      refreshDevice();
    };
    ws.onmessage = (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      if (message.type === "state") {
        instance.lastState = message.payload;
        renderDevice();
      } else if (message.type === "health") {
        lastHealth = message.payload;
        renderDevice();
      } else if (message.type === "event") {
        if (message.op === "fault_history_cleared") {
          clearLocalFaultEvents();
        } else if (
          message.op === "sim_power" &&
          message.payload?.action === "factory_reset"
        ) {
          clearLocalFaultEvents();
          refreshDevice().catch(() => {});
          loadVersions().catch(() => {});
          refreshPower().catch(() => {});
        }
        addEvent(message);
        if (message.op === "device_offline") setConn("sim.conn.offline", "offline");
        if (message.op === "device_online") setConn("sim.conn.connected", "online");
        if (
          message.op === "provisioning" &&
          document.querySelector("#tab-network.active")
        ) {
          refreshNetwork().catch(() => {});
        }
      } else if (message.type === "ack" || message.type === "nack") {
        setStatus(
          "fw-status",
          `${message.type} ${message.op || ""}`,
        );
      } else if (message.type === "ping" && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: "pong" }));
      }
    };
    ws.onclose = () => {
      setConn("conn.reconnecting", "offline");
      clearTimeout(reconnectTimer);
      reconnectTimer = setTimeout(() => {
        reconnectDelay = Math.min(reconnectDelay * 2, 10000);
        connect();
      }, reconnectDelay);
    };
  }

  function openTab(name) {
    document.querySelectorAll(".tab-btn").forEach((button) => {
      button.classList.toggle("active", button.dataset.tab === name);
    });
    document.querySelectorAll(".tab-panel").forEach((panel) => {
      panel.classList.toggle("active", panel.id === `tab-${name}`);
    });
    if (name === "firmware") loadVersions();
    if (name === "modbus") {
      readRegisters().catch((error) =>
        setStatus("mb-status", t("sim.logs.error", { error: error.message })),
      );
    }
    if (name === "logs") renderLogs();
    if (name === "network") {
      refreshNetwork().catch(() => {});
      refreshPolicy().catch(() => {});
    }
  }

  async function bootPanel() {
    await ensureInstance();
    $("sim-instance").textContent = instance.id;
    try {
      await Promise.all([
        refreshDevice(),
        loadVersions(),
        loadRegisterMeta(),
      ]);
      const response = await simApi(
        `/api/v1/instances/${instance.id}/sim/logs?limit=200`,
      );
      const data = await response.json();
      events = (data.logs || []).map((item) => ({
        ts: item.ts,
        op: item.op || item.type || "event",
        payload: item.payload || {},
      }));
      renderTimeline();
      renderLogs();
      renderFaults();
    } catch {
      /* the WS and the retries cover startup */
    }
    connect();
    clearInterval(refreshTimer);
    refreshTimer = setInterval(() => {
      refreshDevice();
      refreshPower();
      if (document.querySelector("#tab-network.active")) {
        refreshNetwork().catch(() => {});
      }
    }, 3000);
    refreshPower();
    refreshNetwork().catch(() => {});
    if (registerMeta) {
      readRegisters().catch(() => {});
    }
  }

  document.querySelectorAll(".tab-btn").forEach((button) => {
    button.addEventListener("click", () => openTab(button.dataset.tab));
  });

  $("pin-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      await login($("pin-input").value.trim());
    } catch (error) {
      $("pin-error").textContent = error.message;
    }
  });

  $("pin-demo").addEventListener("click", () => {
    login("2025").catch((error) => {
      $("pin-error").textContent = error.message;
    });
  });

  $("logout").addEventListener("click", () => {
    sessionStorage.removeItem("lamp_sim_token");
    token = null;
    clearInterval(refreshTimer);
    clearInterval(modbusTimer);
    if (ws) {
      ws.onclose = null;
      ws.close();
      ws = null;
    }
    showPin(t("sim.pin.closed"));
  });

  $("net-start").addEventListener("click", () => {
    provisioningAction(
      `/api/v1/instances/${instance.id}/provisioning/start`,
    ).catch((error) => setStatus("net-state", error.message));
  });
  $("net-reset").addEventListener("click", () => {
    if (!window.confirm(t("sim.network.resetConfirm"))) return;
    provisioningAction(
      `/api/v1/instances/${instance.id}/provisioning/reset`,
    ).catch((error) => setStatus("net-state", error.message));
  });
  $("net-cancel").addEventListener("click", () => {
    provisioningAction(
      `/api/v1/instances/${instance.id}/provisioning/cancel`,
    ).catch((error) => setStatus("net-state", error.message));
  });
  $("net-refresh").addEventListener("click", () => {
    refreshNetwork().catch((error) => setStatus("net-state", error.message));
    refreshPolicy().catch((error) => setStatus("net-policy-status", error.message));
  });
  $("net-add").addEventListener("click", () => {
    const value = $("net-new").value.trim();
    const password = $("net-pass").value.trim();
    if (!value) return;
    if (!networkPolicy.networks.includes(value)) {
      networkPolicy.networks.push(value);
    }
    networkPolicy.passwords[value] = password;
    $("net-new").value = "";
    $("net-pass").value = "";
    savePolicy().catch((error) => setStatus("net-policy-status", error.message));
  });
  $("net-new").addEventListener("keydown", (event) => {
    if (event.key !== "Enter") return;
    event.preventDefault();
    $("net-add").click();
  });
  $("net-any").addEventListener("change", () => {
    networkPolicy.allow_any = $("net-any").checked;
    savePolicy().catch((error) => setStatus("net-policy-status", error.message));
  });
  $("net-failure").addEventListener("change", () => {
    networkPolicy.failure = $("net-failure").value;
    savePolicy().catch((error) => setStatus("net-policy-status", error.message));
  });

  $("p-on").addEventListener("click", () => supervisorPower("on"));
  $("p-off").addEventListener("click", () => supervisorPower("off"));
  $("p-reset").addEventListener("click", () => supervisorPower("reset"));
  $("p-factory").addEventListener("click", () =>
    supervisorPower("factory_reset"),
  );
  $("p-boot").addEventListener("click", supervisorBootloader);
  $("fault-clear").addEventListener("click", clearFaults);
  $("history-clear").addEventListener("click", clearHistory);
  $("fw-mode").addEventListener("change", () => {
    renderFirmwareCurrent();
    if (lastVersions.length) renderVersions(lastVersions);
  });
  $("log-filter").addEventListener("input", renderLogs);
  $("mb-read").addEventListener("click", () =>
    readRegisters().catch((error) => setStatus("mb-status", error.message)),
  );
  $("mb-write").addEventListener("click", async () => {
    const payload = {
      kind: $("mb-kind").value,
      addr: Number($("mb-addr").value),
      value: Number($("mb-value").value),
      apply: $("mb-apply").checked,
    };
    try {
      await simApi(`/api/v1/instances/${instance.id}/sim/modbus/write`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
      await readRegisters();
      setStatus(
        "mb-status",
        t("sim.modbus.wrote", {
          kind: payload.kind,
          addr: payload.addr,
        }),
      );
    } catch (error) {
      setStatus("mb-status", t("sim.logs.error", { error: error.message }));
    }
  });
  $("mb-auto").addEventListener("change", () => {
    clearInterval(modbusTimer);
    if ($("mb-auto").checked) {
      modbusTimer = setInterval(() => {
        if (document.querySelector("#tab-modbus.active")) {
          readRegisters().catch(() => {});
        }
      }, 1000);
    }
  });

  $("log-download").addEventListener("click", () => {
    const text = events
      .map(
        (event) =>
          `${event.ts} ${event.op} ${JSON.stringify(event.payload)}`,
      )
      .join("\n");
    const blob = new Blob([`${text}\n`], { type: "text/plain" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `smartlamp-${instance ? instance.id : "log"}.log`;
    link.click();
    URL.revokeObjectURL(link.href);
  });

  window.addEventListener("lamp:lang", () => {
    document.title = t("sim.pageTitle");
    setConn(connKey, connClass);
    renderDevice();
    renderPower();
    renderTimeline();
    renderLogs();
    renderFaults();
    renderPolicy();
    if (registerMeta) renderFaultCatalog();
    if (lastVersions.length) renderVersions(lastVersions);
    if (lastValues) renderRegisterTables(lastValues);
  });

  document.title = t("sim.pageTitle");

  (async () => {
    try {
      await ensureInstance();
      $("sim-instance").textContent = instance.id;
    } catch {
      setConn("conn.backendDown", "offline");
      return;
    }
    if (token) {
      await bootPanel().catch(() => showPin(t("sim.pin.invalid")));
    } else if (urlPin) {
      /* Demo shortcut: sim.html?pin=2025 */
      await login(urlPin).catch(() => showPin(t("sim.pin.wrong")));
    } else {
      showPin();
    }
  })();
})();
