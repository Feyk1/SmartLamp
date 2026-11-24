/* Smart Lamp — rule-based, client-side assistant.
 *
 * No model, no API key and no network request for reasoning. The knowledge
 * base is generated from docs/manual; actions call the same UI/WS helpers as
 * the dashboard, after an explicit confirmation.
 */
(() => {
  "use strict";

  const COPY = {
    en: {
      button: "Ask",
      title: "Lamp assistant",
      close: "Close",
      placeholder: "Ask about the lamp…",
      send: "Send",
      disclaimer:
        "Rules-based assistant. No AI API; everything runs in this browser.",
      greeting:
        "Hi. I can explain the app, read the current state and apply safe light actions.",
      apply: "Apply",
      cancel: "Cancel",
      done: "Done.",
      cancelled: "Cancelled.",
      empty: "Type a question or choose a suggestion.",
      fallback:
        "I don't have a rule for that yet. Try a suggestion or ask about pairing, faults, OTA, schedules or sequences.",
      source: "From the manual: {page} — {title}",
      manualEnglish: "The manual source is in English.",
      confirm: "I can do that. Confirm to apply it.",
      noFault: "There is no active fault.",
      unlocked: "The controls are available.",
      status:
        "Power: {power} · brightness {brightness} · {color} · fault {fault}.",
      version: "Firmware {version} · slot {slot}.",
      schedules: "{count} schedules stored in the backend.",
      sequence: "{count} sequence steps in this browser.",
      locked: "The controls are locked: {reason}",
      stale:
        "the dashboard lost the device connection. Wait for `device connected`.",
      network:
        "Wi-Fi is not online (`{state}`). Finish pairing or use Add device.",
      faultLocked:
        "a critical fault is latched (code {code}). Clear faults, then turn the lamp on.",
      heartbeat:
        "heartbeat is lost (code 7); the output is off. Wait for the firmware to return or clear faults.",
      safeMode:
        "configuration safe mode is active (code 5). Clear faults and save a valid configuration.",
      faultLabel: "Fault {code}: {label}",
      faultNone: "No active fault.",
      opened: "Opened {target}.",
      plan: {
        powerOn: "Turn on the lamp",
        powerOff: "Turn off the lamp",
        brightness: "Set brightness to {value}",
        cct: "Set white temperature to {value} K",
        color: "Set color to {name}",
        scene: "Apply the {name} scene",
        effect: "Start the {name} effect",
        stop: "Stop the effect",
        clear: "Clear faults",
      },
      doneMessage: {
        powerOn: "Lamp on.",
        powerOff: "Lamp off.",
        brightness: "Brightness set to {value}.",
        cct: "White temperature set to {value} K.",
        color: "Color set to {name}.",
        scene: "Scene {name} applied.",
        effect: "Effect {name} started.",
        stop: "Effect stopped.",
        clear: "Faults cleared. Turn the lamp on when ready.",
      },
      suggestions: [
        "Why is it locked?",
        "Set brightness to 40%",
        "Start Reading",
        "How do I pair?",
        "OTA and slots",
        "Any faults?",
      ],
    },
    es: {
      button: "Preguntar",
      title: "Asistente de la lámpara",
      close: "Cerrar",
      placeholder: "Pregunta sobre la lámpara…",
      send: "Enviar",
      disclaimer:
        "Asistente basado en reglas. Sin API de IA; todo ocurre en este navegador.",
      greeting:
        "Hola. Puedo explicar la app, leer el estado actual y aplicar acciones seguras de luz.",
      apply: "Aplicar",
      cancel: "Cancelar",
      done: "Hecho.",
      cancelled: "Cancelado.",
      empty: "Escribe una pregunta o elige una sugerencia.",
      fallback:
        "Todavía no tengo una regla para eso. Prueba una sugerencia o pregunta por pairing, fallas, OTA, horarios o secuencias.",
      source: "Del manual: {page} — {title}",
      manualEnglish: "La fuente del manual está en inglés.",
      confirm: "Puedo hacerlo. Confirma para aplicarlo.",
      noFault: "No hay ninguna falla activa.",
      unlocked: "Los controles están disponibles.",
      status:
        "Encendido: {power} · brillo {brightness} · {color} · falla {fault}.",
      version: "Firmware {version} · slot {slot}.",
      schedules: "{count} horarios guardados en el backend.",
      sequence: "{count} pasos de secuencia en este navegador.",
      locked: "Los controles están bloqueados: {reason}",
      stale:
        "el dashboard perdió la conexión con el dispositivo. Espera a `device connected`.",
      network:
        "Wi-Fi no está online (`{state}`). Termina el pairing o usa Add device.",
      faultLocked:
        "hay una falla crítica latchada (código {code}). Limpia las fallas y vuelve a encender.",
      heartbeat:
        "se perdió el heartbeat (código 7); la salida está apagada. Espera a que vuelva el firmware o limpia las fallas.",
      safeMode:
        "está activo el modo seguro de configuración (código 5). Limpia las fallas y guarda una configuración válida.",
      faultLabel: "Falla {code}: {label}",
      faultNone: "No hay falla activa.",
      opened: "Abierto: {target}.",
      plan: {
        powerOn: "Encender la lámpara",
        powerOff: "Apagar la lámpara",
        brightness: "Poner el brillo al {value}",
        cct: "Poner el blanco a {value} K",
        color: "Poner el color {name}",
        scene: "Aplicar la escena {name}",
        effect: "Iniciar el efecto {name}",
        stop: "Detener el efecto",
        clear: "Limpiar las fallas",
      },
      doneMessage: {
        powerOn: "Lámpara encendida.",
        powerOff: "Lámpara apagada.",
        brightness: "Brillo al {value}.",
        cct: "Blanco a {value} K.",
        color: "Color {name}.",
        scene: "Escena {name} aplicada.",
        effect: "Efecto {name} iniciado.",
        stop: "Efecto detenido.",
        clear: "Fallas limpiadas. Enciende cuando quieras.",
      },
      suggestions: [
        "¿Por qué está bloqueado?",
        "Pon el brillo al 40 %",
        "Inicia Reading",
        "¿Cómo emparejo?",
        "OTA y slots",
        "¿Hay fallas?",
      ],
    },
  };

  const COLOR_NAMES = {
    en: {
      red: ["red", 0, 1000],
      orange: ["orange", 28, 1000],
      yellow: ["yellow", 52, 1000],
      green: ["green", 120, 1000],
      cyan: ["cyan", 180, 1000],
      turquoise: ["turquoise", 180, 1000],
      blue: ["blue", 220, 1000],
      purple: ["purple", 280, 1000],
      violet: ["violet", 280, 1000],
      pink: ["pink", 330, 1000],
    },
    es: {
      rojo: ["rojo", 0, 1000],
      naranja: ["naranja", 28, 1000],
      amarillo: ["amarillo", 52, 1000],
      verde: ["verde", 120, 1000],
      cian: ["cian", 180, 1000],
      turquesa: ["turquesa", 180, 1000],
      azul: ["azul", 220, 1000],
      morado: ["morado", 280, 1000],
      violeta: ["violeta", 280, 1000],
      rosa: ["rosa", 330, 1000],
    },
  };

  const SCENE_KEYS = {
    en: {
      reading: "reading",
      relax: "relax",
      focus: "focus",
      night: "night",
    },
    es: {
      lectura: "reading",
      descanso: "relax",
      concentracion: "focus",
      noche: "night",
    },
  };

  const EFFECT_IDS = {
    en: {
      breathing: 1,
      rainbow: 2,
      candle: 4,
      strobe: 3,
      police: 5,
      sunrise: 6,
    },
    es: {
      respiracion: 1,
      arcoiris: 2,
      vela: 4,
      estrobo: 3,
      policia: 5,
      amanecer: 6,
    },
  };

  const STOPWORDS = new Set(
    "a an and are as at be by can could do does for from how i in is it me my of on or please show tell the this to what when where which who why with you your el la los las un una de del que para con por como me mi tu te se su sus al lo le les es son esta este esto hay y o en".split(
      " ",
    ),
  );

  const root = document.getElementById("assistant-panel");
  if (!root) return;
  const toggle = document.getElementById("assistant-toggle");
  const messages = document.getElementById("assistant-messages");
  const suggestions = document.getElementById("assistant-suggestions");
  const form = document.getElementById("assistant-form");
  const input = document.getElementById("assistant-input");
  const close = document.getElementById("assistant-close");
  let openedOnce = false;

  function lang() {
    return document.documentElement.lang === "es" ? "es" : "en";
  }

  function copy() {
    return COPY[lang()];
  }

  function fmt(text, values) {
    return Object.entries(values || {}).reduce(
      (out, [key, value]) => out.replaceAll(`{${key}}`, String(value)),
      text,
    );
  }

  function normalize(text) {
    return text
      .toLowerCase()
      .normalize("NFD")
      .replace(/[\u0300-\u036f]/g, "")
      .replace(/[“”"'`]/g, "")
      .replace(/\s+/g, " ")
      .trim();
  }

  function t(key) {
    return window.LampI18n ? window.LampI18n.t(key) : key;
  }

  function context() {
    return window.LampAssistantAPI
      ? window.LampAssistantAPI.context()
      : { state: {}, network: {}, health: null, locked: false, lockReasons: [] };
  }

  function actions() {
    return window.LampAssistantAPI ? window.LampAssistantAPI.actions : null;
  }

  function lockReason(ctx) {
    const c = copy();
    for (const reason of ctx.lockReasons || []) {
      if (reason === "stale") return c.stale;
      if (reason.startsWith("network:")) {
        return fmt(c.network, { state: reason.slice("network:".length) });
      }
      if (reason.startsWith("fault:")) {
        const code = Number(reason.slice("fault:".length));
        if (code === 5) return c.safeMode;
        if (code === 7) return c.heartbeat;
        return fmt(c.faultLocked, { code });
      }
    }
    return c.unlocked;
  }

  function faultText(code) {
    if (!code) return copy().faultNone;
    let label = t(`fault.code.${code}`);
    if (label === `fault.code.${code}`) label = `code ${code}`;
    return fmt(copy().faultLabel, { code, label });
  }

  function statusReply(ctx) {
    const c = copy();
    const s = ctx.state || {};
    const color =
      Number(s.mode) === 1
        ? `${s.cct || 0} K`
        : `H${Math.round(s.h || 0)} S${Math.round(s.s || 0)}`;
    return fmt(c.status, {
      power: s.power ? t("state.on") : t("state.off"),
      brightness: s.brightness ?? 0,
      color,
      fault: s.faultCode || 0,
    });
  }

  function dateReply(ctx) {
    const c = copy();
    if (!ctx.health) return c.version.replace("{version}", "?").replace("{slot}", "?");
    return fmt(c.version, {
      version: ctx.health.fw_version || "?",
      slot: ctx.health.slot || "?",
    });
  }

  function tokenize(text) {
    return normalize(text)
      .replace(/[^a-z0-9% ]/g, " ")
      .split(/\s+/)
      .filter((word) => word.length > 2 && !STOPWORDS.has(word));
  }

  function searchKnowledge(query) {
    const knowledge = window.LAMP_ASSISTANT_KNOWLEDGE || [];
    const terms = tokenize(query);
    if (!terms.length) return null;
    let best = null;
    for (const entry of knowledge) {
      const title = normalize(`${entry.page} ${entry.title}`);
      const body = normalize(entry.text);
      let score = 0;
      for (const term of terms) {
        if (title.includes(term)) score += 4;
        if (body.includes(term)) score += 1;
      }
      if (score >= 3 && (!best || score > best.score)) {
        best = { entry, score };
      }
    }
    return best ? best.entry : null;
  }

  function knowledgeReply(entry) {
    const c = copy();
    const text = entry.text.length > 720 ? `${entry.text.slice(0, 717)}…` : entry.text;
    const source = fmt(c.source, { page: entry.page, title: entry.title });
    const note = lang() === "es" ? `\n\n${c.manualEnglish}` : "";
    return { text: `${text}\n\n${source}${note}` };
  }

  function actionReply(kind, label, done, run) {
    return {
      text: copy().confirm,
      action: { kind, label, done, run },
    };
  }

  function requireUnlocked(ctx) {
    if (!ctx.locked) return null;
    return { text: fmt(copy().locked, { reason: lockReason(ctx) }) };
  }

  function answer(raw) {
    const text = normalize(raw);
    const c = copy();
    const ctx = context();
    const api = actions();
    const parts = text.split(/\s+/);
    const has = (...words) => words.some((word) => text.includes(word));
    const hasWord = (...words) =>
      words.some((word) => parts.includes(word) || text.includes(` ${word} `));

    if (!text) return { text: c.empty };

    if (has("clear fault", "limpiar falla", "limpia falla", "reset fault")) {
      if (!ctx.state?.faultCode) return { text: c.noFault };
      return actionReply(
        "clear",
        c.plan.clear,
        c.doneMessage.clear,
        () => api.clearFaults(),
      );
    }

    if (hasWord("help", "ayuda") && !has("what", "que")) {
      const target = has("schedule", "horario")
        ? "schedules"
        : has("sequence", "secuencia")
          ? "sequences"
          : has("setting", "ajuste")
            ? "settings"
            : "help";
      if (api) api.open(target);
      closePanel();
      return { text: fmt(c.opened, { target }) };
    }
    if (has("open settings", "abrir ajustes", "open setting", "abrir configuracion")) {
      if (api) api.open("settings");
      closePanel();
      return { text: fmt(c.opened, { target: "Settings" }) };
    }
    if (has("open schedule", "abrir horario", "manage schedule", "gestionar horario")) {
      if (api) api.open("schedules");
      closePanel();
      return { text: fmt(c.opened, { target: "Schedules" }) };
    }
    if (has("open sequence", "abrir secuencia", "manage sequence", "gestionar secuencia")) {
      if (api) api.open("sequences");
      closePanel();
      return { text: fmt(c.opened, { target: "Sequences" }) };
    }

    if (has("pair", "pairing", "emparej", "add device", "wifi", "wi-fi")) {
      return {
        text:
          c.plan === COPY.es.plan
            ? "El pairing necesita 3 ciclos Power Off/On duros desde el Panel Sim (1–10 s, espera APP_RUNNING). El dashboard solo escanea: pulsa Add device cuando la lámpara parpadee. Instrucciones completas en el manual: Pairing."
            : "Pairing needs 3 hard Power Off/On cycles from the Sim panel (1–10 s, wait for APP_RUNNING). The dashboard only scans: press Add device when the lamp blinks. Full steps are in the manual: Pairing.",
      };
    }
    if (has("ota", "update firmware", "actualizar firmware", "downgrade", "slot")) {
      return {
        text:
          c.plan === COPY.es.plan
            ? "El OTA se prueba en Panel Sim → Firmware. Un artefacto más nuevo cambia de slot; un downgrade se rechaza por anti-downgrade y el equipo sigue en la versión actual. No hay forma de saltarse esa protección."
            : "Test OTA in Sim panel → Firmware. A newer artifact switches slots; a downgrade is rejected by anti-downgrade and the device stays on its current version. There is no way to bypass that protection.",
      };
    }
    if (has("fault", "falla", "latched", "derate", "safe mode", "heartbeat")) {
      if (has("why", "por que", "porque", "status", "estado", "que")) {
        return {
          text: ctx.state?.faultCode
            ? faultText(Number(ctx.state.faultCode))
            : c.noFault,
        };
      }
      return {
        text:
          c.plan === COPY.es.plan
            ? "Un aviso de sobretemperatura baja el brillo al 50 %. Una falla crítica, de sobrecorriente o de LED abierto apaga y latcha; pulsa Clear faults y luego enciende. El modo seguro por configuración corrupta también bloquea el encendido."
            : "An overtemperature warning derates brightness to 50 %. A critical, overcurrent or LED-open fault trips and latches; press Clear faults, then turn the lamp on. Configuration safe mode also blocks power-on.",
      };
    }
    if (has("schedule", "horario", "programar", "sunrise", "amanecer")) {
      return {
        text:
          c.plan === COPY.es.plan
            ? "Abre Schedules → Manage. Elige hora, repetición y una acción (color/blanco, escena, efecto, apagar o amanecer). Los horarios viven en el backend; el factory reset los borra."
            : "Open Schedules → Manage. Choose the time, repeat mode and an action (color/white, scene, effect, off or sunrise). Schedules live in the backend; factory reset removes them.",
      };
    }
    if (has("sequence", "secuencia")) {
      return {
        text:
          c.plan === COPY.es.plan
            ? "Abre Sequences → Manage. Añade pasos, edita color y brillo en el selector, ajusta duración, ordena con las flechas o arrastrando y pulsa Start. Se guarda en este navegador, no en la flash."
            : "Open Sequences → Manage. Add steps, edit color and brightness in the picker, set duration, reorder with the arrows or drag, and press Start. It is stored in this browser, not in flash.",
      };
    }
    if (has("setting", "ajuste", "reset dashboard", "restablecer")) {
      return {
        text:
          c.plan === COPY.es.plan
            ? "Abre Settings para Reset dashboard (secuencias, adaptive white, fiesta, horarios, power-on e historial) o Clear fault history. No hace factory reset de la lámpara."
            : "Open Settings for Reset dashboard (sequences, adaptive white, party, schedules, power-on and history) or Clear fault history. It does not factory-reset the lamp.",
      };
    }

    if (has("status", "estado", "how is", "que tal", "resumen")) {
      return { text: statusReply(ctx) };
    }
    if (has("version", "firmware", "slot", "actual")) {
      return { text: dateReply(ctx) };
    }
    if (has("why", "por que", "porque", "locked", "bloqueado", "disabled", "gris")) {
      return {
        text: ctx.locked
          ? fmt(c.locked, { reason: lockReason(ctx) })
          : c.unlocked,
      };
    }

    if (hasWord("on", "encender", "enciende", "prender", "prende") &&
        !has("turn off", "apagar", "apaga")) {
      const locked = requireUnlocked(ctx);
      if (locked) return locked;
      return actionReply(
        "powerOn",
        c.plan.powerOn,
        c.doneMessage.powerOn,
        () => api.setPower(true),
      );
    }
    if (hasWord("off", "apagar", "apaga") || has("turn off", "switch off")) {
      const locked = requireUnlocked(ctx);
      if (locked) return locked;
      return actionReply(
        "powerOff",
        c.plan.powerOff,
        c.doneMessage.powerOff,
        () => api.setPower(false),
      );
    }

    const kelvin = text.match(/\b(\d{4})\s*k\b/);
    if (kelvin || has("warm", "calido", "cool", "frio")) {
      const value = kelvin ? Number(kelvin[1]) : has("warm", "calido") ? 3000 : 5600;
      const locked = requireUnlocked(ctx);
      if (locked) return locked;
      return actionReply(
        "cct",
        fmt(c.plan.cct, { value }),
        fmt(c.doneMessage.cct, { value }),
        () => api.setCct(value),
      );
    }

    const brightness = text.match(
      /(?:brightness|brillo|dim)\D{0,16}(\d{1,4})\s*%?/,
    ) || text.match(/\b(\d{1,3})\s*%/);
    if (brightness) {
      let value = Number(brightness[1]);
      if (value <= 100) value *= 10;
      value = Math.max(0, Math.min(1000, value));
      const locked = requireUnlocked(ctx);
      if (locked) return locked;
      return actionReply(
        "brightness",
        fmt(c.plan.brightness, { value: Math.round(value / 10) }),
        fmt(c.doneMessage.brightness, { value: Math.round(value / 10) }),
        () => api.setBrightness(value),
      );
    }

    for (const [word, [name, hue, sat]] of Object.entries(COLOR_NAMES[lang()])) {
      if (text.includes(word)) {
        const locked = requireUnlocked(ctx);
        if (locked) return locked;
        return actionReply(
          "color",
          fmt(c.plan.color, { name }),
          fmt(c.doneMessage.color, { name }),
          () => api.setColor(hue, sat),
        );
      }
    }

    for (const [word, key] of Object.entries(SCENE_KEYS[lang()])) {
      if (parts.includes(word) || text.includes(` ${word} `)) {
        const locked = requireUnlocked(ctx);
        if (locked) return locked;
        return actionReply(
          "scene",
          fmt(c.plan.scene, { name: word }),
          fmt(c.doneMessage.scene, { name: word }),
          () => api.applyScene(key),
        );
      }
    }

    for (const [word, id] of Object.entries(EFFECT_IDS[lang()])) {
      if (parts.includes(word) || text.includes(` ${word} `)) {
        const locked = requireUnlocked(ctx);
        if (locked) return locked;
        return actionReply(
          "effect",
          fmt(c.plan.effect, { name: word }),
          fmt(c.doneMessage.effect, { name: word }),
          () => api.startEffect(id),
        );
      }
    }

    if (has("stop", "detener", "parar")) {
      const locked = requireUnlocked(ctx);
      if (locked) return locked;
      return actionReply(
        "stop",
        c.plan.stop,
        c.doneMessage.stop,
        () => api.stopEffect(),
      );
    }

    const found = searchKnowledge(raw);
    if (found) return knowledgeReply(found);
    return { text: c.fallback };
  }

  function addMessage(role, text, action) {
    const wrapper = document.createElement("div");
    wrapper.className = `assistant-message ${role}`;
    const bubble = document.createElement("div");
    bubble.className = "assistant-bubble";
    bubble.textContent = text;
    wrapper.appendChild(bubble);

    if (action) {
      const card = document.createElement("div");
      card.className = "assistant-action";
      const label = document.createElement("p");
      label.textContent = action.label;
      const row = document.createElement("div");
      row.className = "assistant-action-row";
      const apply = document.createElement("button");
      apply.type = "button";
      apply.className = "secondary small";
      apply.textContent = copy().apply;
      const cancel = document.createElement("button");
      cancel.type = "button";
      cancel.className = "secondary small";
      cancel.textContent = copy().cancel;
      row.append(apply, cancel);
      card.append(label, row);
      wrapper.appendChild(card);

      cancel.addEventListener("click", () => {
        card.remove();
        addMessage("assistant", copy().cancelled);
      });
      apply.addEventListener("click", async () => {
        apply.disabled = true;
        cancel.disabled = true;
        try {
          const result = await action.run();
          if (result && result.locked) {
            addMessage("assistant", copy().locked.replace("{reason}", lockReason(context())));
          } else if (result && result.unknown) {
            addMessage("assistant", copy().fallback);
          } else {
            addMessage("assistant", action.done);
          }
        } catch {
          addMessage("assistant", copy().fallback);
        }
      });
    }

    messages.appendChild(wrapper);
    messages.scrollTop = messages.scrollHeight;
  }

  function renderSuggestions() {
    suggestions.textContent = "";
    copy().suggestions.forEach((text) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "assistant-chip";
      button.textContent = text;
      button.addEventListener("click", () => {
        input.value = text;
        submit();
      });
      suggestions.appendChild(button);
    });
  }

  function applyCopy() {
    const c = copy();
    document.getElementById("assistant-title").textContent = c.title;
    document.getElementById("assistant-toggle-label").textContent = c.button;
    document.getElementById("assistant-close").textContent = c.close;
    document.getElementById("assistant-send").textContent = c.send;
    input.placeholder = c.placeholder;
    input.setAttribute("aria-label", c.placeholder);
    document.getElementById("assistant-disclaimer").textContent = c.disclaimer;
    toggle.setAttribute("aria-label", c.button);
    renderSuggestions();
  }

  function openPanel() {
    root.classList.remove("hidden");
    toggle.setAttribute("aria-expanded", "true");
    if (!openedOnce) {
      openedOnce = true;
      addMessage("assistant", copy().greeting);
    }
    input.focus();
  }

  function closePanel() {
    root.classList.add("hidden");
    toggle.setAttribute("aria-expanded", "false");
  }

  function submit() {
    const text = input.value.trim();
    if (!text) return;
    input.value = "";
    addMessage("user", text);
    const reply = answer(text);
    addMessage("assistant", reply.text, reply.action);
  }

  toggle.addEventListener("click", () => {
    if (root.classList.contains("hidden")) openPanel();
    else closePanel();
  });
  close.addEventListener("click", closePanel);
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    submit();
  });
  input.addEventListener("keydown", (event) => {
    if (event.key === "Escape") closePanel();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !root.classList.contains("hidden")) closePanel();
  });
  window.addEventListener("lamp:lang", applyCopy);

  applyCopy();
})();
