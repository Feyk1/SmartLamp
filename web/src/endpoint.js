/* Resolves the backend for local or LAN access.
 *
 * `config.js` usually points at localhost:8080. If the page is opened from
 * another host on the local network, the browser must use that same host with
 * port 8080 instead of its own localhost. On Fly/GitHub Pages, the configured
 * URL is already public and is used as-is.
 */
(() => {
  "use strict";

  const LOCAL_HOSTS = new Set(["localhost", "127.0.0.1", "::1", "0.0.0.0"]);

  function isLocal(hostname) {
    return LOCAL_HOSTS.has(hostname);
  }

  function stripSlash(value) {
    return (value || "").replace(/\/+$/, "");
  }

  function orchestrator() {
    const configured = stripSlash(
      window.LAMP_CONFIG && window.LAMP_CONFIG.orchestrator,
    );
    const page = window.location;
    if (!page.hostname) {
      return configured || "http://localhost:8080";
    }
    if (configured) {
      try {
        const url = new URL(configured, page.href);
        if (!isLocal(url.hostname) || isLocal(page.hostname)) {
          return stripSlash(url.href);
        }
      } catch {
        return configured;
      }
    }
    const protocol = page.protocol === "https:" ? "https:" : "http:";
    return `${protocol}//${page.hostname}:8080`;
  }

  function wsUrl(instance, ticket) {
    const base = orchestrator()
      .replace(/^http:/, "ws:")
      .replace(/^https:/, "wss:");
    return (
      `${base}/ws/dashboard?instance=${encodeURIComponent(instance.id)}` +
      `&ticket=${encodeURIComponent(ticket)}`
    );
  }

  async function wsTicket(instance, headers = {}) {
    const response = await fetch(
      `${orchestrator()}/api/v1/instances/${encodeURIComponent(instance.id)}` +
        "/ws-ticket",
      { method: "POST", headers },
    );
    if (!response.ok) {
      throw new Error(`ws-ticket -> ${response.status}`);
    }
    const data = await response.json();
    if (!data.ticket) throw new Error("ws-ticket missing");
    return data.ticket;
  }

  window.LampConfig = { orchestrator, wsUrl, wsTicket };
})();
