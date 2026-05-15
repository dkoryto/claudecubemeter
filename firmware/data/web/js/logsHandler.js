function logsHandler() {
  return {
    loading: false,
    logs: [],
    message: "",

    fetchLogs() {
      this.loading = true;
      this.message = "";
      apiFetch("/api/v1/logs")
        .then((r) => r.json())
        .then((data) => {
          this.logs = data.logs || [];
        })
        .catch((err) => {
          this.message = "Failed to fetch logs";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    downloadLogs() {
      this.loading = true;
      this.message = "";
      const token = localStorage.getItem("apiToken") || "";
      fetch("/api/v1/logs/download", {
        headers: { Authorization: "Bearer " + token },
      })
        .then((r) => {
          if (!r.ok) throw new Error("Download failed");
          return r.blob();
        })
        .then((blob) => {
          const url = URL.createObjectURL(blob);
          const a = document.createElement("a");
          a.href = url;
          a.download = "logs.log";
          document.body.appendChild(a);
          a.click();
          document.body.removeChild(a);
          URL.revokeObjectURL(url);
          this.message = "Download started";
        })
        .catch((err) => {
          this.message = "Failed to download logs";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    clearLogs() {
      if (!confirm("Clear all logs?")) return;
      this.loading = true;
      this.message = "";
      apiFetch("/api/v1/logs/clear", { method: "POST" })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.logs = [];
            this.message = "Logs cleared";
          } else {
            this.message = data.message || "Failed to clear logs";
          }
        })
        .catch((err) => {
          this.message = "Failed to clear logs";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchLogs();
    },
  };
}
