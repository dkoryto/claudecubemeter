// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <Logger.h>
#include <ArduinoJson.h>
#include <Updater.h>

#include "web/Webserver.h"
#include "web/Api.h"
#include "display/DisplayManager.h"

#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "ntp/NTPClient.h"
#include "clawd/ClawdManager.h"

extern ConfigManager configManager;
extern WiFiManager* wifiManager;
extern NTPClient* ntpClient;

static bool otaError = false;
static size_t otaSize = 0;
static String otaStatus;
static volatile bool otaInProgress = false;
static volatile bool otaCancelRequested = false;
static size_t otaTotal = 0;

static constexpr int OTA_TEXT_X_OFFSET = 50;
static constexpr int OTA_TEXT_Y_OFFSET = 80;
static constexpr int OTA_LOADING_Y_OFFSET = 110;

static void otaHandleStart(HTTPUpload& upload, int mode);
static void otaHandleWrite(HTTPUpload& upload);
static void otaHandleEnd(HTTPUpload& upload, int mode);
static void otaHandleAborted(HTTPUpload& upload);
void handleDeleteGif(Webserver* webserver);

static constexpr int WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr size_t NTP_CONFIG_DOC_SIZE = 512;
static constexpr int BEARER_LEN = 7;

/**
 * @brief Register API endpoints for the webserver
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void registerApiEndpoints(Webserver* webserver) {
    Logger::info("Registering API endpoints", "API");

    // @openapi {get} /wifi/scan version=v1 group=WiFi summary="Scan available WiFi networks" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/scan", HTTP_GET, [webserver]() { handleWifiScan(webserver); });

    // @openapi {post} /wifi/connect version=v1 group=WiFi summary="Connect to a WiFi network" requiresAuth=true
    // requestBody=application/json requestBodySchema=ssid:string,password:string
    // example={"ssid":"MyNetwork","password":"password123"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/connect", HTTP_POST, [webserver]() { handleWifiConnect(webserver); });

    // @openapi {get} /wifi/status version=v1 group=WiFi summary="Get WiFi connection status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/status", HTTP_GET, [webserver]() { handleWifiStatus(webserver); });

    // @openapi {post} /ntp/sync version=v1 group=NTP summary="Trigger NTP sync" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/sync", HTTP_POST, [webserver]() { handleNtpSync(webserver); });

    // @openapi {get} /ntp/status version=v1 group=NTP summary="Get NTP status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/status", HTTP_GET, [webserver]() { handleNtpStatus(webserver); });

    // @openapi {get} /ntp/config version=v1 group=NTP summary="Get NTP configuration" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_GET, [webserver]() { handleNtpConfigGet(webserver); });

    // @openapi {post} /ntp/config version=v1 group=NTP summary="Set NTP configuration" requiresAuth=true
    // requestBody=application/json requestBodySchema=ntp_server:string example={"ntp_server":"pool.ntp.org"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_POST, [webserver]() { handleNtpConfigSet(webserver); });

    // @openapi {get} /display/rotation version=v1 group=Display summary="Get display rotation" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/display/rotation", HTTP_GET, [webserver]() { handleDisplayRotationGet(webserver); });

    // @openapi {post} /display/rotation version=v1 group=Display summary="Set display rotation" requiresAuth=true
    // requestBody=application/json requestBodySchema=rotation:integer example={"rotation":4}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/display/rotation", HTTP_POST, [webserver]() { handleDisplayRotationSet(webserver); });

    // @openapi {post} /reboot version=v1 group=System summary="Reboot the device" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/reboot", HTTP_POST, [webserver]() { handleReboot(webserver); });

    // @openapi {post} /ota/fw version=v1 group=OTA summary="Upload firmware (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fw", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FLASH); });

    // @openapi {post} /ota/fs version=v1 group=OTA summary="Upload filesystem (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fs", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FS); });

    // @openapi {get} /ota/status version=v1 group=OTA summary="Get OTA status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/status", HTTP_GET, [webserver]() { handleOtaStatus(webserver); });

    // @openapi {post} /ota/cancel version=v1 group=OTA summary="Cancel OTA" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/cancel", HTTP_POST, [webserver]() { handleOtaCancel(webserver); });

    // @openapi {post} /gif version=v1 group=GIF summary="Upload a GIF" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/gif", HTTP_POST, [webserver]() { handleGifUpload(webserver); },
        [webserver]() { handleGifUpload(webserver); });

    // @openapi {post} /gif/play version=v1 group=GIF summary="Play a GIF by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"animation.gif"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->raw().on("/api/v1/gif/play", HTTP_POST, [webserver]() { handlePlayGif(webserver); });

    // @openapi {post} /gif/stop version=v1 group=GIF summary="Stop GIF playback" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/gif/stop", HTTP_POST, [webserver]() { handleStopGif(webserver); });

    // @openapi {delete} /gif version=v1 group=GIF summary="Delete a GIF by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"animation.gif"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->raw().on("/api/v1/gif", HTTP_DELETE, [webserver]() { handleDeleteGif(webserver); });

    // @openapi {get} /gif version=v1 group=GIF summary="List GIFs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/gif", HTTP_GET, [webserver]() { handleListGifs(webserver); });

    // @openapi {get} /token/check version=v1 group=Authentication summary="Check bearer token validity"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/token/check", HTTP_GET, [webserver]() { handleTokenCheck(webserver); });

    // @openapi {post} /token/save version=v1 group=Authentication summary="Save a new bearer token" requiresAuth=true
    // requestBody=application/json requestBodySchema=token:string example={"token":"your_secure_token_value"}
    // responses=200:application/json,401:application/json,400:application/json
    webserver->raw().on("/api/v1/token/save", HTTP_POST, [webserver]() { handleTokenSave(webserver); });

    // @openapi {get} /logs version=v1 group=System summary="Get recent logs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/logs", HTTP_GET, [webserver]() { handleLogsGet(webserver); });

    // @openapi {get} /logs/download version=v1 group=System summary="Download logs as text file" requiresAuth=true
    // responses=200:text/plain,401:application/json
    webserver->raw().on("/api/v1/logs/download", HTTP_GET, [webserver]() { handleLogsDownload(webserver); });

    // @openapi {post} /logs/clear version=v1 group=System summary="Clear log buffer" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/logs/clear", HTTP_POST, [webserver]() { handleLogsClear(webserver); });

    // @openapi {get} /clawd/status version=v1 group=Clawd summary="Get current Claude Code usage" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/clawd/status", HTTP_GET, [webserver]() { handleClawdStatus(webserver); });

    // @openapi {get} /clawd/config version=v1 group=Clawd summary="Get Clawd config (token redacted)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/clawd/config", HTTP_GET, [webserver]() { handleClawdConfigGet(webserver); });

    // @openapi {post} /clawd/config version=v1 group=Clawd summary="Set Anthropic token and webhook URL" requiresAuth=true
    // requestBody=application/json requestBodySchema=anthropic_token:string,webhook_url:string
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/clawd/config", HTTP_POST, [webserver]() { handleClawdConfigSet(webserver); });

    // @openapi {post} /clawd/refresh version=v1 group=Clawd summary="Force immediate poll" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/clawd/refresh", HTTP_POST, [webserver]() { handleClawdRefresh(webserver); });

    // @openapi {post} /clawd/shortcut version=v1 group=Clawd summary="Forward shortcut to webhook" requiresAuth=true
    // requestBody=application/json requestBodySchema=key:string responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/clawd/shortcut", HTTP_POST, [webserver]() { handleClawdShortcut(webserver); });

    // @openapi {post} /clawd/data version=v1 group=Clawd summary="Push usage data from host proxy" requiresAuth=true
    // requestBody=application/json requestBodySchema=session_pct:integer,session_reset_min:integer,weekly_pct:integer,weekly_reset_min:integer,status:string
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/clawd/data", HTTP_POST, [webserver]() { handleClawdData(webserver); });

    // @openapi {post} /clawd/calibrate version=v1 group=Clawd summary="Draw calibration grid on display" requiresAuth=true
    webserver->raw().on("/api/v1/clawd/calibrate", HTTP_POST, [webserver]() { handleClawdCalibrate(webserver); });

    // @openapi {post} /clawd/splash version=v1 group=Clawd summary="Redraw the boot splash screen" requiresAuth=true
    webserver->raw().on("/api/v1/clawd/splash", HTTP_POST, [webserver]() { handleClawdSplash(webserver); });

    webserver->raw().onNotFound([webserver]() {
        if (webserver->raw().method() == HTTP_OPTIONS) {
            setCorsHeaders(webserver);
            webserver->raw().send(HTTP_CODE_OK);
        }
    });
}

/**
 * @brief Set CORS headers for API responses
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void setCorsHeaders(Webserver* webserver) {
    webserver->raw().sendHeader("Access-Control-Allow-Origin", "*");
    webserver->raw().sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    webserver->raw().sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webserver->raw().sendHeader("Access-Control-Max-Age", "3600");
}

/**
 * @brief Validate bearer token from Authorization header
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto validateBearerToken(Webserver* webserver) -> bool {
    if (!webserver->raw().hasHeader("Authorization")) {
        return false;
    }

    String authHeader = webserver->raw().header("Authorization");

    if (!authHeader.startsWith("Bearer ")) {
        return false;
    }

    String providedToken = authHeader.substring(BEARER_LEN);
    String storedToken = configManager.getApiToken();

    if (storedToken.length() == 0) {
        return false;
    }

    return providedToken.equals(storedToken);
}

/**
 * @brief Enforce bearer token check and send 401 response if invalid
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto requireBearerToken(Webserver* webserver) -> bool {
    if (validateBearerToken(webserver)) {
        return true;
    }

    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = "Invalid or missing token";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

    Logger::warn(("Unauthorized request from " + webserver->raw().client().remoteIP().toString()).c_str(), "API");

    return false;
}

/**
 * @brief Check if bearer token is valid
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenCheck(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token is valid";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Save a new bearer token
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenSave(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save API token with invalid JSON", "API");

        return;
    }

    const char* newToken = ddoc["token"] | "";

    if (strlen(newToken) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "token field is required";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save empty API token", "API");
        return;
    }

    configManager.setApiToken(newToken);
    configManager.save();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token saved successfully";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("API token updated", "API");
}

/**
 * @brief OTA status endpoint
 */
void handleOtaStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["inProgress"] = otaInProgress;
    doc["bytesWritten"] = otaSize;
    doc["totalBytes"] = otaTotal;
    doc["error"] = otaError;
    doc["message"] = otaStatus;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief OTA cancel endpoint
 */
void handleOtaCancel(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    otaCancelRequested = true;
    otaStatus = "Cancel requested";

    JsonDocument doc;
    doc["status"] = "cancelling";
    doc["message"] = "Cancel request received";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief List GIF files and FS info
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleListGifs(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();

    size_t usedBytes = 0;
    size_t totalBytes = 0;

    if (LittleFS.begin()) {
        Dir dir = LittleFS.openDir("/gif");

        while (dir.next()) {
            String name = dir.fileName();
            if (name.endsWith(".gif") || name.endsWith(".GIF")) {
                JsonObject fileObj = files.add<JsonObject>();

                fileObj["name"] = name;            // NOLINT(readability-misplaced-array-index)
                fileObj["size"] = dir.fileSize();  // NOLINT(readability-misplaced-array-index)
                usedBytes += dir.fileSize();
            }
        }

        FSInfo fs_info;

        if (LittleFS.info(fs_info)) {
            totalBytes = fs_info.totalBytes;
            usedBytes = fs_info.usedBytes;
        }
    }

    doc["usedBytes"] = usedBytes;
    doc["totalBytes"] = totalBytes;
    doc["freeBytes"] = totalBytes > usedBytes ? totalBytes - usedBytes : 0;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload start
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadStart(const String& currentFilename, File& gifFile, bool& uploadError) {
    uploadError = false;
    Logger::info((String("UPLOAD_FILE_START for: ") + currentFilename).c_str(), "API::GIF");

    if (!LittleFS.exists("/gif")) {
        Logger::info("/gif directory does not exist, creating...", "API::GIF");
        if (!LittleFS.mkdir("/gif")) {
            Logger::error("Failed to create /gif directory!", "API::GIF");
        }
    }

    gifFile = LittleFS.open(currentFilename, "w");
    if (!gifFile) {
        uploadError = true;
        Logger::error((String("Impossible to open file: ") + currentFilename).c_str(), "API::GIF");
        Logger::error("GIF UPLOAD Failed to open file", "API::GIF");
    } else {
        Logger::info("File opened successfully for writing.", "API::GIF");
    }
}

/**
 * @brief Handle GIF upload write
 * @param upload Reference to the HTTPUpload object
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadWrite(HTTPUpload& upload, File& gifFile, bool& uploadError) {
    if (!uploadError && gifFile) {
        size_t total = 0;

        while (total < upload.currentSize) {
            size_t remaining = upload.currentSize - total;
            int toWrite = static_cast<int>(remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : remaining);
            size_t written = gifFile.write(upload.buf + total, toWrite);

            if (written == 0) {
                Logger::error("Write returned 0 bytes!", "API::GIF");
                uploadError = true;
                break;
            }

            total += written;
        }
    } else {
        Logger::error("Cannot write, file not open or previous error", "API::GIF");
    }
}

/**
 * @brief Handle GIF upload end
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 *
 * @return void
 */
void handleGifUploadEnd(const String& currentFilename, File& gifFile) {
    if (gifFile) {
        gifFile.close();
    }

    Logger::info((String("Gif upload end: ") + currentFilename).c_str(), "API::GIF");
}

/**
 * @brief Handle GIF upload aborted
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadAborted(const String& currentFilename, File& gifFile, bool& uploadError) {
    Logger::warn("UPLOAD_FILE_ABORTED", "API::GIF");

    if (gifFile) {
        gifFile.close();

        Logger::warn("File closed after abort", "API::GIF");
    }

    if (!currentFilename.isEmpty()) {
        if (LittleFS.remove(currentFilename)) {
            Logger::warn((String("Removed incomplete file: ") + currentFilename).c_str(), "API::GIF");
        } else {
            Logger::error((String("Failed to remove incomplete file: ") + currentFilename).c_str(), "API::GIF");
        }
    }

    uploadError = true;
}

/**
 * @brief Send GIF upload result
 * @param webserver Pointer to the Webserver instance
 * @param currentFilename The current filename being uploaded
 * @param uploadError The upload error flag
 *
 * @return void
 */
void sendGifUploadResult(Webserver* webserver, const String& currentFilename, bool uploadError) {
    JsonDocument doc;

    if (uploadError) {
        doc["status"] = "error";
        doc["message"] = "Error during GIF upload";

        Logger::error("GIF UPLOAD Error during upload", "API::GIF");
    } else {
        doc["status"] = "success";
        doc["message"] = "GIF uploaded successfully";
        doc["filename"] = currentFilename;

        Logger::info((String("Gif upload success, filename: ") + currentFilename).c_str(), "API::GIF");
    }

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleGifUpload(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();
    static File gifFile;
    static bool uploadError = false;

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        uploadError = true;
        JsonDocument doc;

        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    String filename = upload.filename;
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String currentFilename = "/gif/" + filename;

    switch (upload.status) {
        case UPLOAD_FILE_START:
            handleGifUploadStart(currentFilename, gifFile, uploadError);
            break;
        case UPLOAD_FILE_WRITE:
            handleGifUploadWrite(upload, gifFile, uploadError);
            break;
        case UPLOAD_FILE_END:
            handleGifUploadEnd(currentFilename, gifFile);
            break;
        case UPLOAD_FILE_ABORTED:
            handleGifUploadAborted(currentFilename, gifFile, uploadError);
            break;
        default:
            Logger::warn("Unknown upload status.", "API::GIF");
            break;
    }

    if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        sendGifUploadResult(webserver, currentFilename, uploadError);
    }
}

/**
 * @brief Reboot endpoint
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleReboot(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 1000;

    doc["status"] = "rebooting";
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    delay(rebootDelayMs);
    ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
}

/**
 * @brief Manual NTP sync trigger endpoint
 */
void handleNtpSync(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    bool syncOk = ntpClient->syncNow();
    doc["status"] = syncOk ? "ok" : "error";
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Return NTP status
 */
void handleNtpStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    doc["lastOk"] = ntpClient->lastSyncOk();
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get NTP configuration
 */
void handleNtpConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["ntp_server"] = configManager.getNtpServer();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set NTP configuration
 */
void handleNtpConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const char* server = ddoc["ntp_server"] | "";

    if (strlen(server) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "ntp_server missing";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setNtpServer(server);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;

        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    // optionally trigger a sync
    if (ntpClient != nullptr) {
        ntpClient->syncNow();
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["ntp_server"] = server;
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get display rotation configuration
 */
void handleDisplayRotationGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["rotation"] = configManager.getLCDRotationSafe();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set display rotation configuration
 */
void handleDisplayRotationSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err || !ddoc["rotation"].is<int>()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing rotation";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    int rotation = ddoc["rotation"].as<int>();
    const int rotation_range_min = 0;
    const int rotation_range_max = 7;

    if (rotation < rotation_range_min || rotation > rotation_range_max) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] =
            "rotation must be between " + String(rotation_range_min) + " and " + String(rotation_range_max);

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    auto newRotation = static_cast<uint8_t>(rotation);
    configManager.setLCDRotation(newRotation);
    String currentIP = "unknown";

    if (wifiManager != nullptr) {
        currentIP = wifiManager->getIP().toString();
    }

    DisplayManager::setRotation(newRotation, currentIP);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["rotation"] = newRotation;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Display rotation updated to " + String(newRotation)).c_str(), "API");
}

/**
 * @brief Handle OTA upload
 * @param webserver Pointer to the Webserver instance
 * @param mode Update mode U_FLASH U_FS
 *
 * @return void
 */
void handleOtaUpload(Webserver* webserver, int mode) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        otaError = true;
        otaStatus = "Unauthorized";

        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    switch (upload.status) {
        case UPLOAD_FILE_START:
            otaHandleStart(upload, mode);
            break;
        case UPLOAD_FILE_WRITE:
            otaHandleWrite(upload);
            break;
        case UPLOAD_FILE_END:
            otaHandleEnd(upload, mode);
            break;
        case UPLOAD_FILE_ABORTED:
            otaHandleAborted(upload);
            break;
        default:
            break;
    }
}

/**
 * @brief Handle OTA finished
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleOtaFinished(Webserver* webserver) {
    if (!validateBearerToken(webserver)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 5000;

    doc["status"] = "Upload successful";
    doc["message"] = otaStatus;

    if (otaError) {
        doc["status"] = "Error";
    }

    otaInProgress = false;
    otaCancelRequested = false;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    if (!otaError) {
        delay(rebootDelayMs);
        ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
    }
}

/**
 * @brief Play a GIF from LittleFS full screen
 *
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handlePlayGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);

    String path1 = String("/gifs/") + filename;
    String path2 = String("/gif/") + filename;
    String foundPath;

    if (LittleFS.exists(path1)) {
        foundPath = path1;
    } else if (LittleFS.exists(path2)) {
        foundPath = path2;
    } else {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    bool playOk = DisplayManager::playGifFullScreen(foundPath);

    JsonDocument resp;

    resp["status"] = playOk ? "playing" : "error";
    resp["file"] = foundPath;

    String jsonOut;

    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Stop currently playing GIF
 */
void handleStopGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    const bool stopped = DisplayManager::stopGif();

    resp["status"] = stopped ? "stopped" : "error";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Delete a GIF file from storage
 */
void handleDeleteGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String path = String("/gif/") + filename;

    if (!LittleFS.exists(path)) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    if (LittleFS.remove(path)) {
        JsonDocument resp;
        resp["status"] = "success";
        resp["message"] = "file removed";
        resp["file"] = path;

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);

        Logger::info((String("Removed file: ") + path).c_str(), "API::GIF");
    } else {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "failed to remove file";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        Logger::error((String("Failed to remove file: ") + path).c_str(), "API::GIF");
    }
}

/**
 * @brief Handle WiFi scan
 */
void handleWifiScan(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (wifiManager != nullptr) {
        WiFiManager::scanNetworks(networks);
    }

    String out;
    serializeJson(doc["networks"], out);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", out);
}

/**
 * @brief Handle WiFi connect request
 */
void handleWifiConnect(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";

    if (strlen(ssid) == 0) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "missing ssid";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    bool connectOk = false;
    if (wifiManager != nullptr) {
        connectOk = wifiManager->connectToNetwork(ssid, password, WIFI_CONNECT_TIMEOUT_MS);
    }

    JsonDocument resp;

    resp["status"] = connectOk ? "connected" : "error";
    resp["ssid"] = ssid;

    if (connectOk) {
        resp["ip"] = wifiManager->getIP().toString();
        configManager.setWiFi(ssid, password);
        configManager.save();
    }

    if (!connectOk) {
        resp["message"] = "failed to connect";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief WiFi status
 */
void handleWifiStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    bool connected = (wifiManager != nullptr) && WiFiManager::isConnected();

    resp["connected"] = connected;
    resp["ssid"] = connected ? WiFiManager::getConnectedSSID() : "";
    resp["ip"] = connected ? wifiManager->getIP().toString() : "";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Handle OTA start
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleStart(HTTPUpload& upload, int mode) {
    Logger::info((String("OTA start: ") + upload.filename).c_str(), "API::OTA");

    otaError = false;
    otaSize = 0;
    otaStatus = "";
    otaInProgress = true;
    otaCancelRequested = false;
    otaTotal = static_cast<size_t>(upload.contentLength);

    DisplayManager::clearScreen();
    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Uploading...", 2, LCD_WHITE, LCD_BLACK,
                                    true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

    int constexpr security_space = 0x1000;
    u_int constexpr bin_mask = 0xFFFFF000;

    FSInfo fs_info;
    LittleFS.info(fs_info);
    size_t fsSize = fs_info.totalBytes;
    size_t maxSketchSpace =
        (ESP.getFreeSketchSpace() - security_space) &  // NOLINT(readability-static-accessed-through-instance)
        bin_mask;
    size_t place = (mode == U_FS) ? fsSize : maxSketchSpace;

    if (!Update.begin(place, mode)) {
        otaError = true;
        otaStatus = Update.getErrorString();
        Logger::error((String("Update.begin failed: ") + otaStatus).c_str(), "API::OTA");
    }
}

/**
 * @brief Handle OTA write
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleWrite(HTTPUpload& upload) {
    if (!otaError) {
        if (otaCancelRequested) {
            Update.end();
            otaError = true;
            otaStatus = "Update canceled";
            otaInProgress = false;
            Logger::warn("OTA canceled by user", "API::OTA");

            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Canceled", 2, LCD_WHITE, LCD_BLACK,
                                            true);
            DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

            return;
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaError = true;
            otaStatus = Update.getErrorString();
            Logger::error((String("Write failed: ") + otaStatus).c_str(), "API::OTA");
        }

        otaSize += upload.currentSize;

        float progress = 0.0F;
        if (otaTotal > 0) {
            progress = static_cast<float>(otaSize) / static_cast<float>(otaTotal);
        }

        DisplayManager::drawLoadingBar(progress, OTA_LOADING_Y_OFFSET);
    }
}

/**
 * @brief Handle OTA end
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleEnd(HTTPUpload& /*upload*/, int mode) {
    if (!otaError) {
        if (Update.end(true)) {
            if (mode == U_FS) {
                Logger::info("OTA FS update complete, mounting file system...", "API::OTA");
                LittleFS.begin();
            }

            otaStatus = String("Update OK (") + String(otaSize) + " bytes)";
            Logger::info(otaStatus.c_str(), "API::OTA");

            DisplayManager::drawLoadingBar(1.0F, OTA_LOADING_Y_OFFSET);
            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Success!", 2, LCD_WHITE, LCD_BLACK,
                                            true);
        } else {
            otaError = true;
            otaStatus = Update.getErrorString();
        }
    }
}

/**
 * @brief Handle OTA aborted
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleAborted(HTTPUpload& /*upload*/) {
    Update.end();
    otaError = true;
    otaStatus = "Update aborted";
    otaInProgress = false;
    otaCancelRequested = false;

    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Aborted", 2, LCD_WHITE, LCD_BLACK, true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);
}

/**
 * @brief Get recent logs
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray logsArray = doc["logs"].to<JsonArray>();

    size_t count = Logger::getLogCount();
    for (size_t i = 0; i < count; i++) {
        const char* entry = Logger::getLogEntry(i);
        if (entry != nullptr) {
            logsArray.add(entry);
        }
    }

    doc["count"] = count;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Download logs as a text file
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsDownload(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String logs = Logger::getLogsAsString();

    setCorsHeaders(webserver);
    webserver->raw().sendHeader("Content-Disposition", "attachment; filename=\"logs.log\"");
    webserver->raw().send(HTTP_CODE_OK, "text/plain", logs);
}

/**
 * @brief Clear log buffer
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsClear(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    Logger::clearLogs();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Logs cleared";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

static String redactToken(const String& t) {
    if (t.length() < 8) return "";
    return t.substring(0, 4) + "..." + t.substring(t.length() - 4);
}

/**
 * @brief Return cached Clawd state as JSON
 */
void handleClawdStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;

    const ClawdState& s = ClawdManager::state();
    JsonDocument doc;
    doc["configured"] = ClawdManager::isConfigured();
    doc["valid"] = s.valid;
    doc["session_pct"] = s.session_pct;
    doc["session_reset_min"] = s.session_reset_min;
    doc["weekly_pct"] = s.weekly_pct;
    doc["weekly_reset_min"] = s.weekly_reset_min;
    doc["status"] = s.status;
    doc["last_poll_unix"] = static_cast<int64_t>(s.last_poll_unix);
    doc["http_code"] = s.http_code;
    doc["last_error"] = s.last_error;

    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Return Clawd config (token redacted)
 */
void handleClawdConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;

    JsonDocument doc;
    doc["anthropic_token_preview"] = redactToken(configManager.getAnthropicToken());
    doc["has_anthropic_token"] = strlen(configManager.getAnthropicToken()) > 0;
    doc["webhook_url"] = configManager.getWebhookUrl();

    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set Clawd config (anthropic token and/or webhook URL)
 */
void handleClawdConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    if (deserializeJson(ddoc, body)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    bool changed = false;
    if (ddoc["anthropic_token"].is<const char*>()) {
        configManager.setAnthropicToken(ddoc["anthropic_token"].as<const char*>());
        changed = true;
    }
    if (ddoc["webhook_url"].is<const char*>()) {
        configManager.setWebhookUrl(ddoc["webhook_url"].as<const char*>());
        changed = true;
    }

    if (!changed) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "no recognized fields (anthropic_token, webhook_url)";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    configManager.save();

    JsonDocument doc;
    doc["status"] = "ok";
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
    Logger::info("Clawd config updated", "API");
}

/**
 * @brief Force immediate Clawd poll
 */
void handleClawdRefresh(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;

    bool ok = ClawdManager::pollNow();
    ClawdManager::render();

    const ClawdState& s = ClawdManager::state();
    JsonDocument doc;
    doc["status"] = ok ? "ok" : "error";
    doc["http_code"] = s.http_code;
    doc["last_error"] = s.last_error;
    doc["session_pct"] = s.session_pct;
    doc["weekly_pct"] = s.weekly_pct;

    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Forward a shortcut key to the configured webhook URL
 */
void handleClawdShortcut(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    if (body.length() == 0 || deserializeJson(ddoc, body)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    const char* key = ddoc["key"] | "";
    if (strlen(key) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "key field required";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    if (strlen(configManager.getWebhookUrl()) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "webhook_url not configured";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    bool ok = ClawdManager::sendShortcut(String(key));

    JsonDocument doc;
    doc["status"] = ok ? "ok" : "error";
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Accept a usage payload from a host-side proxy (push pattern).
 */
void handleClawdData(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    if (body.length() == 0 || deserializeJson(ddoc, body)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    int s = ddoc["session_pct"] | 0;
    int sr = ddoc["session_reset_min"] | 0;
    int w = ddoc["weekly_pct"] | 0;
    int wr = ddoc["weekly_reset_min"] | 0;
    const char* st = ddoc["status"] | "allowed";

    ClawdManager::setStateFromProxy(s, sr, w, wr, String(st));
    ClawdManager::render();

    JsonDocument doc;
    doc["status"] = "ok";
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

void handleClawdCalibrate(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;
    ClawdManager::drawCalibration();
    JsonDocument doc;
    doc["status"] = "ok";
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

void handleClawdSplash(Webserver* webserver) {
    if (!requireBearerToken(webserver)) return;
    String ip = (wifiManager != nullptr) ? wifiManager->getIP().toString() : String("?");
    DisplayManager::drawStartup(ip);
    JsonDocument doc;
    doc["status"] = "ok";
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}
