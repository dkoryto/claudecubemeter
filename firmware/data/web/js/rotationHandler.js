function rotationHandler() {
  return {
    loading: false,
    rotation: 0,
    statusMsg: "",

    fetchRotation() {
      this.loading = true;
      apiFetch("/api/v1/display/rotation")
        .then((r) => r.json())
        .then((data) => {
          this.rotation = Number.isInteger(data.rotation) ? data.rotation : 0;
          this.statusMsg = "";
        })
        .catch((err) => {
          this.statusMsg = "Failed to load rotation";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    saveRotation() {
      this.loading = true;
      const payload = { rotation: Number(this.rotation) };

      apiFetch("/api/v1/display/rotation", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.rotation = Number.isInteger(data.rotation)
              ? data.rotation
              : payload.rotation;
            this.statusMsg = "Rotation updated";
          } else {
            this.statusMsg = data.message || "Failed to save rotation";
          }
        })
        .catch((err) => {
          this.statusMsg = "Failed to save rotation";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchRotation();
    },
  };
}
