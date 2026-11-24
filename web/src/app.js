(() => {
  "use strict";

  const { t } = window.LampI18n;
  const API = window.LampConfig
    ? window.LampConfig.orchestrator()
    : (window.LAMP_CONFIG && window.LAMP_CONFIG.orchestrator) ||
      "http://localhost:8080";

  const $ = (id) => document.getElementById(id);
  const swatch = $("swatch");
  const staleNote = $("stale-note");
  const lampMeta = $("lamp-meta");
  const conn = $("conn");
  const power = $("power");
  const brightness = $("brightness");
  const cct = $("cct");
  const wheel = $("wheel");
  const stateText = $("state-text");
  const healthText = $("health");
  const last = $("last");

  const outputs = {
    brightness: $("brightness-out"),
    cct: $("cct-out"),
  };

  const PRESETS = [
    { h: 0, s: 1000 },
    { h: 28, s: 1000 },
    { h: 52, s: 1000 },
    { h: 120, s: 1000 },
    { h: 180, s: 1000 },
    { h: 210, s: 1000 },
    { h: 265, s: 1000 },
    { h: 305, s: 1000 },
  ];
  const POWER_ON_DEFAULT = {
    mode: "cct",
    cct: 3000,
    brightness: 700,
  };
  const SCENES = [
    { key: "scene.reading", cct: 4000, brightness: 800 },
    { key: "scene.relax", cct: 2700, brightness: 450 },
    { key: "scene.focus", cct: 5600, brightness: 1000 },
    { key: "scene.night", cct: 2200, brightness: 120 },
  ];
  const EFFECTS = [
    { id: 1, key: "effect.breathing" },
    { id: 2, key: "effect.rainbow" },
    { id: 4, key: "effect.candle" },
    { id: 3, key: "effect.strobe" },
    { id: 5, key: "effect.police" },
    { id: 6, key: "effect.sunrise" },
  ];
  const SEQUENCE_KEY = "lamp_sequence_v1";
  const SEQUENCE_SETTINGS_KEY = "lamp_sequence_settings_v1";
  const SEQUENCE_MAX = 8;
  let sequence = [];
  let sequenceSettings = { fade_ms: 800, speed: 1000 };
  const PARTY_SOURCE_KEY = "lamp_party_source";
  const CIRCADIAN_KEY = "lamp_circadian";
  const CIRCADIAN_CONFIG_KEY = "lamp_circadian_config_v1";
  const CIRCADIAN_OVERRIDE_KEY = "lamp_circadian_override_until";
  const DEFAULT_CIRCADIAN_POINTS = [
    [0, 2200, 80],
    [7, 3000, 350],
    [12, 5600, 900],
    [18, 4000, 600],
    [23, 2200, 80],
  ];
  let schedules = [];
  let editingScheduleId = null;
  let scheduleDraft = null;
  let scheduleAction = null;
  let activeEffect = null;
  let pairingPoll = null;
  let pairError = "";
  let pairPolicyPasswords = {};
  let circadianEnabled = false;
  let circadianTimer = null;
  let circadianOverrideUntil = 0;
  let circadianConfig = {
    points: DEFAULT_CIRCADIAN_POINTS.map((point) => [...point]),
    adjustBrightness: false,
    transition: "gradual",
    transitionSeconds: 5,
  };
  let circadianRamp = null;
  let circadianPointIndex = null;
  const party = {
    active: false,
    source: "mic",
    ctx: null,
    stream: null,
    analyser: null,
    data: null,
    timer: null,
    hue: 0,
    level: 0,
    baseline: 0.05,
    lastBeat: 0,
    ticks: 0,
    fallback: false,
    session: 0,
  };

  let ws = null;
  let instance = null;
  let reconnectDelay = 1000;
  let reconnectTimer = null;
  let commandSeq = 0;
  let uiMode = "white";
  let connKey = "conn.connecting";
  let connClass = "";
  let backendDown = false;
  const throttles = new Map();

  const state = {
    power: false,
    brightness: 0,
    h: 30,
    s: 800,
    v: 800,
    cct: 2700,
    mode: 0,
    fwVersion: null,
    stale: true,
    faultCode: 0,
  };

  const network = { state: "unknown", ssid: null, ip: null };
  let powerOn = { mode: "default", state: null };
  let lastHealth = null;

  function authHeaders() {
    return instance && instance.session_token
      ? { Authorization: `Bearer ${instance.session_token}` }
      : {};
  }

  function hsvToRgb(h, s, v) {
    const c = v * s;
    const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
    const m = v - c;
    let r = 0;
    let g = 0;
    let b = 0;

    if (h < 60) [r, g] = [c, x];
    else if (h < 120) [r, g] = [x, c];
    else if (h < 180) [g, b] = [c, x];
    else if (h < 240) [g, b] = [x, c];
    else if (h < 300) [r, b] = [x, c];
    else [r, b] = [c, x];

    return [
      Math.round((r + m) * 255),
      Math.round((g + m) * 255),
      Math.round((b + m) * 255),
    ];
  }

  // Tanner Helland approximation for 1000–40000 K; here 2000–6500 K.
  function kelvinToRgb(kelvin) {
    const temp = Math.min(40000, Math.max(1000, kelvin)) / 100;
    let r;
    let g;
    let b;
    if (temp <= 66) {
      r = 255;
      g = 99.4708025861 * Math.log(temp) - 161.1195681661;
      b = temp <= 19 ? 0 : 138.5177312231 * Math.log(temp - 10) - 305.0447927307;
    } else {
      r = 329.698727446 * Math.pow(temp - 60, -0.1332047592);
      g = 288.1221695283 * Math.pow(temp - 60, -0.0755148492);
      b = 255;
    }
    const clamp = (value) => Math.max(0, Math.min(255, Math.round(value)));
    return [clamp(r), clamp(g), clamp(b)];
  }

  function effectiveRgb() {
    if (!state.power) return [0, 0, 0];
    if (state.mode === 1) {
      const [r, g, b] = kelvinToRgb(state.cct);
      const level = state.brightness / 1000;
      return [r * level, g * level, b * level].map((value) => Math.round(value));
    }
    const level = (state.v / 1000) * (state.brightness / 1000);
    return hsvToRgb(state.h, state.s / 1000, Math.max(0, level));
  }

  function setConn(key, cls = "") {
    connKey = key;
    connClass = cls;
    conn.removeAttribute("data-i18n");
    conn.textContent = t(key);
    conn.className = cls ? `badge ${cls}` : "badge";
  }

  function setBackendDown(value) {
    backendDown = value;
    const alert = $("backend-alert");
    if (alert) alert.classList.toggle("hidden", !value);
    if (value) {
      setConn("conn.backendDown", "offline");
    } else {
      setDevicePresence(!state.stale);
    }
  }

  function setDevicePresence(online) {
    setStale(!online);
    if (!backendDown) {
      setConn(
        online ? "conn.deviceConnected" : "conn.deviceOffline",
        online ? "online" : "offline",
      );
    }
  }

  function setLast(text) {
    last.textContent = text;
  }

  function setStale(value) {
    if (state.stale === value) return;
    state.stale = value;
    if (value && party.active) stopParty(true);
    render();
  }

  function renderNetwork() {
    const badge = $("network");
    let key = "network.unknown";
    let cls = "";
    if (network.state === "online") {
      key = network.ssid ? "network.onlineSsid" : "network.online";
      cls = "online";
    } else if (network.state === "provisioned") {
      key = "network.provisioned";
      cls = "offline";
    } else if (network.state === "pairing") {
      key = "network.pairing";
    } else if (network.state === "connecting") {
      key = "network.connecting";
    }
    badge.removeAttribute("data-i18n");
    badge.textContent = t(key, { ssid: network.ssid || "" });
    badge.className = cls ? `badge ${cls}` : "badge";

    const note = $("network-note");
    let noteKey = "";
    if (network.state === "provisioned") {
      noteKey = "network.noteProvisioned";
    } else if (network.state === "pairing") {
      noteKey = "network.notePairing";
    } else if (network.state === "connecting") {
      noteKey = "network.noteConnecting";
    }
    note.textContent = noteKey ? t(noteKey) : "";
    note.classList.toggle("hidden", !noteKey);
  }

  function setNetwork(info) {
    if (!info) return;
    if (typeof info.state === "string") network.state = info.state;
    if ("ssid" in info) network.ssid = info.ssid;
    if ("ip" in info) network.ip = info.ip;
    renderNetwork();
    render();
  }

  function isFaultLatched() {
    return [2, 3, 4].includes(Number(state.faultCode));
  }

  function isSafeMode() {
    return Number(state.faultCode) === 5;
  }

  function isFaultLocked() {
    return [2, 3, 4, 5, 7].includes(Number(state.faultCode));
  }

  function renderFault() {
    const code = Number(state.faultCode) || 0;
    const banner = $("fault-banner");
    if (!banner) return;
    banner.classList.toggle("hidden", !code);
    if (!code) return;
    const error = [2, 3, 4, 5, 7].includes(code);
    banner.classList.toggle("error", error);
    banner.classList.toggle("warn", !error);
    $("fault-severity").textContent = "!";
    $("fault-title").textContent = t(`fault.code.${code}`);
    $("fault-detail").textContent = t(`fault.detail.${code}`);
    $("fault-latched").classList.toggle("hidden", !isFaultLatched());
    $("fault-safe").classList.toggle("hidden", !isSafeMode());
  }

  function isLocked() {
    return state.stale || network.state !== "online" || isFaultLocked();
  }

  async function clearFaults() {
    if (!instance || !instance.id) return;
    if (!window.confirm(t("fault.confirmClear"))) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/faults/clear`,
        { method: "POST", headers: authHeaders() },
      );
      if (!response.ok) throw new Error(`faults/clear -> ${response.status}`);
      state.faultCode = 0;
      renderFault();
      render();
      setLast(t("fault.cleared"));
    } catch {
      setLast(t("fault.clearError", { error: "backend" }));
    }
  }

  function render() {
    wheel.setAttribute("aria-valuemin", "0");
    wheel.setAttribute("aria-valuemax", "1000");
    wheel.setAttribute("aria-valuenow", String(state.s));
    wheel.setAttribute(
      "aria-valuetext",
      `H${Math.round(state.h)} S${state.s}`,
    );
    const [r, g, b] = effectiveRgb();
    swatch.style.background = `rgb(${r} ${g} ${b})`;
    swatch.style.boxShadow = state.power
      ? `0 0 80px rgba(${r}, ${g}, ${b}, 0.45)`
      : "none";
    swatch.classList.toggle("stale", state.stale);
    swatch.classList.toggle("off", !state.power);
    staleNote.classList.toggle("hidden", !state.stale);

    power.textContent = state.power ? t("power.off") : t("power.on");
    brightness.value = state.brightness;
    cct.value = state.cct;
    outputs.brightness.textContent = state.brightness;
    outputs.cct.textContent = `${state.cct} K`;

    const powerLabel = state.power ? t("state.on") : t("state.off");
    const detail =
      state.mode === 1
        ? t("state.detailCct", {
            power: powerLabel,
            kelvin: state.cct,
            b: state.brightness,
          })
        : t("state.detail", {
            power: powerLabel,
            h: Math.round(state.h),
            s: state.s,
            v: state.v,
            b: state.brightness,
          });
    stateText.textContent = state.stale
      ? `${t("state.lastKnown")}: ${detail}`
      : detail;
    lampMeta.textContent = state.stale
      ? `${t("state.lastKnown")} · ${detail}`
      : detail;

    healthText.classList.toggle("stale-value", state.stale);
    const locked = isLocked();
    if (locked) {
      ["schedules-dialog", "sequences-dialog"].forEach((id) => {
        const dialog = $(id);
        if (dialog && dialog.open) closeManageDialog(id);
      });
    }
    $("dashboard-lock").disabled = locked;
    $("soft-hint").classList.toggle("hidden", locked);
    $("wheel").classList.toggle("locked", locked);
    $("wheel").setAttribute("aria-disabled", String(locked));
    $("pair-device").disabled = state.stale;

    document.querySelectorAll(".mode-btn").forEach((button) => {
      const active = button.dataset.mode === uiMode;
      button.classList.toggle("active", active);
      button.setAttribute("aria-selected", String(active));
    });
    $("panel-white").classList.toggle("hidden", uiMode !== "white");
    $("panel-color").classList.toggle("hidden", uiMode !== "color");

    updateSequenceControls();
    updatePartyControls();
    drawWheel();
  }

  function applyHealth(payload) {
    if (!payload) return;
    lastHealth = payload;
    if (payload.fw_version) state.fwVersion = payload.fw_version;
    const device = $("settings-device");
    if (device) {
      const serial = payload.serial || "—";
      const hw = payload.hw_rev != null ? ` · hw ${payload.hw_rev}` : "";
      const fw = payload.fw_version || "—";
      device.textContent = `${t("settings.serial")}: ${serial}${hw} · fw ${fw}`;
    }
    const parts = [];
    if (payload.fw_version) parts.push(`fw ${payload.fw_version}`);
    if (payload.mem && payload.mem.free_heap != null) {
      parts.push(`heap ${Math.round(payload.mem.free_heap / 1024)} kB`);
    }
    if (payload.led_temp_c != null) {
      parts.push(`${Number(payload.led_temp_c).toFixed(1)} °C`);
    }
    healthText.textContent = parts.length ? parts.join(" · ") : "—";
  }

  function applyState(payload) {
    const color = payload.color || {};
    state.power = Boolean(payload.power);
    state.brightness = Number(payload.brightness) || 0;
    state.h = Number(color.h) || 0;
    state.s = Number(color.s) || 0;
    state.v = Number(color.v) || 0;
    state.cct = Number(payload.cct) || state.cct;
    state.mode = Number(payload.mode) || 0;
    if (state.mode !== 2) activeEffect = null;
    if (state.mode === 1) {
      uiMode = "white";
    } else if (state.mode === 0) {
      uiMode = "color";
    }
    render();
  }

  async function refreshState() {
    if (!instance || !instance.id) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/state`,
        { headers: authHeaders() },
      );
      if (!response.ok) return;
      const data = await response.json();
      if (data.state) applyState(data.state);
    } catch {
      /* retried on the next sync */
    }
  }

  async function refreshHealth() {
    if (!instance || !instance.id) return;
    try {
      const response = await fetch(`${API}/api/v1/instances/${instance.id}`, {
        headers: authHeaders(),
      });
      if (!response.ok) return;
      const data = await response.json();
      applyHealth(data.health);
      if (typeof data.device_online === "boolean") {
        setDevicePresence(data.device_online);
      }
    } catch {
      /* no backend: retried on the next cycle */
    }
  }

  async function refreshProvisioning() {
    if (!instance || !instance.id) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/provisioning`,
        { headers: authHeaders() },
      );
      if (!response.ok) return;
      setNetwork(await response.json());
    } catch {
      /* no backend: retried on the next cycle */
    }
  }

  function currentPowerOnState() {
    if (state.mode === 1) {
      return {
        mode: "cct",
        cct: state.cct,
        brightness: state.brightness || 800,
      };
    }
    return {
      mode: "color",
      h: state.h,
      s: state.s,
      v: 1000,
      brightness: state.brightness || 800,
    };
  }

  function powerOnEffective(stateValue) {
    const level = (stateValue.brightness ?? 800) / 1000;
    const [r, g, b] =
      stateValue.mode === "cct"
        ? kelvinToRgb(stateValue.cct ?? 3000)
        : hsvToRgb(stateValue.h ?? 30, (stateValue.s ?? 800) / 1000, 1);
    return `rgb(${Math.round(r * level)} ${Math.round(
      g * level,
    )} ${Math.round(b * level)})`;
  }

  function renderPowerOn() {
    const mode = powerOn.mode || "default";
    $("power-on-mode").value = mode;
    const badgeKey =
      mode === "last"
        ? "Last"
        : mode === "default"
          ? "Default"
          : "Custom";
    $("power-on-badge").textContent = t(`powerOn.badge${badgeKey}`);
    const swatch = $("power-on-swatch");
    const detail = $("power-on-detail");
    if (mode === "last") {
      swatch.style.background = powerOnEffective(currentPowerOnState());
      detail.textContent = t("powerOn.lastHint");
      return;
    }
    const stateValue =
      mode === "default" ? POWER_ON_DEFAULT : powerOn.state || POWER_ON_DEFAULT;
    swatch.style.background = powerOnEffective(stateValue);
    if (stateValue.mode === "cct") {
      detail.textContent = t("powerOn.detailCct", {
        kelvin: stateValue.cct,
        brightness: stateValue.brightness ?? 700,
      });
    } else {
      detail.textContent = t("powerOn.detailColor", {
        h: stateValue.h,
        s: stateValue.s,
        brightness: stateValue.brightness ?? 800,
      });
    }
  }

  async function refreshPowerOn() {
    if (!instance || !instance.id) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/power-on`,
        { headers: authHeaders() },
      );
      if (!response.ok) return;
      powerOn = await response.json();
      renderPowerOn();
    } catch {
      /* retried on reconnect */
    }
  }

  async function savePowerOn() {
    if (!instance || !instance.id) return;
    const mode = powerOn.mode || $("power-on-mode").value;
    const stateValue =
      mode === "custom" ? powerOn.state || currentPowerOnState() : null;
    const response = await fetch(
      `${API}/api/v1/instances/${instance.id}/power-on`,
      {
        method: "PUT",
        headers: { ...authHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify({ mode, state: stateValue }),
      },
    );
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      $("power-on-detail").textContent = t("powerOn.saveError", {
        error: data.error?.message || response.status,
      });
      return;
    }
    powerOn = data;
    renderPowerOn();
  }

  function openPowerOnPicker() {
    const initial =
      powerOn.mode === "custom" && powerOn.state
        ? powerOn.state
        : currentPowerOnState();
    openColorPicker({
      title: t("powerOn.pickTitle"),
      mode: initial.mode === "cct" ? "white" : "color",
      h: initial.h ?? state.h,
      s: initial.s ?? state.s,
      cct: initial.cct ?? state.cct,
      brightness: initial.brightness ?? state.brightness ?? 800,
      onApply: (value) => {
        powerOn.mode = "custom";
        $("power-on-mode").value = "custom";
        powerOn.state =
          value.mode === "white"
            ? {
                mode: "cct",
                cct: value.cct,
                brightness: value.brightness,
              }
            : {
                mode: "color",
                h: value.h,
                s: value.s,
                v: 1000,
                brightness: value.brightness,
              };
        savePowerOn().catch(() => {
          $("power-on-detail").textContent = t("powerOn.saveError", {
            error: "backend",
          });
        });
      },
    });
  }

  function handleMessage(message) {
    switch (message.type) {
      case "state":
        applyState(message.payload || {});
        break;
      case "health":
        applyHealth(message.payload);
        break;
      case "subscribed":
        break;
      case "ack":
        if (
          message.op !== "start_sequence" &&
          message.op !== "stop_sequence"
        ) {
          setLast(`ack ${message.op || ""}`);
        }
        break;
      case "nack": {
        const code = message.payload?.code || "";
        const text = code ? t(`nack.${code}`) : "";
        setLast(
          `nack ${message.op || ""}: ${
            text !== `nack.${code}`
              ? text
              : message.payload?.message || code
          }`,
        );
        break;
      }
      case "event":
        if (message.op === "provisioning") {
          setNetwork(message.payload || {});
        } else if (message.op === "device_online") {
          setDevicePresence(true);
          syncDashboard().catch(() => {});
        } else if (message.op === "device_offline") {
          setDevicePresence(false);
        } else if (message.op === "fault") {
          const code = Number(message.payload?.code) || 0;
          if (code !== state.faultCode) {
            state.faultCode = code;
            renderFault();
            render();
          }
        } else if (message.op === "sim_fault") {
          const code = Number(message.payload?.code) || 0;
          if (code && code !== state.faultCode) {
            state.faultCode = code;
            renderFault();
            render();
          }
        } else if (message.op === "sim_faults_cleared") {
          state.faultCode = 0;
          renderFault();
          render();
        } else if (message.op === "fault_history_cleared") {
          setLast(t("settings.historyCleared"));
        } else if (
          message.op === "sim_power" &&
          message.payload?.action === "factory_reset"
        ) {
          state.faultCode = 0;
          renderFault();
          clearLocalDashboardConfig();
          syncDashboard().catch(() => {});
          setLast(t("settings.resetDone"));
        }
        break;
      case "ping":
        if (ws && ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: "pong" }));
        }
        break;
      default:
        break;
    }
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
    const response = await fetch(`${API}/api/v1/instances`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ profile: "lamp-v1", ttl_s: 3600 }),
    });
    if (!response.ok) {
      throw new Error(`POST /instances -> ${response.status}`);
    }
    const data = await response.json();
    sessionStorage.setItem("lamp_instance", JSON.stringify(data));
    return data;
  }

  async function ensureInstance() {
    if (instance && instance.id) {
      const response = await fetch(`${API}/api/v1/instances/${instance.id}`, {
        headers: authHeaders(),
      });
      if ([401, 403, 404].includes(response.status)) {
        sessionStorage.removeItem("lamp_instance");
        instance = null;
      }
    }
    if (!instance) {
      instance = await getInstance();
    }
    return instance;
  }

  function send(op, payload) {
    if (isLocked()) return;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      setLast(t("state.offlineCommand", { op }));
      return;
    }
    const id = `web-${++commandSeq}`;
    ws.send(JSON.stringify({ id, type: "command", op, payload }));
  }

  function sendThrottled(op, payload) {
    const existing = throttles.get(op);
    if (existing) {
      existing.payload = payload;
      return;
    }
    const entry = {
      payload,
      timer: setTimeout(() => {
        throttles.delete(op);
        send(op, entry.payload);
      }, 80),
    };
    throttles.set(op, entry);
  }

  function setUiMode(mode, notify = true) {
    if (uiMode === mode) return;
    uiMode = mode;
    if (notify) activeEffect = null;
    if (mode === "white") {
      if (notify) send("set_mode", { mode: "cct" });
      if (notify) state.mode = 1;
    } else {
      if (notify) send("set_mode", { mode: "manual" });
      if (notify) state.mode = 0;
    }
    render();
  }

  function drawColorWheel(canvas, hue, sat, markerRadius = 12) {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    if (!canvas._base) {
      const size = canvas.width;
      const center = size / 2;
      const radius = center - 6;
      const base = document.createElement("canvas");
      base.width = size;
      base.height = size;
      const baseCtx = base.getContext("2d");
      const image = baseCtx.createImageData(size, size);
      for (let y = 0; y < size; y += 1) {
        for (let x = 0; x < size; x += 1) {
          const dx = x - center;
          const dy = y - center;
          const distance = Math.sqrt(dx * dx + dy * dy);
          const index = (y * size + x) * 4;
          if (distance > radius) {
            image.data[index + 3] = 0;
            continue;
          }
          const value = (Math.atan2(-dy, dx) * 180) / Math.PI;
          const [r, g, b] = hsvToRgb(
            (value + 360) % 360,
            Math.min(1, distance / radius),
            1,
          );
          image.data[index] = r;
          image.data[index + 1] = g;
          image.data[index + 2] = b;
          image.data[index + 3] = distance > radius - 1 ? 120 : 255;
        }
      }
      baseCtx.putImageData(image, 0, 0);
      canvas._base = base;
      canvas._radius = radius;
      canvas._center = center;
    }
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(canvas._base, 0, 0);
    const angle = (hue * Math.PI) / 180;
    const distance = (sat / 1000) * canvas._radius;
    const x = canvas._center + Math.cos(angle) * distance;
    const y = canvas._center - Math.sin(angle) * distance;
    ctx.beginPath();
    ctx.arc(x, y, markerRadius, 0, Math.PI * 2);
    ctx.strokeStyle = "#ffffff";
    ctx.lineWidth = markerRadius > 10 ? 5 : 3;
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(x, y, markerRadius, 0, Math.PI * 2);
    ctx.strokeStyle = "rgba(0,0,0,0.45)";
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }

  function drawWheel() {
    drawColorWheel(wheel, state.h, state.s);
  }

  const picker = {
    mode: "color",
    h: 30,
    s: 800,
    v: 1000,
    cct: 4000,
    brightness: 800,
    onApply: null,
  };
  let pickerPointer = null;

  function pickerWheelEl() {
    return $("picker-wheel");
  }

  function drawPickerWheel() {
    drawColorWheel(pickerWheelEl(), picker.h, picker.s, 9);
  }

  function renderPickerPresets() {
    const container = $("picker-presets");
    container.textContent = "";
    PRESETS.forEach((preset) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "preset";
      button.style.background = `hsl(${preset.h} ${preset.s / 10}% 50%)`;
      button.title = `H${preset.h}`;
      button.setAttribute("aria-label", `H${preset.h} S${preset.s}`);
      button.addEventListener("click", () => {
        picker.mode = "color";
        picker.h = preset.h;
        picker.s = preset.s;
        renderPicker();
      });
      container.appendChild(button);
    });
  }

  function renderPicker() {
    const colorMode = picker.mode === "color";
    $("picker-color-panel").classList.toggle("hidden", !colorMode);
    $("picker-white-panel").classList.toggle("hidden", colorMode);
    $("picker-mode-color").classList.toggle("active", colorMode);
    $("picker-mode-white").classList.toggle("active", !colorMode);
    $("picker-cct").value = String(picker.cct);
    $("picker-cct-out").textContent = `${picker.cct} K`;
    $("picker-brightness").value = String(picker.brightness);
    $("picker-brightness-out").textContent = String(picker.brightness);
    const [pr, pg, pb] = colorMode
      ? hsvToRgb(picker.h, picker.s / 1000, 1)
      : kelvinToRgb(picker.cct);
    const level = picker.brightness / 1000;
    $("picker-preview-swatch").style.background = `rgb(${Math.round(
      pr * level,
    )} ${Math.round(pg * level)} ${Math.round(pb * level)})`;
    $("picker-preview-detail").textContent = colorMode
      ? `H${Math.round(picker.h)} S${picker.s} · ${picker.brightness}`
      : `${picker.cct} K · ${picker.brightness}`;
    if (colorMode) drawPickerWheel();
    const canvas = pickerWheelEl();
    canvas.setAttribute("aria-valuenow", String(picker.s));
    canvas.setAttribute(
      "aria-valuetext",
      `H${Math.round(picker.h)} S${picker.s}`,
    );
  }

  function openColorPicker(options) {
    picker.mode = options.mode || "color";
    picker.h = Math.min(360, Math.max(0, Number(options.h) || 0));
    picker.s = Math.min(1000, Math.max(0, Number(options.s) || 0));
    picker.v = 1000;
    picker.cct = Math.min(
      6500,
      Math.max(2000, Number(options.cct) || 4000),
    );
    picker.brightness = Math.min(
      1000,
      Math.max(0, Number(options.brightness) || 0),
    );
    picker.onApply = options.onApply || null;
    $("picker-title").textContent = options.title || t("picker.title");
    renderPickerPresets();
    renderPicker();
    const dialog = $("color-picker-dialog");
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
  }

  function closeColorPicker() {
    const dialog = $("color-picker-dialog");
    picker.onApply = null;
    if (!dialog) return;
    if (typeof dialog.close === "function") dialog.close();
    else dialog.removeAttribute("open");
  }

  function pickerFromEvent(event) {
    const canvas = pickerWheelEl();
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !canvas._center) return;
    const scale = canvas.width / rect.width;
    const x = (event.clientX - rect.left) * scale;
    const y = (event.clientY - rect.top) * scale;
    const dx = x - canvas._center;
    const dy = y - canvas._center;
    const distance = Math.min(
      canvas._radius,
      Math.sqrt(dx * dx + dy * dy),
    );
    picker.mode = "color";
    picker.h = Math.round(
      ((Math.atan2(-dy, dx) * 180) / Math.PI + 360) % 360,
    );
    picker.s = Math.round((distance / canvas._radius) * 1000);
    renderPicker();
  }

  const pickerWheel = pickerWheelEl();
  pickerWheel.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    pickerPointer = event.pointerId;
    pickerWheel.setPointerCapture(event.pointerId);
    pickerFromEvent(event);
  });
  pickerWheel.addEventListener("pointermove", (event) => {
    if (pickerPointer !== event.pointerId) return;
    pickerFromEvent(event);
  });
  pickerWheel.addEventListener("pointerup", (event) => {
    if (pickerPointer !== event.pointerId) return;
    pickerWheel.releasePointerCapture(event.pointerId);
    pickerPointer = null;
  });
  pickerWheel.addEventListener("pointercancel", () => {
    pickerPointer = null;
  });
  pickerWheel.addEventListener("keydown", (event) => {
    let handled = true;
    if (event.key === "ArrowLeft") picker.h = (picker.h + 358) % 360;
    else if (event.key === "ArrowRight") picker.h = (picker.h + 2) % 360;
    else if (event.key === "ArrowUp") {
      picker.s = Math.min(1000, picker.s + 25);
    } else if (event.key === "ArrowDown") {
      picker.s = Math.max(0, picker.s - 25);
    } else if (event.key === "Home") picker.s = 0;
    else if (event.key === "End") picker.s = 1000;
    else handled = false;
    if (!handled) return;
    event.preventDefault();
    picker.mode = "color";
    renderPicker();
  });

  $("picker-mode-color").addEventListener("click", () => {
    picker.mode = "color";
    renderPicker();
  });
  $("picker-mode-white").addEventListener("click", () => {
    picker.mode = "white";
    renderPicker();
  });
  $("picker-cct").addEventListener("input", () => {
    picker.mode = "white";
    picker.cct = Number($("picker-cct").value) || 4000;
    renderPicker();
  });
  $("picker-brightness").addEventListener("input", () => {
    picker.brightness = Number($("picker-brightness").value) || 0;
    renderPicker();
  });
  $("picker-apply").addEventListener("click", () => {
    const apply = picker.onApply;
    const value = {
      mode: picker.mode,
      h: picker.h,
      s: picker.s,
      v: 1000,
      cct: picker.cct,
      brightness: picker.brightness,
    };
    closeColorPicker();
    if (apply) apply(value);
  });
  $("picker-cancel").addEventListener("click", closeColorPicker);
  $("picker-close").addEventListener("click", closeColorPicker);
  $("color-picker-dialog").addEventListener("click", (event) => {
    if (event.target === $("color-picker-dialog")) closeColorPicker();
  });
  $("color-picker-dialog").addEventListener("close", () => {
    picker.onApply = null;
  });

  function pickWheelColor(event) {
    if (isLocked()) return;
    const rect = wheel.getBoundingClientRect();
    const size = wheel.width;
    const scale = size / rect.width;
    const x = (event.clientX - rect.left) * scale;
    const y = (event.clientY - rect.top) * scale;
    const dx = x - wheel._center;
    const dy = y - wheel._center;
    const distance = Math.min(
      wheel._radius,
      Math.sqrt(dx * dx + dy * dy),
    );
    state.h = Math.round(
      ((Math.atan2(-dy, dx) * 180) / Math.PI + 360) % 360,
    );
    state.s = Math.round((distance / wheel._radius) * 1000);
    state.v = 1000;
    suspendCircadian();
    if (state.mode !== 0) {
      state.mode = 0;
      send("set_mode", { mode: "manual" });
    }
    render();
    sendThrottled("set_color", { h: state.h, s: state.s, v: state.v });
  }

  let wheelPointer = null;
  wheel.addEventListener("pointerdown", (event) => {
    if (isLocked()) return;
    wheel.focus();
    wheelPointer = event.pointerId;
    wheel.setPointerCapture(event.pointerId);
    pickWheelColor(event);
  });
  wheel.addEventListener("pointermove", (event) => {
    if (wheelPointer !== event.pointerId) return;
    pickWheelColor(event);
  });
  wheel.addEventListener("pointerup", (event) => {
    if (wheelPointer !== event.pointerId) return;
    wheel.releasePointerCapture(event.pointerId);
    wheelPointer = null;
  });
  wheel.addEventListener("keydown", (event) => {
    if (isLocked()) return;
    let handled = true;
    if (event.key === "ArrowLeft") state.h = (state.h + 358) % 360;
    else if (event.key === "ArrowRight") state.h = (state.h + 2) % 360;
    else if (event.key === "ArrowUp") state.s = Math.min(1000, state.s + 25);
    else if (event.key === "ArrowDown") state.s = Math.max(0, state.s - 25);
    else if (event.key === "Home") state.s = 0;
    else if (event.key === "End") state.s = 1000;
    else handled = false;
    if (!handled) return;
    event.preventDefault();
    state.v = 1000;
    render();
    sendThrottled("set_color", { h: state.h, s: state.s, v: state.v });
  });

  function renderPresets() {
    const container = $("presets");
    container.textContent = "";
    PRESETS.forEach((preset) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "preset";
      button.style.background = `hsl(${preset.h} ${preset.s / 10}% 50%)`;
      button.title = `H${preset.h}`;
      button.setAttribute("aria-label", `H${preset.h} S${preset.s}`);
      button.addEventListener("click", () => {
        if (state.stale) return;
        state.h = preset.h;
        state.s = preset.s;
        state.v = 1000;
        state.mode = 0;
        activeEffect = null;
        suspendCircadian();
        if (uiMode !== "color") setUiMode("color", false);
        render();
        send("set_mode", { mode: "manual" });
        send("set_color", { h: state.h, s: state.s, v: 1000 });
      });
      container.appendChild(button);
    });
  }

  function renderScenes() {
    const container = $("scenes");
    container.textContent = "";
    SCENES.forEach((scene) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "chip";
      button.textContent = t(scene.key);
      button.addEventListener("click", () => {
        if (state.stale) return;
        suspendCircadian();
        state.cct = scene.cct;
        state.brightness = scene.brightness;
        state.mode = 1;
        activeEffect = null;
        state.power = true;
        uiMode = "white";
        send("set_power", { on: true });
        send("set_mode", { mode: "cct" });
        send("set_color_temperature", { kelvin: scene.cct });
        send("set_brightness", { value: scene.brightness });
        setLast(t(scene.key));
        render();
      });
      container.appendChild(button);
    });
  }

  function renderEffects() {
    const container = $("effects");
    container.textContent = "";
    EFFECTS.forEach((effect) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "chip";
      button.textContent = t(effect.key);
      button.addEventListener("click", () => {
        if (state.stale) return;
        const speed = Number($("effect-speed").value) || 1000;
        state.mode = 2;
        state.power = true;
        activeEffect = effect;
        suspendCircadian();
        send("start_sequence", {
          id: effect.id,
          speed,
          direction: 0,
          loop: 0,
        });
        setLast(t("effect.running", { name: t(effect.key) }));
        render();
      });
      container.appendChild(button);
    });
  }

  function loadSequence() {
    try {
      const raw = JSON.parse(localStorage.getItem(SEQUENCE_KEY) || "[]");
      if (!Array.isArray(raw)) return [];
      return raw
        .slice(0, SEQUENCE_MAX)
        .filter((step) => step && Number.isFinite(Number(step.h)))
        .map((step) => ({
          h: Math.min(360, Math.max(0, Math.round(Number(step.h)))),
          s: Math.min(1000, Math.max(0, Math.round(Number(step.s) || 1000))),
          v: Math.min(1000, Math.max(0, Math.round(Number(step.v) || 1000))),
          brightness: Math.min(
            1000,
            Math.max(0, Math.round(Number(step.brightness) || 800)),
          ),
          cct: Math.min(
            6500,
            Math.max(2000, Math.round(Number(step.cct) || 2700)),
          ),
          duration_ms: Math.min(
            60000,
            Math.max(500, Math.round(Number(step.duration_ms) || 2000)),
          ),
          transition_ms: Math.min(
            10000,
            Math.max(0, Math.round(Number(step.transition_ms) || 0)),
          ),
        }));
    } catch {
      return [];
    }
  }

  function saveSequence() {
    try {
      localStorage.setItem(SEQUENCE_KEY, JSON.stringify(sequence));
    } catch {
      /* private mode: the sequence lasts for the page */
    }
  }

  function loadSequenceSettings() {
    try {
      const raw = JSON.parse(
        localStorage.getItem(SEQUENCE_SETTINGS_KEY) || "{}",
      );
      const fade = Number(raw.fade_ms);
      const speed = Number(raw.speed);
      if (Number.isFinite(fade)) {
        sequenceSettings.fade_ms = Math.min(3000, Math.max(0, fade));
      }
      if ([500, 1000, 1500, 2000].includes(speed)) {
        sequenceSettings.speed = speed;
      }
    } catch {
      /* default values */
    }
  }

  function saveSequenceSettings() {
    try {
      localStorage.setItem(
        SEQUENCE_SETTINGS_KEY,
        JSON.stringify(sequenceSettings),
      );
    } catch {
      /* private mode */
    }
  }

  function updateSequenceSummary() {
    const el = $("sequences-summary");
    if (!el) return;
    if (!sequence.length) {
      el.textContent = t("sequences.summaryEmpty");
      return;
    }
    const total =
      sequence.reduce((sum, step) => sum + step.duration_ms, 0) / 1000;
    el.textContent = t("sequences.summary", {
      count: sequence.length,
      seconds: total.toFixed(1),
    });
  }

  function updateSequenceControls() {
    const disabled = state.stale;
    $("seq-add").disabled = disabled || sequence.length >= SEQUENCE_MAX;
    $("seq-start").disabled = disabled || sequence.length < 2;
    $("seq-stop").disabled = disabled;
    document
      .querySelectorAll(".sequence-step input, .sequence-step button")
      .forEach((element) => {
        element.disabled = disabled;
      });
  }

  function renderSequence() {
    updateSequenceSummary();
    const container = $("sequence-steps");
    container.textContent = "";
    if (!sequence.length) {
      const empty = document.createElement("p");
      empty.className = "mono muted";
      empty.textContent = t("sequences.empty");
      container.appendChild(empty);
      return;
    }
    sequence.forEach((step, index) => {
      const row = document.createElement("div");
      row.className = "sequence-step";

      const handle = document.createElement("span");
      handle.className = "drag-handle";
      handle.textContent = "⋮⋮";
      handle.title = t("sequences.drag");
      handle.setAttribute("aria-label", t("sequences.drag"));
      handle.draggable = true;
      handle.addEventListener("dragstart", (event) => {
        event.dataTransfer.effectAllowed = "move";
        event.dataTransfer.setData("text/plain", String(index));
        row.classList.add("dragging");
      });
      handle.addEventListener("dragend", () => {
        row.classList.remove("dragging");
      });

      row.addEventListener("dragover", (event) => {
        event.preventDefault();
        event.dataTransfer.dropEffect = "move";
        row.classList.add("drag-over");
      });
      row.addEventListener("dragleave", () => {
        row.classList.remove("drag-over");
      });
      row.addEventListener("drop", (event) => {
        event.preventDefault();
        row.classList.remove("drag-over");
        const raw = event.dataTransfer.getData("text/plain");
        if (!raw) return;
        const from = Number(raw);
        if (
          !Number.isInteger(from) ||
          from < 0 ||
          from >= sequence.length ||
          from === index
        ) {
          return;
        }
        const [moved] = sequence.splice(from, 1);
        sequence.splice(index, 0, moved);
        saveSequence();
        renderSequence();
        updateSequenceControls();
      });

      const color = document.createElement("button");
      color.type = "button";
      color.className = "seq-color";
      const [r, g, b] =
        step.s === 0
          ? kelvinToRgb(step.cct)
          : hsvToRgb(step.h, step.s / 1000, step.v / 1000);
      const level = step.brightness / 1000;
      color.style.background = `rgb(${Math.round(r * level)} ${Math.round(
        g * level,
      )} ${Math.round(b * level)})`;
      const colorLabel = `${t("sequences.color")} · ${Math.round(
        level * 100,
      )}%`;
      color.title = colorLabel;
      color.setAttribute("aria-label", colorLabel);
      color.addEventListener("click", () => {
        openColorPicker({
          title: t("picker.titleStep", { n: index + 1 }),
          mode: step.s === 0 ? "white" : "color",
          h: step.h,
          s: step.s,
          cct: step.cct,
          brightness: step.brightness,
          onApply: (value) => {
            if (value.mode === "white") {
              step.h = 0;
              step.s = 0;
              step.cct = value.cct;
            } else {
              step.h = value.h;
              step.s = value.s;
              step.cct = value.cct;
            }
            step.v = 1000;
            step.brightness = value.brightness;
            saveSequence();
            renderSequence();
          },
        });
      });

      const name = document.createElement("span");
      name.className = "seq-index";
      name.textContent = String(index + 1);
      name.title = t("sequences.step", { n: index + 1 });
      name.setAttribute("aria-label", t("sequences.step", { n: index + 1 }));

      const duration = document.createElement("input");
      duration.type = "number";
      duration.className = "seq-duration";
      duration.min = "0.5";
      duration.max = "60";
      duration.step = "0.5";
      duration.value = String(step.duration_ms / 1000);
      duration.title = t("sequences.duration");
      const unit = document.createElement("span");
      unit.className = "seq-unit";
      unit.textContent = "s";
      duration.setAttribute("aria-label", t("sequences.duration"));
      duration.addEventListener("change", () => {
        const seconds = Math.min(
          60,
          Math.max(0.5, Number(duration.value) || 2),
        );
        step.duration_ms = Math.round(seconds * 1000);
        duration.value = String(seconds);
        saveSequence();
        updateSequenceSummary();
      });

      const up = document.createElement("button");
      up.type = "button";
      up.className = "secondary seq-mini";
      up.textContent = "↑";
      up.title = t("sequences.up");
      up.setAttribute("aria-label", t("sequences.up"));
      up.addEventListener("click", () => {
        if (index === 0) return;
        [sequence[index - 1], sequence[index]] = [
          sequence[index],
          sequence[index - 1],
        ];
        saveSequence();
        renderSequence();
        updateSequenceControls();
      });

      const down = document.createElement("button");
      down.type = "button";
      down.className = "secondary seq-mini";
      down.textContent = "↓";
      down.title = t("sequences.down");
      down.setAttribute("aria-label", t("sequences.down"));
      down.addEventListener("click", () => {
        if (index === sequence.length - 1) return;
        [sequence[index + 1], sequence[index]] = [
          sequence[index],
          sequence[index + 1],
        ];
        saveSequence();
        renderSequence();
        updateSequenceControls();
      });

      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "secondary seq-mini";
      remove.textContent = "×";
      remove.title = t("sequences.remove");
      remove.setAttribute("aria-label", t("sequences.remove"));
      remove.addEventListener("click", () => {
        sequence.splice(index, 1);
        saveSequence();
        renderSequence();
        updateSequenceControls();
      });

      row.append(
        handle,
        name,
        color,
        duration,
        unit,
        up,
        down,
        remove,
      );
      container.appendChild(row);
    });
  }

  function setSequenceStatus(text) {
    $("seq-status").textContent = text;
  }

  function addCurrentColor() {
    if (state.stale || sequence.length >= SEQUENCE_MAX) return;
    let hue = state.h;
    let sat = state.s;
    if (state.mode === 1) {
      hue = 30;
      sat = state.cct < 3000 ? 350 : state.cct > 5000 ? 150 : 220;
    }
    sequence.push({
      h: Math.round(hue),
      s: Math.round(sat),
      v: 1000,
      brightness: state.brightness || 800,
      cct: state.cct,
      duration_ms: 4000,
      transition_ms: 800,
    });
    saveSequence();
    renderSequence();
    updateSequenceControls();
    setSequenceStatus(t("sequences.added"));
  }

  async function startSequence() {
    if (!instance || sequence.length < 2) {
      setSequenceStatus(t("sequences.empty"));
      return;
    }
    if (state.stale) return;
    const fade = Math.round(
      Math.min(3, Math.max(0, Number($("seq-fade").value) || 0)) * 1000,
    );
    const speed = Number($("seq-speed").value) || 1000;
    sequenceSettings = { fade_ms: fade, speed };
    saveSequenceSettings();
    setSequenceStatus(t("sequences.uploading"));
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/sequences`,
        {
          method: "POST",
          headers: { ...authHeaders(), "Content-Type": "application/json" },
          body: JSON.stringify({
            steps: sequence.map((step) => ({
              ...step,
              transition_ms: fade,
            })),
            speed,
          }),
        },
      );
      const data = await response.json().catch(() => ({}));
      if (!response.ok) {
        setSequenceStatus(
          t("sequences.error", {
            error: data.error?.message || response.status,
          }),
        );
        return;
      }
      if (data.ack && data.ack.type === "nack") {
        setSequenceStatus(
          t("sequences.error", {
            error: data.ack.payload?.code || "nack",
          }),
        );
        return;
      }
      const message = t("sequences.started", { count: sequence.length });
      setSequenceStatus(message);
      setLast(message);
      state.mode = 2;
      state.power = true;
      render();
    } catch {
      setSequenceStatus(t("sequences.error", { error: "backend" }));
    }
  }

  function stopSequence() {
    if (state.stale) return;
    send("stop_sequence", {});
    setSequenceStatus(t("sequences.stopped"));
    setLast(t("sequences.stopped"));
  }

  function setPartyStatus(text) {
    $("party-status").textContent = text;
  }

  function updatePartyControls() {
    const toggle = $("party-toggle");
    toggle.textContent = party.active ? t("party.stop") : t("party.start");
    toggle.disabled = state.stale;
    $("party-source").disabled = party.active || state.stale;
  }

  function releasePartyAudio() {
    if (party.stream) {
      party.stream.getTracks().forEach((track) => track.stop());
      party.stream = null;
    }
    if (party.ctx) {
      party.ctx.close().catch(() => {});
      party.ctx = null;
    }
    party.analyser = null;
    party.data = null;
  }

  async function setupPartyAudio() {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      return false;
    }
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return false;
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: {
          echoCancellation: false,
          noiseSuppression: false,
          autoGainControl: false,
        },
      });
      const ctx = new Ctx();
      ctx.resume().catch(() => {});
      const analyser = ctx.createAnalyser();
      analyser.fftSize = 256;
      analyser.smoothingTimeConstant = 0.5;
      ctx.createMediaStreamSource(stream).connect(analyser);
      party.stream = stream;
      party.ctx = ctx;
      party.analyser = analyser;
      party.data = new Uint8Array(analyser.fftSize);
      return true;
    } catch {
      return false;
    }
  }

  function stopParty(silent = false) {
    clearInterval(party.timer);
    party.timer = null;
    releasePartyAudio();
    party.active = false;
    party.level = 0;
    $("party-level").style.width = "0%";
    updatePartyControls();
    if (!silent) setPartyStatus(t("party.stopped"));
  }

  async function startParty() {
    if (state.stale || party.active) return;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      setPartyStatus(t("party.error", { error: "offline" }));
      return;
    }
    party.source = $("party-source").value === "auto" ? "auto" : "mic";
    try {
      localStorage.setItem(PARTY_SOURCE_KEY, party.source);
    } catch {
      /* private mode */
    }
    party.active = true;
    party.hue = state.h;
    party.ticks = 0;
    party.baseline = 0.05;
    party.lastBeat = 0;
    party.level = 0;
    party.fallback = false;
    party.session += 1;
    const session = party.session;
    send("set_power", { on: true });
    send("set_mode", { mode: "manual" });
    state.power = true;
    state.mode = 0;
    uiMode = "color";
    activeEffect = null;
    suspendCircadian();
    if (party.source === "mic") {
      const ok = await setupPartyAudio();
      if (!party.active || party.session !== session) {
        releasePartyAudio();
        return;
      }
      if (!ok) {
        party.source = "auto";
        party.fallback = true;
        $("party-source").value = "auto";
        setPartyStatus(t("party.fallback"));
      }
    }
    if (!party.active) {
      updatePartyControls();
      return;
    }
    if (party.source === "mic") {
      setPartyStatus(t("party.runningMic", { level: 0 }));
    } else {
      setPartyStatus(t("party.runningAuto"));
    }
    updatePartyControls();
    party.timer = setInterval(tickParty, 125);
    tickParty();
  }

  function tickParty() {
    if (!party.active) return;
    if (state.stale || !ws || ws.readyState !== WebSocket.OPEN) {
      stopParty(true);
      setPartyStatus(t("party.stopped"));
      return;
    }

    const now = performance.now();
    let beat = false;

    if (party.source === "mic" && party.analyser && party.data) {
      party.analyser.getByteTimeDomainData(party.data);
      let sum = 0;
      for (const sample of party.data) {
        const value = (sample - 128) / 128;
        sum += value * value;
      }
      const rms = Math.sqrt(sum / party.data.length);
      party.level = Math.min(1, rms * 4.5);
      const threshold = Math.max(0.12, party.baseline * 1.6);
      if (party.level > threshold && now - party.lastBeat > 180) {
        beat = true;
        party.lastBeat = now;
      }
      party.baseline = party.baseline * 0.92 + party.level * 0.08;
    } else {
      party.ticks += 1;
      beat = party.ticks % 5 === 0;
      party.level = beat
        ? 1
        : 0.35 + 0.15 * Math.sin(party.ticks / 3);
      if (beat) party.lastBeat = now;
    }

    party.hue = (party.hue + (beat ? 32 : 1.5)) % 360;
    const brightness = Math.round(beat ? 950 : 280 + party.level * 620);
    state.h = Math.round(party.hue);
    state.s = 1000;
    state.v = 1000;
    state.brightness = Math.min(1000, brightness);
    state.mode = 0;
    state.power = true;
    render();
    send("set_color", { h: state.h, s: 1000, v: 1000 });
    send("set_brightness", { value: state.brightness });
    $("party-level").style.width = `${Math.round(party.level * 100)}%`;
    if (party.source === "mic") {
      setPartyStatus(
        t("party.runningMic", { level: Math.round(party.level * 100) }),
      );
    } else {
      setPartyStatus(
        party.fallback ? t("party.runningFallback") : t("party.runningAuto"),
      );
    }
  }

  function repeatName(repeat) {
    return {
      once: "schedules.once",
      daily: "schedules.daily",
      weekdays: "schedules.weekdays",
      weekends: "schedules.weekends",
      custom: "schedules.custom",
    }[repeat] || repeat;
  }

  function dayName(index) {
    return ["day.mon", "day.tue", "day.wed", "day.thu", "day.fri", "day.sat", "day.sun"][
      index
    ];
  }

  function scheduleActionLabel(action) {
    if (action.type === "scene") return t(`scene.${action.scene}`);
    if (action.type === "off") return t("schedules.actionOff");
    if (action.type === "color") return t("schedules.actionColor");
    if (action.type === "cct") return `${t("schedules.actionCct")} · ${action.kelvin} K`;
    if (action.type === "effect") return t(`effect.${action.effect}`);
    if (action.type === "sunrise") {
      return `${t("schedules.sunrise")} · ${action.duration_min} min`;
    }
    return action.type;
  }

  function scheduleRepeatLabel(schedule) {
    if (schedule.repeat === "custom") {
      const names = (schedule.days || []).map((day) => t(dayName(day)));
      return `${t("schedules.custom")}: ${names.join(", ")}`;
    }
    return t(repeatName(schedule.repeat));
  }

  function localDateTime(value) {
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "—";
    return date.toLocaleString(document.documentElement.lang || "en", {
      weekday: "short",
      hour: "2-digit",
      minute: "2-digit",
    });
  }

  function updateSchedulesSummary() {
    const el = $("schedules-summary");
    if (!el) return;
    if (!schedules.length) {
      el.textContent = t("schedules.summaryEmpty");
      return;
    }
    const next = schedules
      .filter((schedule) => schedule.enabled && schedule.next_run)
      .map((schedule) => schedule.next_run)
      .sort()[0];
    el.textContent = next
      ? t("schedules.summaryNext", {
          count: schedules.length,
          time: localDateTime(next),
        })
      : t("schedules.summaryCount", { count: schedules.length });
  }

  function renderSchedules() {
    updateSchedulesSummary();
    const list = $("schedule-list");
    list.textContent = "";
    if (!schedules.length) {
      const item = document.createElement("li");
      item.className = "schedule-empty";
      item.textContent = t("schedules.empty");
      list.appendChild(item);
      return;
    }
    schedules.forEach((schedule) => {
      const item = document.createElement("li");
      item.className = schedule.enabled ? "" : "schedule-disabled";

      const head = document.createElement("div");
      head.className = "schedule-head";
      const toggleLabel = document.createElement("label");
      toggleLabel.className = "schedule-toggle";
      const toggle = document.createElement("input");
      toggle.type = "checkbox";
      toggle.checked = schedule.enabled;
      toggle.addEventListener("change", () => {
        toggleSchedule(schedule.id, toggle.checked).catch(() => {
          setStatusSchedule(t("schedules.error", { error: "backend" }));
        });
      });
      const timeLabel = document.createElement("strong");
      timeLabel.textContent = schedule.time;
      toggleLabel.append(toggle, timeLabel);

      const buttons = document.createElement("span");
      buttons.className = "schedule-buttons";
      const edit = document.createElement("button");
      edit.type = "button";
      edit.className = "secondary seq-mini";
      edit.textContent = "✎";
      edit.title = t("schedules.edit");
      edit.setAttribute("aria-label", t("schedules.edit"));
      edit.addEventListener("click", () => editSchedule(schedule));
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "secondary seq-mini";
      remove.textContent = "×";
      remove.title = t("schedules.delete");
      remove.setAttribute("aria-label", t("schedules.delete"));
      remove.addEventListener("click", () => {
        deleteSchedule(schedule.id).catch(() => {
          setStatusSchedule(t("schedules.error", { error: "backend" }));
        });
      });
      buttons.append(edit, remove);
      head.append(toggleLabel, buttons);

      const meta = document.createElement("div");
      meta.className = "schedule-meta";
      meta.textContent = `${scheduleRepeatLabel(schedule)} · ${scheduleActionLabel(
        schedule.action,
      )}`;

      const next = document.createElement("div");
      next.className = "schedule-meta";
      if (!schedule.enabled) {
        next.textContent = t("schedules.disabled");
      } else if (schedule.next_run) {
        next.textContent = t("schedules.nextAt", {
          time: localDateTime(schedule.next_run),
        });
      } else {
        next.textContent = t("schedules.noNext");
      }

      item.append(head, meta, next);
      if (schedule.last_status) {
        const last = document.createElement("div");
        last.className = "schedule-meta";
        last.textContent = t(`schedules.last.${schedule.last_status}`);
        item.appendChild(last);
      }
      list.appendChild(item);
    });
  }

  function setStatusSchedule(text) {
    $("schedule-status").textContent = text;
  }

  function scheduleActionKind(action) {
    if (!action) return null;
    if (action.type === "color" || action.type === "cct") return "color";
    return action.type;
  }

  function updateScheduleScene() {
    if (!scheduleDraft || scheduleDraft.type !== "scene") return;
    scheduleDraft.scene = $("schedule-scene").value;
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  function updateScheduleEffect() {
    if (!scheduleDraft || scheduleDraft.type !== "effect") return;
    scheduleDraft.effect = $("schedule-effect").value;
    scheduleDraft.speed = Number($("schedule-effect-speed").value) || 1000;
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  function updateScheduleSunrise() {
    if (!scheduleDraft || scheduleDraft.type !== "sunrise") return;
    scheduleDraft.duration_min =
      Number($("schedule-sunrise-duration").value) || 30;
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  function setScheduleAction(kind) {
    scheduleAction = kind;
    if (kind === "current") {
      captureCurrentAction();
      return;
    }
    if (kind === "color") {
      if (!scheduleDraft || !["color", "cct"].includes(scheduleDraft.type)) {
        scheduleDraft = {
          type: "cct",
          kelvin: state.mode === 1 ? state.cct : 4000,
          brightness: state.brightness || 800,
        };
      }
    } else if (kind === "scene") {
      if (!scheduleDraft || scheduleDraft.type !== "scene") {
        scheduleDraft = {
          type: "scene",
          scene: $("schedule-scene").value || "reading",
        };
      }
    } else if (kind === "effect") {
      if (!scheduleDraft || scheduleDraft.type !== "effect") {
        scheduleDraft = {
          type: "effect",
          effect: $("schedule-effect").value || "breathing",
          speed: Number($("schedule-effect-speed").value) || 1000,
        };
      }
    } else if (kind === "off") {
      scheduleDraft = { type: "off" };
    } else if (kind === "sunrise") {
      scheduleDraft = {
        type: "sunrise",
        kelvin: state.mode === 1 ? state.cct : 4000,
        brightness: state.brightness || 800,
        duration_min: Number($("schedule-sunrise-duration").value) || 30,
      };
    }
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    renderScheduleFields();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  function renderScheduleFields() {
    const repeat = $("schedule-repeat").value;
    $("schedule-days").classList.toggle("hidden", repeat !== "custom");
    $("schedule-add").disabled = scheduleDraft === null;

    const kind = scheduleAction || scheduleActionKind(scheduleDraft);
    document.querySelectorAll("[data-schedule-action]").forEach((button) => {
      const active = button.dataset.scheduleAction === kind;
      button.classList.toggle("active", active);
      button.setAttribute("aria-pressed", active ? "true" : "false");
    });
    document.querySelectorAll("[data-schedule-panel]").forEach((panel) => {
      panel.classList.toggle(
        "hidden",
        panel.dataset.schedulePanel !== kind,
      );
    });
  }

  function renderSchedulePreview() {
    const dot = $("schedule-preview-dot");
    const text = $("schedule-preview-text");
    if (!scheduleDraft) {
      dot.style.background = "transparent";
      text.textContent = t("schedules.chooseAction");
      return;
    }
    const action = scheduleDraft;
    if (action.type === "off") {
      dot.style.background = "#3a4250";
      text.textContent = t("schedules.turnOff");
    } else if (action.type === "cct") {
      const [r, g, b] = kelvinToRgb(action.kelvin);
      dot.style.background = `rgb(${r} ${g} ${b})`;
      text.textContent = t("schedules.currentWhite", {
        kelvin: action.kelvin,
        brightness: action.brightness ?? 800,
      });
    } else if (action.type === "color") {
      const [r, g, b] = hsvToRgb(action.h, action.s / 1000, 1);
      dot.style.background = `rgb(${r} ${g} ${b})`;
      text.textContent = t("schedules.currentColor", {
        h: action.h,
        s: action.s,
        brightness: action.brightness ?? 800,
      });
    } else if (action.type === "scene") {
      const scene = SCENES.find(
        (item) => item.key === `scene.${action.scene}`,
      );
      const cct = scene ? scene.cct : 2700;
      const [r, g, b] = kelvinToRgb(cct);
      dot.style.background = `rgb(${r} ${g} ${b})`;
      text.textContent = `${t(`scene.${action.scene}`)} · ${cct} K`;
    } else if (action.type === "effect") {
      dot.style.background =
        "linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f)";
      text.textContent = t("schedules.currentEffect", {
        name: t(`effect.${action.effect}`),
        speed: action.speed ?? 1000,
      });
    } else if (action.type === "sunrise") {
      dot.style.background =
        "linear-gradient(90deg,#ff9b52,#ffd6a5,#fff4e6)";
      text.textContent = t("schedules.currentSunrise", {
        minutes: action.duration_min,
        kelvin: action.kelvin,
      });
    }
  }

  function scheduleSwatchColor(action) {
    if (action && action.type === "color") {
      const [r, g, b] = hsvToRgb(action.h, action.s / 1000, 1);
      const level = (action.brightness ?? 800) / 1000;
      return `rgb(${Math.round(r * level)} ${Math.round(
        g * level,
      )} ${Math.round(b * level)})`;
    }
    if (action && action.type === "cct") {
      const [r, g, b] = kelvinToRgb(action.kelvin ?? 4000);
      const level = (action.brightness ?? 800) / 1000;
      return `rgb(${Math.round(r * level)} ${Math.round(
        g * level,
      )} ${Math.round(b * level)})`;
    }
    if (action && action.type === "scene") {
      const scene = SCENES.find(
        (item) => item.key === `scene.${action.scene}`,
      );
      const [r, g, b] = kelvinToRgb(scene ? scene.cct : 2700);
      return `rgb(${r} ${g} ${b})`;
    }
    if (action && action.type === "effect") {
      return "linear-gradient(90deg,#f00,#ff0,#0f0,#0ff,#00f,#f0f)";
    }
    if (action && action.type === "sunrise") {
      return "linear-gradient(90deg,#ff9b52,#ffd6a5,#fff4e6)";
    }
    if (action && action.type === "off") return "#3a4250";
    const [r, g, b] =
      state.mode === 1
        ? kelvinToRgb(state.cct)
        : hsvToRgb(state.h, state.s / 1000, 1);
    return `rgb(${r} ${g} ${b})`;
  }

  function syncScheduleBuilder(action = null) {
    $("schedule-color-swatch").style.background = scheduleSwatchColor(action);
    if (action && action.type === "scene") {
      $("schedule-scene").value = action.scene;
    }
    if (action && action.type === "effect") {
      $("schedule-effect").value = action.effect;
      $("schedule-effect-speed").value = String(action.speed || 1000);
    }
    if (action && action.type === "sunrise") {
      $("schedule-sunrise-duration").value = String(
        action.duration_min || 30,
      );
    }
    renderScheduleFields();
  }

  function openScheduleColorPicker() {
    const action = scheduleDraft;
    const isColor = action && action.type === "color";
    const isCct = action && action.type === "cct";
    openColorPicker({
      title: t("picker.titleSchedule"),
      mode: isCct ? "white" : "color",
      h: isColor ? action.h : state.h,
      s: isColor ? action.s : state.s,
      cct: isCct ? action.kelvin : state.cct,
      brightness:
        isColor || isCct
          ? action.brightness ?? 800
          : state.brightness || 800,
      onApply: (value) => {
        if (value.mode === "white") {
          scheduleDraft = {
            type: "cct",
            kelvin: value.cct,
            brightness: value.brightness,
          };
        } else {
          scheduleDraft = {
            type: "color",
            h: value.h,
            s: value.s,
            v: 1000,
            brightness: value.brightness,
          };
        }
        syncScheduleBuilder(scheduleDraft);
        renderSchedulePreview();
        renderScheduleFields();
        setStatusSchedule(t("schedules.actionSelected"));
      },
    });
  }

  function captureCurrentAction() {
    if (state.mode === 2 && activeEffect) {
      scheduleDraft = {
        type: "effect",
        effect: activeEffect.key.replace("effect.", ""),
        speed: Number($("effect-speed").value) || 1000,
      };
    } else if (state.mode === 1) {
      scheduleDraft = {
        type: "cct",
        kelvin: state.cct,
        brightness: state.brightness || 800,
      };
    } else {
      scheduleDraft = {
        type: "color",
        h: Math.round(state.h),
        s: state.s,
        v: 1000,
        brightness: state.brightness || 800,
      };
    }
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    renderScheduleFields();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  function setScheduleOff() {
    scheduleDraft = { type: "off" };
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    renderScheduleFields();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  function captureSunrise() {
    scheduleDraft = {
      type: "sunrise",
      kelvin: state.mode === 1 ? state.cct : 4000,
      brightness: state.brightness || 800,
      duration_min: Number($("schedule-sunrise-duration").value) || 30,
    };
    syncScheduleBuilder(scheduleDraft);
    renderSchedulePreview();
    renderScheduleFields();
    setStatusSchedule(t("schedules.actionSelected"));
  }

  async function loadSchedules() {
    if (!instance || !instance.id) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/schedules`,
        { headers: authHeaders() },
      );
      if (!response.ok) return;
      const data = await response.json();
      schedules = data.schedules || [];
      renderSchedules();
      updateScheduleZone();
    } catch {
      /* retried on the next cycle */
    }
  }

  function schedulePayload() {
    const time = $("schedule-time").value;
    const repeat = $("schedule-repeat").value;
    if (!time) throw new Error("time required");
    if (!scheduleDraft) throw new Error(t("schedules.noAction"));
    const days =
      repeat === "custom"
        ? [...document.querySelectorAll("#schedule-days input:checked")].map(
            (input) => Number(input.value),
          )
        : [];
    if (repeat === "custom" && !days.length) throw new Error("days required");

    return {
      time,
      timezone:
        Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC",
      repeat,
      days,
      action: scheduleDraft,
      enabled: true,
    };
  }

  async function submitSchedule() {
    if (!instance || !instance.id) return;
    let payload;
    try {
      payload = schedulePayload();
    } catch (error) {
      setStatusSchedule(t("schedules.error", { error: error.message }));
      return;
    }
    const editing = editingScheduleId;
    const response = await fetch(
      editing
        ? `${API}/api/v1/instances/${instance.id}/schedules/${editing}`
        : `${API}/api/v1/instances/${instance.id}/schedules`,
      {
        method: editing ? "PUT" : "POST",
        headers: { ...authHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      },
    );
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      setStatusSchedule(
        t("schedules.error", {
          error: data.error?.message || response.status,
        }),
      );
      return;
    }
    setStatusSchedule(
      t(editing ? "schedules.updated" : "schedules.saved", {
        time: data.time,
      }),
    );
    resetScheduleForm();
    await loadSchedules();
  }

  function resetScheduleForm() {
    editingScheduleId = null;
    scheduleDraft = null;
    scheduleAction = null;
    $("schedule-add").textContent = t("schedules.add");
    $("schedule-cancel").classList.add("hidden");
    $("schedule-days")
      .querySelectorAll("input")
      .forEach((input) => {
        input.checked = false;
      });
    syncScheduleBuilder();
    renderSchedulePreview();
    renderScheduleFields();
  }

  function editSchedule(schedule) {
    editingScheduleId = schedule.id;
    scheduleDraft = { ...schedule.action };
    scheduleAction = scheduleActionKind(schedule.action);
    $("schedule-time").value = schedule.time;
    $("schedule-repeat").value = schedule.repeat;
    $("schedule-days")
      .querySelectorAll("input")
      .forEach((input) => {
        input.checked = (schedule.days || []).includes(Number(input.value));
      });
    $("schedule-add").textContent = t("schedules.save");
    $("schedule-cancel").classList.remove("hidden");
    syncScheduleBuilder(schedule.action);
    renderSchedulePreview();
    renderScheduleFields();
    setStatusSchedule(t("schedules.editing"));
  }

  async function toggleSchedule(scheduleId, enabled) {
    await fetch(
      `${API}/api/v1/instances/${instance.id}/schedules/${scheduleId}`,
      {
        method: "PATCH",
        headers: { ...authHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify({ enabled }),
      },
    );
    await loadSchedules();
  }

  async function deleteSchedule(scheduleId) {
    if (!instance || !instance.id) return;
    await fetch(
      `${API}/api/v1/instances/${instance.id}/schedules/${scheduleId}`,
      { method: "DELETE", headers: authHeaders() },
    );
    await loadSchedules();
    setStatusSchedule(t("schedules.hint"));
  }

  function validCircadianPoint(point) {
    return (
      Array.isArray(point) &&
      Number.isFinite(point[0]) &&
      point[0] >= 0 &&
      point[0] <= 24 &&
      Number.isFinite(point[1]) &&
      point[1] >= 2000 &&
      point[1] <= 6500 &&
      (point[2] === null ||
        (Number.isFinite(point[2]) && point[2] >= 0 && point[2] <= 1000))
    );
  }

  function normalizeCircadianPoint(item) {
    const point = Array.isArray(item)
      ? [Number(item[0]), Number(item[1]), item[2] == null ? null : Number(item[2])]
      : [
          Number(item.hour),
          Number(item.kelvin),
          item.brightness == null ? null : Number(item.brightness),
        ];
    if (!validCircadianPoint(point)) return null;
    if (point[2] === null) point[2] = circadianBrightness(point[1]);
    return point;
  }

  function loadCircadianConfig() {
    try {
      const raw = JSON.parse(
        localStorage.getItem(CIRCADIAN_CONFIG_KEY) || "null",
      );
      if (raw && Array.isArray(raw.points)) {
        const points = raw.points
          .map(normalizeCircadianPoint)
          .filter(Boolean)
          .sort((a, b) => a[0] - b[0]);
        if (points.length >= 2) {
          circadianConfig = {
            points,
            adjustBrightness: Boolean(raw.adjustBrightness),
            transition: raw.transition === "quick" ? "quick" : "gradual",
            transitionSeconds: Math.min(
              60,
              Math.max(1, Number(raw.transitionSeconds) || 5),
            ),
          };
        }
      }
      const until = Number(
        localStorage.getItem(CIRCADIAN_OVERRIDE_KEY) || 0,
      );
      if (Number.isFinite(until) && until > Date.now()) {
        circadianOverrideUntil = until;
      } else {
        localStorage.removeItem(CIRCADIAN_OVERRIDE_KEY);
      }
    } catch {
      /* default values */
    }
  }

  function saveCircadianConfig() {
    try {
      localStorage.setItem(
        CIRCADIAN_CONFIG_KEY,
        JSON.stringify(circadianConfig),
      );
    } catch {
      /* private mode */
    }
  }

  function circadianValues(date = new Date()) {
    const points = circadianConfig.points.length
      ? circadianConfig.points
      : DEFAULT_CIRCADIAN_POINTS;
    const hour = date.getHours() + date.getMinutes() / 60;
    if (hour <= points[0][0]) {
      return { kelvin: points[0][1], brightness: points[0][2] };
    }
    for (let index = 1; index < points.length; index += 1) {
      const [startAt, startKelvin, startBrightness] = points[index - 1];
      const [endAt, endKelvin, endBrightness] = points[index];
      if (hour <= endAt) {
        const span = endAt - startAt || 1;
        const progress = (hour - startAt) / span;
        return {
          kelvin: Math.round(
            (startKelvin + (endKelvin - startKelvin) * progress) / 50,
          ) * 50,
          brightness: Math.round(
            startBrightness +
              (endBrightness - startBrightness) * progress,
          ),
        };
      }
    }
    const last = points[points.length - 1];
    return { kelvin: last[1], brightness: last[2] };
  }

  function circadianBrightness(kelvin) {
    const progress = Math.min(
      1,
      Math.max(0, (kelvin - 2200) / (5600 - 2200)),
    );
    return Math.round(60 + progress * 840);
  }

  function currentCircadianPoint(date = new Date()) {
    const points = circadianConfig.points;
    const hour = date.getHours() + date.getMinutes() / 60;
    let index = points.length - 1;
    for (let i = 0; i < points.length; i += 1) {
      if (hour >= points[i][0]) index = i;
    }
    return index;
  }

  function applyCircadianValues(kelvin, brightness, force = false) {
    const targetKelvin = Math.round(kelvin / 50) * 50;
    const targetBrightness =
      brightness === null ? null : Math.max(0, Math.min(1000, Math.round(brightness)));
    const changed =
      force ||
      state.mode !== 1 ||
      Math.abs(state.cct - targetKelvin) >= 25 ||
      (targetBrightness !== null &&
        Math.abs(state.brightness - targetBrightness) >= 10);
    if (!changed) {
      updateCircadianNote();
      return;
    }
    state.cct = targetKelvin;
    state.mode = 1;
    uiMode = "white";
    if (targetBrightness !== null) state.brightness = targetBrightness;
    if (ws && ws.readyState === WebSocket.OPEN) {
      send("set_mode", { mode: "cct" });
      send("set_color_temperature", { kelvin: targetKelvin });
      if (targetBrightness !== null) {
        send("set_brightness", { value: targetBrightness });
      }
    }
    render();
    updateCircadianNote();
  }

  function startCircadianRamp(point) {
    circadianRamp = {
      start: Date.now(),
      duration: circadianConfig.transitionSeconds * 1000,
      fromKelvin: state.cct,
      fromBrightness: state.brightness,
      toKelvin: point[1],
      toBrightness: circadianConfig.adjustBrightness ? point[2] : null,
    };
  }

  function applyCircadian() {
    if (!circadianEnabled || state.stale) return;
    if (circadianOverrideUntil > Date.now()) {
      updateCircadianNote();
      return;
    }
    if (circadianOverrideUntil !== 0) {
      circadianOverrideUntil = 0;
      try {
        localStorage.removeItem(CIRCADIAN_OVERRIDE_KEY);
      } catch {
        /* private mode */
      }
    }

    const now = new Date();
    if (circadianConfig.transition === "quick") {
      const index = currentCircadianPoint(now);
      if (circadianPointIndex !== index) {
        circadianPointIndex = index;
        startCircadianRamp(circadianConfig.points[index]);
      }
      if (circadianRamp) {
        const progress = Math.min(
          1,
          (Date.now() - circadianRamp.start) / circadianRamp.duration,
        );
        const kelvin =
          circadianRamp.fromKelvin +
          (circadianRamp.toKelvin - circadianRamp.fromKelvin) * progress;
        const brightness =
          circadianRamp.toBrightness === null
            ? null
            : circadianRamp.fromBrightness +
              (circadianRamp.toBrightness - circadianRamp.fromBrightness) *
                progress;
        applyCircadianValues(kelvin, brightness);
        if (progress >= 1) circadianRamp = null;
      }
      return;
    }

    circadianRamp = null;
    circadianPointIndex = null;
    const values = circadianValues(now);
    applyCircadianValues(
      values.kelvin,
      circadianConfig.adjustBrightness ? values.brightness : null,
    );
  }

  function clockTime(value) {
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "—";
    return date.toLocaleTimeString(document.documentElement.lang || "en", {
      hour: "2-digit",
      minute: "2-digit",
    });
  }

  function updateCircadianNote() {
    const note = $("circadian-note");
    const resume = $("circadian-resume");
    if (!circadianEnabled) {
      note.textContent = t("circadian.hint");
      resume.classList.add("hidden");
      return;
    }
    if (circadianOverrideUntil > Date.now()) {
      note.textContent = t("circadian.override", {
        time: clockTime(circadianOverrideUntil),
      });
      resume.classList.remove("hidden");
      return;
    }
    resume.classList.add("hidden");
    const values = circadianConfig.points.map((point) => point[1]);
    const key = circadianConfig.adjustBrightness
      ? "circadian.summaryAdaptive"
      : "circadian.summaryManual";
    const mode =
      circadianConfig.transition === "quick"
        ? t("circadian.modeQuick", {
            seconds: circadianConfig.transitionSeconds,
          })
        : t("circadian.modeGradual");
    note.textContent = `${t(key, {
      min: Math.min(...values),
      max: Math.max(...values),
    })} · ${mode}`;
  }

  function setCircadian(enabled, apply = true) {
    circadianEnabled = enabled;
    $("circadian").checked = enabled;
    try {
      localStorage.setItem(CIRCADIAN_KEY, enabled ? "1" : "0");
    } catch {
      /* private mode */
    }
    clearInterval(circadianTimer);
    circadianTimer = null;
    circadianRamp = null;
    circadianPointIndex = null;
    if (!enabled) {
      circadianOverrideUntil = 0;
      try {
        localStorage.removeItem(CIRCADIAN_OVERRIDE_KEY);
      } catch {
        /* private mode */
      }
      updateCircadianNote();
      return;
    }
    if (apply) {
      circadianOverrideUntil = 0;
      applyCircadian();
    }
    circadianTimer = setInterval(applyCircadian, 1000);
    updateCircadianNote();
  }

  function suspendCircadian() {
    if (!circadianEnabled) return;
    circadianOverrideUntil = Date.now() + 60 * 60 * 1000;
    circadianRamp = null;
    circadianPointIndex = null;
    try {
      localStorage.setItem(
        CIRCADIAN_OVERRIDE_KEY,
        String(circadianOverrideUntil),
      );
    } catch {
      /* private mode */
    }
    updateCircadianNote();
  }

  function appendCircadianPoint(hour, kelvin, brightness) {
    const row = document.createElement("div");
    row.className = "circadian-point";

    const timeInput = document.createElement("input");
    timeInput.type = "time";
    timeInput.className = "circadian-time";
    timeInput.value = `${String(Math.floor(hour)).padStart(2, "0")}:${String(
      Math.round((hour % 1) * 60),
    ).padStart(2, "0")}`;
    timeInput.setAttribute("aria-label", t("circadian.time"));

    const swatch = document.createElement("span");
    swatch.className = "circadian-swatch";
    const [r, g, b] = kelvinToRgb(kelvin);
    swatch.style.background = `rgb(${r} ${g} ${b})`;

    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "secondary seq-mini circadian-remove";
    remove.textContent = "×";
    remove.title = t("circadian.remove");
    remove.setAttribute("aria-label", t("circadian.remove"));
    remove.addEventListener("click", () => row.remove());

    const temperatureLabel = document.createElement("label");
    temperatureLabel.className = "circadian-temp-row";
    const temperatureText = document.createElement("span");
    temperatureText.textContent = t("circadian.temperature");
    const temperatureOutput = document.createElement("output");
    temperatureOutput.textContent = `${kelvin} K`;
    temperatureText.appendChild(temperatureOutput);
    const temperature = document.createElement("input");
    temperature.type = "range";
    temperature.className = "cct-range";
    temperature.min = "2000";
    temperature.max = "6500";
    temperature.step = "50";
    temperature.value = String(kelvin);
    temperature.addEventListener("input", () => {
      const value = Number(temperature.value);
      temperatureOutput.textContent = `${value} K`;
      const [vr, vg, vb] = kelvinToRgb(value);
      swatch.style.background = `rgb(${vr} ${vg} ${vb})`;
    });
    temperatureLabel.append(temperatureText, temperature);

    const brightnessLabel = document.createElement("label");
    brightnessLabel.className = "circadian-brightness-row";
    const brightnessText = document.createElement("span");
    brightnessText.textContent = t("circadian.brightness");
    const brightnessOutput = document.createElement("output");
    brightnessOutput.textContent = String(brightness);
    brightnessText.appendChild(brightnessOutput);
    const brightnessInput = document.createElement("input");
    brightnessInput.type = "range";
    brightnessInput.min = "0";
    brightnessInput.max = "1000";
    brightnessInput.step = "10";
    brightnessInput.value = String(brightness);
    brightnessInput.addEventListener("input", () => {
      brightnessOutput.textContent = brightnessInput.value;
    });
    brightnessLabel.append(brightnessText, brightnessInput);

    row.append(timeInput, swatch, temperatureLabel, brightnessLabel, remove);
    $("circadian-points").appendChild(row);
  }

  function updateCircadianBrightnessRows() {
    const enabled = $("circadian-brightness").checked;
    document.querySelectorAll(".circadian-brightness-row").forEach((row) => {
      row.classList.toggle("hidden", !enabled);
    });
  }

  function renderCircadianPoints() {
    $("circadian-points").textContent = "";
    circadianConfig.points.forEach(([hour, kelvin, brightness]) => {
      appendCircadianPoint(hour, kelvin, brightness);
    });
    $("circadian-brightness").checked = circadianConfig.adjustBrightness;
    $("circadian-transition-mode").value = circadianConfig.transition;
    $("circadian-transition-seconds").value = String(
      circadianConfig.transitionSeconds,
    );
    $("circadian-seconds-wrap").classList.toggle(
      "hidden",
      circadianConfig.transition !== "quick",
    );
    $("circadian-error").textContent = "";
    updateCircadianBrightnessRows();
  }

  function openCircadianDialog() {
    renderCircadianPoints();
    const dialog = $("circadian-dialog");
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
  }

  function closeCircadianDialog() {
    const dialog = $("circadian-dialog");
    if (typeof dialog.close === "function") dialog.close();
    else dialog.removeAttribute("open");
  }

  function saveCircadianDialog() {
    const rows = [...document.querySelectorAll(".circadian-point")];
    const points = [];
    for (const row of rows) {
      const time = row.querySelector('input[type="time"]').value;
      const temperature = Number(
        row.querySelector('input[type="range"].cct-range').value,
      );
      const brightness = Number(
        row.querySelector(".circadian-brightness-row input").value,
      );
      if (!time || !Number.isFinite(temperature)) continue;
      const [hours, minutes] = time.split(":").map(Number);
      points.push([hours + minutes / 60, temperature, brightness]);
    }
    points.sort((a, b) => a[0] - b[0]);
    if (
      points.length < 2 ||
      points.some(
        (point) =>
          point[1] < 2000 || point[1] > 6500 || point[2] < 0 || point[2] > 1000,
      )
    ) {
      $("circadian-error").textContent = t("circadian.invalid");
      return;
    }
    circadianConfig = {
      points,
      adjustBrightness: $("circadian-brightness").checked,
      transition:
        $("circadian-transition-mode").value === "quick" ? "quick" : "gradual",
      transitionSeconds: Math.min(
        60,
        Math.max(1, Number($("circadian-transition-seconds").value) || 5),
      ),
    };
    saveCircadianConfig();
    closeCircadianDialog();
    circadianRamp = null;
    circadianPointIndex = null;
    if (circadianEnabled) applyCircadian();
    updateCircadianNote();
  }

  function resetCircadianDialog() {
    circadianConfig = {
      points: DEFAULT_CIRCADIAN_POINTS.map((point) => [...point]),
      adjustBrightness: false,
      transition: "gradual",
      transitionSeconds: 5,
    };
    renderCircadianPoints();
  }

  function pairDialog() {
    return $("pair-dialog");
  }

  function openManageDialog(id) {
    const dialog = $(id);
    if (!dialog) return;
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
  }

  function closeManageDialog(id) {
    const dialog = $(id);
    if (!dialog) return;
    if (typeof dialog.close === "function") dialog.close();
    else dialog.removeAttribute("open");
  }

  function bindManageDialog(id) {
    const dialog = $(id);
    if (!dialog) return;
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) closeManageDialog(id);
    });
  }

  function renderPairing(info, device) {
    const hint = $("pair-hint");
    const scan = $("pair-scan");
    const wifi = $("pair-wifi");
    const connect = $("pair-connect");
    const status = $("pair-status");
    scan.classList.add("hidden");
    wifi.classList.add("hidden");
    connect.classList.add("hidden");
    connect.disabled = false;

    if (info.state === "online") {
      hint.textContent = t("pair.online", { ssid: info.ssid || "" });
      status.textContent = t("pair.connected", { ip: info.ip || "—" });
      return;
    }
    if (info.state === "connecting") {
      hint.textContent = t("pair.connecting", { ssid: info.ssid || "" });
      status.textContent = "";
      return;
    }
    if (info.state === "pairing") {
      if (!device) {
        hint.textContent = t("pair.noDevices");
        return;
      }
      hint.textContent = t("pair.found", {
        name: device.name,
        rssi: device.rssi,
      });
      const card = $("pair-device-card");
      card.textContent = "";
      const name = document.createElement("strong");
      name.textContent = device.name;
      const rssi = document.createElement("span");
      rssi.className = "rssi";
      rssi.textContent = `${device.rssi} dBm`;
      card.append(name, rssi);
      scan.classList.remove("hidden");
      wifi.classList.remove("hidden");
      connect.classList.remove("hidden");
      status.textContent =
        pairError ||
        (info.ssid ? t("pair.current", { ssid: info.ssid }) : "");
      return;
    }
    hint.textContent = t("pair.hint");
    status.textContent =
      info.cycles > 0
        ? t("pair.cycles", {
            count: info.cycles,
            required: info.cycles_required || 3,
          })
        : "";
  }

  async function refreshPairingPolicy() {
    if (!instance) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/provisioning/policy`,
        { headers: authHeaders() },
      );
      if (!response.ok) return;
      const data = await response.json();
      pairPolicyPasswords = data.passwords || {};
      const list = $("pair-networks");
      list.textContent = "";
      (data.networks || []).forEach((ssid) => {
        const option = document.createElement("option");
        option.value = ssid;
        list.appendChild(option);
      });
      const note = $("pair-networks-note");
      const entries = (data.networks || []).map((ssid) => {
        const password = (data.passwords || {})[ssid];
        return password
          ? `${ssid} / ${password}`
          : `${ssid} (${t("pair.anyPassword")})`;
      });
      if (data.allow_any) {
        note.textContent = entries.length
          ? t("pair.anyNetworkCreds", { networks: entries.join(", ") })
          : t("pair.anyNetwork");
      } else if (entries.length) {
        note.textContent = t("pair.allowed", {
          networks: entries.join(", "),
        });
      } else {
        note.textContent = t("pair.noNetworks");
      }
    } catch {
      /* retried when the dialog opens */
    }
  }

  async function refreshPairing() {
    if (!instance) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/provisioning`,
        { headers: authHeaders() },
      );
      if (!response.ok) return;
      const info = await response.json();
      let device = null;
      if (info.state === "pairing") {
        const scan = await fetch(
          `${API}/api/v1/instances/${instance.id}/provisioning/scan`,
          { headers: authHeaders() },
        );
        if (scan.ok) {
          const data = await scan.json();
          device = (data.devices || [])[0] || null;
        }
      }
      renderPairing(info, device);
    } catch {
      /* retried by polling */
    }
  }

  function openPairDialog() {
    const dialog = pairDialog();
    if (!dialog) return;
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
    pairError = "";
    clearInterval(pairingPoll);
    refreshPairing();
    refreshPairingPolicy();
    pairingPoll = setInterval(refreshPairing, 2000);
  }

  function closePairDialog() {
    const dialog = pairDialog();
    clearInterval(pairingPoll);
    pairingPoll = null;
    pairError = "";
    $("pair-password").value = "";
    if (!dialog) return;
    if (typeof dialog.close === "function") dialog.close();
    else dialog.removeAttribute("open");
  }

  async function connectPairing() {
    if (!instance) return;
    const ssid = $("pair-ssid").value.trim();
    const password = $("pair-password").value;
    if (!ssid || !password) {
      $("pair-status").textContent = t("pair.error", {
        error: "SSID/password",
      });
      return;
    }
    pairError = "";
    $("pair-connect").disabled = true;
    $("pair-status").textContent = t("pair.connecting", { ssid });
    const response = await fetch(
      `${API}/api/v1/instances/${instance.id}/provisioning/connect`,
      {
        method: "POST",
        headers: { ...authHeaders(), "Content-Type": "application/json" },
        body: JSON.stringify({ ssid, password }),
      },
    );
    const data = await response.json().catch(() => ({}));
    $("pair-password").value = "";
    if (!response.ok) {
      const code = data.error?.code;
      const keys = {
        UNKNOWN_NETWORK: "pair.errorUnknownNetwork",
        INVALID_PASSWORD: "pair.errorWrongPassword",
        WIFI_TIMEOUT: "pair.errorTimeout",
        INVALID_NETWORK: "pair.errorInvalidNetwork",
      };
      pairError = keys[code]
        ? t(keys[code])
        : t("pair.error", {
            error: data.error?.message || response.status,
          });
      $("pair-status").textContent = pairError;
      $("pair-connect").disabled = false;
      return;
    }
    if (data.state === "online") {
      pairError = "";
      $("pair-hint").textContent = t("pair.online", { ssid: data.ssid });
      $("pair-status").textContent = t("pair.connected", { ip: data.ip });
      $("pair-scan").classList.add("hidden");
      $("pair-wifi").classList.add("hidden");
      $("pair-connect").classList.add("hidden");
    } else {
      await refreshPairing();
    }
    await refreshProvisioning();
  }

  function scheduleReconnect() {
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(async () => {
      try {
        await ensureInstance();
        await refreshProvisioning();
        await refreshPowerOn();
        loadSchedules().catch(() => {});
        connect();
      } catch {
        setBackendDown(true);
        reconnectTimer = setTimeout(scheduleReconnect, reconnectDelay);
      }
    }, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 10000);
  }

  async function connect() {
    if (!instance) return;
    let ticket;
    try {
      ticket = await window.LampConfig.wsTicket(instance, authHeaders());
    } catch {
      setBackendDown(true);
      scheduleReconnect();
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
      setBackendDown(false);
      syncDashboard().catch(() => {});
    };

    ws.onmessage = (event) => {
      try {
        handleMessage(JSON.parse(event.data));
      } catch {
        /* ignore non-JSON */
      }
    };

    ws.onclose = (event) => {
      setBackendDown(true);
      if (event.code === 1008) {
        sessionStorage.removeItem("lamp_instance");
        instance = null;
      }
      scheduleReconnect();
    };

    ws.onerror = () => {
      /* onclose handles the retry */
    };
  }

  power.addEventListener("click", () => {
    if (isLocked()) return;
    const turnOn = !state.power;
    if (turnOn) {
      send("set_power", { on: true });
      if (state.brightness === 0) {
        state.brightness = 800;
        send("set_brightness", { value: 800 });
      }
      if (state.mode === 1) {
        send("set_color_temperature", { kelvin: state.cct });
      } else if (state.v === 0) {
        state.v = 800;
        send("set_color", { h: state.h, s: state.s, v: 800 });
      }
    } else {
      send("set_power", { on: false });
    }
    state.power = turnOn;
    if (!turnOn) activeEffect = null;
    render();
  });

  brightness.addEventListener("input", (event) => {
    if (circadianEnabled && circadianConfig.adjustBrightness) {
      suspendCircadian();
    }
    state.brightness = Number(event.target.value);
    render();
    sendThrottled("set_brightness", { value: state.brightness });
  });

  cct.addEventListener("input", (event) => {
    suspendCircadian();
    activeEffect = null;
    state.cct = Number(event.target.value);
    state.mode = 1;
    render();
    sendThrottled("set_color_temperature", { kelvin: state.cct });
  });

  document.querySelectorAll(".mode-btn").forEach((button) => {
    button.addEventListener("click", () => setUiMode(button.dataset.mode));
  });

  $("stop").addEventListener("click", () => {
    if (state.stale) return;
    send("stop_sequence", {});
    activeEffect = null;
    setLast(t("effect.stopped"));
  });

  async function resetSchedules() {
    if (!instance || !instance.id || !schedules.length) return;
    if (!window.confirm(t("schedules.confirmReset"))) return;
    try {
      for (const schedule of [...schedules]) {
        const response = await fetch(
          `${API}/api/v1/instances/${instance.id}/schedules/${schedule.id}`,
          { method: "DELETE", headers: authHeaders() },
        );
        if (!response.ok && response.status !== 404) {
          throw new Error(`schedules/${schedule.id} -> ${response.status}`);
        }
      }
      schedules = [];
      renderSchedules();
      setStatusSchedule(t("schedules.resetDone"));
    } catch (error) {
      setStatusSchedule(t("schedules.error", { error: error.message }));
    }
  }

  function resetSequence() {
    if (!window.confirm(t("sequences.confirmReset"))) return;
    sequence = [];
    sequenceSettings = { fade_ms: 800, speed: 1000 };
    saveSequence();
    saveSequenceSettings();
    $("seq-fade").value = "0.8";
    $("seq-speed").value = "1000";
    renderSequence();
    updateSequenceControls();
    setSequenceStatus(t("sequences.resetDone"));
  }

  $("schedules-reset").addEventListener("click", () => {
    resetSchedules().catch(() => {});
  });
  $("seq-reset").addEventListener("click", resetSequence);

  function setSettingsStatus(text) {
    const el = $("settings-status");
    if (el) el.textContent = text;
  }

  function clearLocalDashboardConfig() {
    sequence = [];
    sequenceSettings = { fade_ms: 800, speed: 1000 };
    saveSequence();
    saveSequenceSettings();
    $("seq-fade").value = "0.8";
    $("seq-speed").value = "1000";
    renderSequence();
    updateSequenceControls();

    circadianConfig = {
      points: DEFAULT_CIRCADIAN_POINTS.map((point) => [...point]),
      adjustBrightness: false,
      transition: "gradual",
      transitionSeconds: 5,
    };
    circadianEnabled = false;
    circadianRamp = null;
    circadianPointIndex = null;
    $("circadian").checked = false;
    saveCircadianConfig();
    try {
      localStorage.removeItem(CIRCADIAN_OVERRIDE_KEY);
    } catch {
      /* private mode */
    }
    updateCircadianNote();

    stopParty(true);
    party.source = "mic";
    $("party-source").value = "mic";
    try {
      localStorage.setItem(PARTY_SOURCE_KEY, "mic");
    } catch {
      /* private mode */
    }
    updatePartyControls();
    render();
  }

  async function syncDashboard() {
    if (!instance || !instance.id) return;
    await Promise.allSettled([
      refreshState(),
      refreshHealth(),
      refreshProvisioning(),
      refreshPowerOn(),
      loadSchedules(),
    ]);
  }

  async function resetDashboardSettings() {
    if (!window.confirm(t("settings.confirmReset"))) return;
    try {
      clearLocalDashboardConfig();
      if (instance && instance.id) {
        for (const schedule of [...schedules]) {
          const response = await fetch(
            `${API}/api/v1/instances/${instance.id}/schedules/${schedule.id}`,
            { method: "DELETE", headers: authHeaders() },
          );
          if (!response.ok && response.status !== 404) {
            throw new Error(`schedules/${schedule.id} -> ${response.status}`);
          }
        }
        schedules = [];
        renderSchedules();
        await fetch(`${API}/api/v1/instances/${instance.id}/power-on`, {
          method: "PUT",
          headers: { ...authHeaders(), "Content-Type": "application/json" },
          body: JSON.stringify({ mode: "default" }),
        });
        await fetch(`${API}/api/v1/instances/${instance.id}/faults/history`, {
          method: "DELETE",
          headers: authHeaders(),
        });
        await syncDashboard();
      }
      setSettingsStatus(t("settings.resetDone"));
    } catch (error) {
      setSettingsStatus(t("settings.error", { error: error.message }));
    }
  }

  async function clearFaultHistoryFromSettings() {
    if (!window.confirm(t("settings.confirmHistory"))) return;
    if (!instance || !instance.id) return;
    try {
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/faults/history`,
        { method: "DELETE", headers: authHeaders() },
      );
      if (!response.ok) throw new Error(`faults/history -> ${response.status}`);
      setSettingsStatus(t("settings.historyCleared"));
    } catch (error) {
      setSettingsStatus(t("settings.error", { error: error.message }));
    }
  }

  $("schedule-add").addEventListener("click", () => {
    submitSchedule().catch(() => {
      setStatusSchedule(t("schedules.error", { error: "backend" }));
    });
  });
  $("schedule-cancel").addEventListener("click", resetScheduleForm);
  $("schedule-repeat").addEventListener("change", renderScheduleFields);
  $("schedule-color-button").addEventListener(
    "click",
    openScheduleColorPicker,
  );
  document.querySelectorAll("[data-schedule-action]").forEach((button) => {
    button.addEventListener("click", () =>
      setScheduleAction(button.dataset.scheduleAction),
    );
  });
  $("schedule-scene").addEventListener("change", updateScheduleScene);
  $("schedule-effect").addEventListener("change", updateScheduleEffect);
  $("schedule-effect-speed").addEventListener(
    "change",
    updateScheduleEffect,
  );
  $("schedule-sunrise-duration").addEventListener(
    "change",
    updateScheduleSunrise,
  );
  $("circadian").addEventListener("change", () => {
    setCircadian($("circadian").checked);
  });
  $("power-on-mode").addEventListener("change", () => {
    powerOn.mode = $("power-on-mode").value;
    if (powerOn.mode === "custom" && !powerOn.state) {
      powerOn.state = currentPowerOnState();
    }
    savePowerOn().catch((error) => {
      $("power-on-detail").textContent = t("powerOn.saveError", {
        error: error.message,
      });
    });
  });
  $("power-on-choose").addEventListener("click", openPowerOnPicker);
  $("fault-clear").addEventListener("click", () => {
    clearFaults().catch(() => {});
  });

  $("schedules-manage").addEventListener("click", () => {
    openManageDialog("schedules-dialog");
    syncScheduleBuilder(scheduleDraft);
    loadSchedules().catch(() => {});
  });
  $("schedules-close").addEventListener("click", () =>
    closeManageDialog("schedules-dialog"),
  );
  $("sequences-manage").addEventListener("click", () =>
    openManageDialog("sequences-dialog"),
  );
  $("sequences-close").addEventListener("click", () =>
    closeManageDialog("sequences-dialog"),
  );
  bindManageDialog("schedules-dialog");
  bindManageDialog("sequences-dialog");
  $("settings-open").addEventListener("click", () =>
    openManageDialog("settings-dialog"),
  );
  $("settings-close").addEventListener("click", () =>
    closeManageDialog("settings-dialog"),
  );
  bindManageDialog("settings-dialog");
  $("settings-reset").addEventListener("click", () => {
    resetDashboardSettings().catch(() => {});
  });
  $("settings-clear-history").addEventListener("click", () => {
    clearFaultHistoryFromSettings().catch(() => {});
  });

  $("pair-device").addEventListener("click", openPairDialog);
  $("pair-ssid").addEventListener("change", () => {
    const password = pairPolicyPasswords[$("pair-ssid").value.trim()];
    if (!password) return;
    const pass = $("pair-password");
    pass.value = password;
    pass.focus();
    pass.select();
  });
  $("pair-close").addEventListener("click", closePairDialog);
  $("pair-connect").addEventListener("click", () => {
    connectPairing().catch(() => {
      pairError = t("pair.error", { error: "backend" });
      $("pair-status").textContent = pairError;
      $("pair-connect").disabled = false;
    });
  });
  pairDialog().addEventListener("close", () => {
    clearInterval(pairingPoll);
    pairingPoll = null;
    pairError = "";
    $("pair-password").value = "";
  });
  $("circadian-config").addEventListener("click", openCircadianDialog);
  $("circadian-close").addEventListener("click", closeCircadianDialog);
  $("circadian-cancel").addEventListener("click", closeCircadianDialog);
  $("circadian-save").addEventListener("click", saveCircadianDialog);
  $("circadian-reset").addEventListener("click", resetCircadianDialog);
  $("circadian-add-point").addEventListener("click", () => {
    appendCircadianPoint(12, 4000, 500);
    updateCircadianBrightnessRows();
  });
  $("circadian-brightness").addEventListener(
    "change",
    updateCircadianBrightnessRows,
  );
  $("circadian-transition-mode").addEventListener("change", () => {
    $("circadian-seconds-wrap").classList.toggle(
      "hidden",
      $("circadian-transition-mode").value !== "quick",
    );
  });
  $("circadian-resume").addEventListener("click", () => {
    circadianOverrideUntil = 0;
    circadianRamp = null;
    circadianPointIndex = null;
    try {
      localStorage.removeItem(CIRCADIAN_OVERRIDE_KEY);
    } catch {
      /* private mode */
    }
    applyCircadian();
    updateCircadianNote();
  });

  $("party-toggle").addEventListener("click", () => {
    if (party.active) {
      stopParty();
    } else {
      startParty().catch((error) => {
        stopParty(true);
        setPartyStatus(t("party.error", { error: error.message || "audio" }));
      });
    }
  });
  $("party-source").addEventListener("change", () => {
    try {
      localStorage.setItem(PARTY_SOURCE_KEY, $("party-source").value);
    } catch {
      /* private mode */
    }
  });

  $("seq-fade").addEventListener("change", () => {
    sequenceSettings.fade_ms = Math.round(
      Math.min(3, Math.max(0, Number($("seq-fade").value) || 0)) * 1000,
    );
    $("seq-fade").value = String(sequenceSettings.fade_ms / 1000);
    saveSequenceSettings();
  });
  $("seq-speed").addEventListener("change", () => {
    sequenceSettings.speed = Number($("seq-speed").value) || 1000;
    saveSequenceSettings();
  });

  $("seq-add").addEventListener("click", addCurrentColor);
  $("seq-start").addEventListener("click", () => {
    startSequence().catch(() => {
      setSequenceStatus(t("sequences.error", { error: "backend" }));
    });
  });
  $("seq-stop").addEventListener("click", stopSequence);

  $("update").addEventListener("click", async () => {
    if (!instance || !instance.id) {
      setLast(t("ota.noInstance"));
      return;
    }
    setLast(t("ota.checking"));
    try {
      const latestResponse = await fetch(`${API}/api/v1/ota/latest`);
      const latest = await latestResponse.json();
      if (!latest.version) {
        setLast(t("ota.noArtifacts"));
        return;
      }
      if (state.fwVersion && latest.version === state.fwVersion) {
        setLast(t("ota.upToDate", { version: state.fwVersion }));
        return;
      }
      setLast(t("ota.installing", { version: latest.version }));
      const response = await fetch(
        `${API}/api/v1/instances/${instance.id}/ota`,
        {
          method: "POST",
          headers: { ...authHeaders(), "Content-Type": "application/json" },
          body: JSON.stringify({ version: latest.version }),
        },
      );
      const data = await response.json();
      if (data.type === "ack") {
        setLast(t("ota.accepted"));
      } else {
        setLast(
          t("ota.rejected", {
            code: data.payload?.code || data.type || response.status,
          }),
        );
      }
    } catch {
      setLast(t("ota.backendDown"));
    }
  });

  setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: "heartbeat" }));
    }
  }, 30000);

  setInterval(refreshHealth, 15000);
  setInterval(refreshProvisioning, 10000);

  window.addEventListener("lamp:lang", () => {
    document.title = t("page.title");
    setConn(connKey, connClass);
    renderNetwork();
    renderFault();
    renderPowerOn();
    renderPresets();
    renderScenes();
    renderEffects();
    renderSequence();
    renderSchedules();
    updateScheduleZone();
    renderSchedulePreview();
    renderScheduleFields();
    updateCircadianNote();
    if (pairDialog() && pairDialog().open) {
      refreshPairing();
      refreshPairingPolicy();
    }
    if ($("circadian-dialog").hasAttribute("open")) {
      renderCircadianPoints();
    }
    updateSequenceControls();
    updatePartyControls();
    if (party.active) {
      setPartyStatus(
        party.source === "mic"
          ? t("party.runningMic", { level: Math.round(party.level * 100) })
          : party.fallback
            ? t("party.runningFallback")
            : t("party.runningAuto"),
      );
    } else {
      setPartyStatus(t("party.hint"));
    }
    render();
  });

  document.title = t("page.title");
  sequence = loadSequence();
  loadSequenceSettings();
  $("seq-fade").value = String(sequenceSettings.fade_ms / 1000);
  $("seq-speed").value = String(sequenceSettings.speed);
  try {
    const source = localStorage.getItem(PARTY_SOURCE_KEY);
    if (source === "auto" || source === "mic") {
      $("party-source").value = source;
    }
    circadianEnabled = localStorage.getItem(CIRCADIAN_KEY) === "1";
  } catch {
    /* default value */
  }
  loadCircadianConfig();
  $("circadian").checked = circadianEnabled;
  if (circadianEnabled) {
    setCircadian(true);
  } else {
    updateCircadianNote();
  }
  renderScheduleFields();
  renderPresets();
  renderScenes();
  renderEffects();
  renderSequence();
  render();

  function assistantClamp(value, minimum, maximum) {
    return Math.max(minimum, Math.min(maximum, Math.round(Number(value) || 0)));
  }

  function assistantSetPower(on) {
    if (isLocked()) return { ok: false, locked: true };
    if (Boolean(on) === state.power) return { ok: true };
    if (on) {
      send("set_power", { on: true });
      if (state.brightness === 0) {
        state.brightness = 800;
        send("set_brightness", { value: 800 });
      }
      if (state.mode === 1) {
        send("set_color_temperature", { kelvin: state.cct });
      } else if (state.v === 0) {
        state.v = 800;
        send("set_color", { h: state.h, s: state.s, v: 800 });
      }
      state.power = true;
    } else {
      send("set_power", { on: false });
      state.power = false;
      activeEffect = null;
    }
    render();
    return { ok: true };
  }

  function assistantSetBrightness(value) {
    if (isLocked()) return { ok: false, locked: true };
    const brightness = assistantClamp(value, 0, 1000);
    state.brightness = brightness;
    if (brightness > 0 && !state.power) {
      state.power = true;
      send("set_power", { on: true });
    }
    send("set_brightness", { value: brightness });
    render();
    return { ok: true, brightness };
  }

  function assistantSetCct(value) {
    if (isLocked()) return { ok: false, locked: true };
    const cct = assistantClamp(value, 2000, 6500);
    state.mode = 1;
    state.cct = cct;
    if (!state.power) send("set_power", { on: true });
    state.power = true;
    setUiMode("white", false);
    send("set_mode", { mode: "cct" });
    send("set_color_temperature", { kelvin: cct });
    render();
    return { ok: true, cct };
  }

  function assistantSetColor(hue, saturation) {
    if (isLocked()) return { ok: false, locked: true };
    const h = assistantClamp(hue, 0, 360);
    const s = assistantClamp(saturation, 0, 1000);
    state.mode = 0;
    state.h = h;
    state.s = s;
    state.v = 1000;
    if (!state.power) send("set_power", { on: true });
    state.power = true;
    setUiMode("color", false);
    send("set_mode", { mode: "manual" });
    send("set_color", { h, s, v: 1000 });
    render();
    return { ok: true, h, s };
  }

  function assistantApplyScene(key) {
    if (isLocked()) return { ok: false, locked: true };
    const scene = SCENES.find((item) => item.key === `scene.${key}`);
    if (!scene) return { ok: false, unknown: true };
    state.mode = 1;
    state.cct = scene.cct;
    state.brightness = scene.brightness;
    state.power = true;
    setUiMode("white", false);
    send("set_power", { on: true });
    send("set_mode", { mode: "cct" });
    send("set_color_temperature", { kelvin: scene.cct });
    send("set_brightness", { value: scene.brightness });
    setLast(t(scene.key));
    render();
    return { ok: true, scene: key, cct: scene.cct, brightness: scene.brightness };
  }

  function assistantStartEffect(id) {
    if (isLocked()) return { ok: false, locked: true };
    const effect = EFFECTS.find((item) => item.id === Number(id));
    if (!effect) return { ok: false, unknown: true };
    state.mode = 2;
    state.power = true;
    activeEffect = effect;
    send("start_sequence", {
      id: effect.id,
      speed: 1000,
      direction: 0,
      loop: 0,
    });
    setLast(t(effect.key));
    render();
    return { ok: true, effect: effect.id };
  }

  function assistantStopEffect() {
    if (isLocked()) return { ok: false, locked: true };
    send("stop_sequence", {});
    activeEffect = null;
    state.mode = 0;
    render();
    return { ok: true };
  }

  function assistantOpen(id) {
    if (id === "help") {
      const button = document.querySelector("[data-help-open]");
      if (button) button.click();
    } else if (id === "settings") {
      $("settings-open").click();
    } else if (id === "schedules") {
      $("schedules-manage").click();
    } else if (id === "sequences") {
      $("sequences-manage").click();
    }
    return { ok: true };
  }

  function assistantContext() {
    const lockReasons = [];
    if (state.stale) lockReasons.push("stale");
    if (network.state !== "online") lockReasons.push(`network:${network.state}`);
    if ([2, 3, 4, 5, 7].includes(Number(state.faultCode))) {
      lockReasons.push(`fault:${state.faultCode}`);
    }
    return {
      state: {
        power: state.power,
        brightness: state.brightness,
        h: state.h,
        s: state.s,
        v: state.v,
        cct: state.cct,
        mode: state.mode,
        stale: state.stale,
        faultCode: state.faultCode,
      },
      network: {
        state: network.state,
        ssid: network.ssid,
      },
      health: lastHealth
        ? {
            fw_version: lastHealth.fw_version || null,
            slot: lastHealth.slot || null,
            led_temp_c: lastHealth.led_temp_c ?? null,
          }
        : null,
      schedules: schedules.length,
      sequence: sequence.length,
      powerOn: { mode: powerOn.mode },
      locked: isLocked(),
      lockReasons,
    };
  }

  window.LampAssistantAPI = {
    context: assistantContext,
    actions: {
      setPower: assistantSetPower,
      setBrightness: assistantSetBrightness,
      setCct: assistantSetCct,
      setColor: assistantSetColor,
      applyScene: assistantApplyScene,
      startEffect: assistantStartEffect,
      stopEffect: assistantStopEffect,
      clearFaults,
      open: assistantOpen,
      setMode: (mode) => setUiMode(mode),
    },
  };

  setConn("conn.deviceOffline", "offline");
  render();

  (async () => {
    try {
      await ensureInstance();
      await refreshProvisioning();
      await refreshPowerOn();
      loadSchedules().catch(() => {});
      connect();
    } catch {
      setBackendDown(true);
      scheduleReconnect();
    }
  })();
})();
