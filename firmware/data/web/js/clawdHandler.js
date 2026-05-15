function clawdHandler() {
  return {
    status: {
      configured: false,
      valid: false,
      session_pct: 0,
      session_reset_min: 0,
      weekly_pct: 0,
      weekly_reset_min: 0,
      status: "",
      last_poll_unix: 0,
      http_code: 0,
      last_error: "",
    },
    config: { anthropic_token_preview: "", webhook_url: "" },
    anthropicToken: "",
    webhookUrl: "",
    showToken: false,
    loading: false,
    tokenMsg: "",
    webhookMsg: "",
    pollTimer: null,

    authHeaders() {
      const t = localStorage.getItem("Authorization") || "";
      return { Authorization: "Bearer " + t };
    },

    async init() {
      await this.loadConfig();
      await this.refresh();
      this.pollTimer = setInterval(() => this.refresh(true), 15000);
    },

    formatMin(m) {
      if (!m || m <= 0) return "—";
      if (m < 60) return m + "m";
      const h = Math.floor(m / 60);
      const rem = m % 60;
      if (h < 24) return rem === 0 ? h + "h" : h + "h " + rem + "m";
      const d = Math.floor(h / 24);
      const remH = h % 24;
      return remH === 0 ? d + "d" : d + "d " + remH + "h";
    },

    formatPollAgo(unix) {
      if (!unix) return "(never)";
      const ago = Math.floor(Date.now() / 1000 - unix);
      if (ago < 60) return ago + "s ago";
      return Math.floor(ago / 60) + "m ago";
    },

    async loadConfig() {
      try {
        const r = await fetch("/api/v1/clawd/config", { headers: this.authHeaders() });
        if (r.ok) {
          this.config = await r.json();
          this.webhookUrl = this.config.webhook_url || "";
        }
      } catch (e) {}
    },

    async refresh(silent) {
      if (!silent) this.loading = true;
      try {
        const r = await fetch("/api/v1/clawd/status", { headers: this.authHeaders() });
        if (r.ok) this.status = await r.json();
      } catch (e) {}
      this.loading = false;
    },

    async saveToken() {
      if (!this.anthropicToken.trim()) {
        this.tokenMsg = "Paste a token first.";
        return;
      }
      this.loading = true;
      this.tokenMsg = "Saving...";
      try {
        const r = await fetch("/api/v1/clawd/config", {
          method: "POST",
          headers: { "Content-Type": "application/json", ...this.authHeaders() },
          body: JSON.stringify({ anthropic_token: this.anthropicToken.trim() }),
        });
        if (r.ok) {
          this.tokenMsg = "Saved. Triggering refresh...";
          this.anthropicToken = "";
          await this.loadConfig();
          await this.forcePoll();
        } else {
          const data = await r.json().catch(() => ({}));
          this.tokenMsg = data.message || "Save failed.";
        }
      } catch (e) {
        this.tokenMsg = "Request failed.";
      } finally {
        this.loading = false;
      }
    },

    async saveWebhook() {
      this.loading = true;
      this.webhookMsg = "Saving...";
      try {
        const r = await fetch("/api/v1/clawd/config", {
          method: "POST",
          headers: { "Content-Type": "application/json", ...this.authHeaders() },
          body: JSON.stringify({ webhook_url: this.webhookUrl.trim() }),
        });
        this.webhookMsg = r.ok ? "Saved." : "Save failed.";
        await this.loadConfig();
      } catch (e) {
        this.webhookMsg = "Request failed.";
      } finally {
        this.loading = false;
      }
    },

    async forcePoll() {
      try {
        await fetch("/api/v1/clawd/refresh", {
          method: "POST",
          headers: this.authHeaders(),
        });
        await this.refresh();
      } catch (e) {}
    },

    async sendShortcut(key) {
      this.webhookMsg = "Sending " + key + "...";
      try {
        const r = await fetch("/api/v1/clawd/shortcut", {
          method: "POST",
          headers: { "Content-Type": "application/json", ...this.authHeaders() },
          body: JSON.stringify({ key: key }),
        });
        const data = await r.json().catch(() => ({}));
        this.webhookMsg = data.status === "ok" ? "Sent." : "Failed: " + (data.message || "");
      } catch (e) {
        this.webhookMsg = "Request failed.";
      }
    },
  };
}
