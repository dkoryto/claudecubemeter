document.addEventListener("alpine:init", () => {
  Alpine.data("themeSwitcher", themeSwitcher);
  if (typeof otaUploadHandler !== "undefined")
    Alpine.data("otaUploadHandler", otaUploadHandler);
  if (typeof gifUploadHandler !== "undefined")
    Alpine.data("gifUploadHandler", gifUploadHandler);
  if (typeof wifiHandler !== "undefined")
    Alpine.data("wifiHandler", wifiHandler);
  if (typeof ntpHandler !== "undefined") Alpine.data("ntpHandler", ntpHandler);
  if (typeof rotationHandler !== "undefined")
    Alpine.data("rotationHandler", rotationHandler);
  if (typeof rebootHandler !== "undefined")
    Alpine.data("rebootHandler", rebootHandler);
  if (typeof tokenHandler !== "undefined")
    Alpine.data("tokenHandler", tokenHandler);
});
