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
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <functional>
#include <Logger.h>
#include <cstring>
#include <cstdlib>
#include <array>

#include "web/Webserver.h"

static constexpr size_t URI_BUF_SIZE = 192;
static constexpr size_t PATH_BUF_SIZE = 256;
static constexpr bool LOG_STATIC_HIT_INFO = false;

namespace {

struct ContentTypeMapping {
    const char* suffix;
    const char* mimeType;
};

constexpr const char* DEFAULT_CONTENT_TYPE = "application/octet-stream";
constexpr const char* HTML_CONTENT_TYPE = "text/html";

constexpr std::array<ContentTypeMapping, 12> CONTENT_TYPE_MAPPINGS = {{
    {".html", HTML_CONTENT_TYPE},
    {".htm", HTML_CONTENT_TYPE},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".txt", "text/plain"},
}};

auto hasSuffix(const char* value, const char* suffix) -> bool {
    if (value == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t valueLen = strlen(value);
    const size_t suffixLen = strlen(suffix);
    if (valueLen < suffixLen) {
        return false;
    }

    return strcmp(value + valueLen - suffixLen, suffix) == 0;
}

}  // namespace

Webserver::Webserver(uint16_t port) : _server(port) {}

/**
 * @brief Initializes the LittleFS filesystem
 * @param formatIfFailed Whether to format the filesystem if mounting fails
 *
 * @return true if filesystem is mounted false otherwise
 */
auto Webserver::beginFS(bool formatIfFailed) -> bool {
    if (LittleFS.begin()) {
        return true;
    };

    if (formatIfFailed) {
        return LittleFS.begin();
    };

    return false;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
/**
 * @brief Starts the webserver
 *
 * @return void
 */
void Webserver::begin() {
    Logger::info("Starting webserver", "Webserver");
    _server.begin();
}
// NOLINTEND(readability-convert-member-functions-to-static)

/**
 * @brief Handles incoming client requests
 *
 * @return void
 */
void Webserver::handleClient() { _server.handleClient(); }

/**
 * @brief Register a handler for a route
 * @param uri The URI path to handle
 * @param method The HTTP method to handle
 * @param handler The function to call when the route is accessed
 *
 * @return void
 */
void Webserver::on(const String& uri, HTTPMethod method, std::function<void()> handler) {
    _server.on(uri.c_str(), method, [handler]() { handler(); });
}

/**
 * @brief Register a generic handler (all methods)
 * @param uri The URI path to handle
 * @param handler The function to call when the route is accessed
 *
 * @return void
 */
void Webserver::on(const String& uri, std::function<void()> handler) {
    _server.on(uri.c_str(), [handler]() { handler(); });
}

/**
 * @brief Serve a static file from LittleFS using C-strings
 * @param uriC The URL path
 * @param pathC The filesystem path
 * @param contentTypeC The content type to use. If nullptr or empty string, it will be derived from the file extension
 * @param cacheSeconds The number of seconds to cache the file (0 = no-cache)
 *
 * @return void
 */
void Webserver::serveStaticC(const char* uriC, const char* pathC, const char* contentTypeC, int cacheSeconds) {
    _server.on(uriC, HTTP_GET, [this, pathC, contentTypeC, cacheSeconds, uriC]() {
        const char* chosenPath = pathC;
        const char* contentTypeStr = contentTypeC;

        if (!LittleFS.exists(chosenPath)) {
            char msg[320];
            snprintf(msg, sizeof(msg), "File not found: %s", chosenPath);
            Logger::error(msg, "Webserver");
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");

            return;
        }

        File f = LittleFS.open(chosenPath, "r");
        if (!f) {
            char msg[320];
            snprintf(msg, sizeof(msg), "Failed to open file: %s", chosenPath);
            Logger::error(msg, "Webserver");
            _server.send(HTTP_CODE_INTERNAL_ERROR, "text/plain", "Open failed");

            return;
        }

        size_t size = f.size();

        const char* contentTypeResolved = contentTypeStr;
        if (contentTypeResolved == nullptr || contentTypeResolved[0] == '\0') {
            contentTypeResolved = guessContentTypeC(chosenPath);
        }

        if (cacheSeconds > 0) {
            if (cacheSeconds == 86400) {
                _server.sendHeader("Cache-Control", "public, max-age=86400");
            } else {
                char headerVal[64];
                snprintf(headerVal, sizeof(headerVal), "public, max-age=%d", cacheSeconds);
                _server.sendHeader("Cache-Control", String(headerVal));
            }
        } else {
            _server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        }

        _server.setContentLength(size);
        _server.streamFile(f, contentTypeResolved);
        f.close();

        if (LOG_STATIC_HIT_INFO) {
            char infoMsg[320];
            snprintf(infoMsg, sizeof(infoMsg), "Served %s for URI: %s", chosenPath, uriC);
            Logger::info(infoMsg, "Webserver");
        }
    });
}

/**
 * @brief Register all files in a LittleFS directory as static routes
 * @param fsDir The LittleFS directory path
 * @param uriPrefix The URI prefix to use
 * @param contentType The content type to use for all files
 *
 * @return void
 */
void Webserver::registerStaticDir(  // NOLINT(readability-convert-member-functions-to-static)
    const String& fsDir, const String& uriPrefix, const String& contentType) {
    String dirPath = fsDir;
    if (dirPath.endsWith("/") && dirPath.length() > 1) {
        dirPath = dirPath.substring(0, dirPath.length() - 1);
    }

    String prefix = uriPrefix;
    if (prefix.endsWith("/") && prefix.length() > 1) {
        prefix = prefix.substring(0, prefix.length() - 1);
    }

    if (!LittleFS.exists(dirPath)) {
        Logger::warn((String("Static dir not found: ") + dirPath).c_str(), "Webserver");
        return;
    }

    _server.serveStatic(prefix.c_str(), LittleFS, dirPath.c_str(), "max-age=86400");

    String info = String("Registered static dir: ") + prefix + " -> " + dirPath;
    if (!contentType.isEmpty()) {
        info += String(" (ct=") + contentType + ")";
    }
    Logger::info(info.c_str(), "Webserver");
}

/**
 * @brief Register a generic static fallback route using onNotFound
 *
 * Serves GET requests from fsBasePath + request URI. API routes are excluded
 * Can optionally exclude '/' to keep an explicit root route
 */
void Webserver::registerGenericStaticFallback(  // NOLINT(readability-convert-member-functions-to-static)
    const String& fsBasePath, bool excludeRoot) {
    String basePath = fsBasePath;
    if (basePath.endsWith("/") && basePath.length() > 1) {
        basePath = basePath.substring(0, basePath.length() - 1);
    }

    _server.onNotFound([this, basePath, excludeRoot]() {
        if (_server.method() == HTTP_OPTIONS) {
            _server.sendHeader("Access-Control-Allow-Origin", "*");
            _server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            _server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            _server.sendHeader("Access-Control-Max-Age", "3600");
            _server.send(HTTP_CODE_OK);
            return;
        }

        if (_server.method() != HTTP_GET) {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        const String& uri = _server.uri();

        if (excludeRoot && uri == "/") {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        if (uri.startsWith("/api/")) {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        char uriBuf[URI_BUF_SIZE] = {0};
        char fsPath[PATH_BUF_SIZE] = {0};
        char chosenPath[PATH_BUF_SIZE] = {0};

        strncpy(uriBuf, uri.c_str(), sizeof(uriBuf) - 1);

        if (snprintf(fsPath, sizeof(fsPath), "%s%s", basePath.c_str(), uriBuf) <= 0) {
            _server.send(HTTP_CODE_INTERNAL_ERROR, "text/plain", "Path error");
            return;
        }

        if (LittleFS.exists(fsPath)) {
            strncpy(chosenPath, fsPath, sizeof(chosenPath) - 1);
        } else {
            _server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Not found");
            return;
        }

        File f = LittleFS.open(chosenPath, "r");
        if (!f) {
            _server.send(HTTP_CODE_INTERNAL_ERROR, "text/plain", "Open failed");
            return;
        }

        _server.sendHeader("Cache-Control", "public, max-age=86400");

        _server.setContentLength(f.size());
        _server.streamFile(f, guessContentTypeC(fsPath));
        f.close();

        if (LOG_STATIC_HIT_INFO) {
            char infoMsg[320];
            snprintf(infoMsg, sizeof(infoMsg), "Served %s for URI: %s", chosenPath, uriBuf);
            Logger::info(infoMsg, "Webserver");
        }
    });
}

/**
 * @brief Simple notFound handler registration
 * @param handler The function to call when a route is not found
 *
 * @return void
 */
void Webserver::onNotFound(std::function<void()> handler) {
    _server.onNotFound([handler]() { handler(); });
}

/**
 * @brief Expose underlying server where advanced config is needed
 *
 * @return reference to the underlying ESP8266WebServer
 */
auto Webserver::raw() -> ESP8266WebServer& { return _server; }

auto Webserver::guessContentTypeC(const char* path) -> const char* {
    if (path == nullptr || path[0] == '\0') {
        return DEFAULT_CONTENT_TYPE;
    }

    const size_t len = strlen(path);
    if (path[len - 1] == '/') {
        return HTML_CONTENT_TYPE;
    }

    for (const auto& mapping : CONTENT_TYPE_MAPPINGS) {
        if (hasSuffix(path, mapping.suffix)) {
            return mapping.mimeType;
        }
    }

    return DEFAULT_CONTENT_TYPE;
}
