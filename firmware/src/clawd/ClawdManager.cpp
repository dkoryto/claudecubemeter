// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Clawdmeter for GeekMagic Open Firmware
 */

#include "clawd/ClawdManager.h"

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>

#include "config/ConfigManager.h"
#include "display/DisplayManager.h"
#include <Arduino_GFX_Library.h>
#include <Logger.h>

extern ConfigManager configManager;

namespace {

constexpr unsigned long POLL_INTERVAL_MS = 60UL * 1000UL;
constexpr unsigned long FIRST_POLL_DELAY_MS = 5UL * 1000UL;
constexpr unsigned long SPLASH_HOLD_MS = 5UL * 1000UL;
// Earliest plausible time after NTP sync (2024-01-01); anything below means
// time is not set and reset-minutes math would be meaningless.
constexpr time_t REASONABLE_EPOCH = 1704067200L;
// Buffer sizes depend on whether the server supports MFLN. probeMaxFragmentLength()
// negotiates a smaller TLS record size with the server; if that works we use 1k
// receive buffer (heap-friendly). If the server refuses we fall back to a full
// 16k buffer — heap-risky on this MCU but unavoidable for a TLS 1.2 record.
constexpr uint16_t TLS_RX_BUFFER_SMALL = 1024;
constexpr uint16_t TLS_RX_BUFFER_LARGE = 16384;
constexpr uint16_t TLS_TX_BUFFER = 512;
constexpr const char* API_HOST = "api.anthropic.com";
constexpr uint16_t API_PORT = 443;

ClawdState g_state;
unsigned long g_next_poll_ms = 0;
bool g_first_poll_pending = true;
bool g_rendered_once = false;
unsigned long g_splash_until_ms = 0;

const char* HEADER_KEYS[] = {
    "anthropic-ratelimit-unified-5h-utilization",
    "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-5h-status",
    "anthropic-ratelimit-unified-7d-utilization",
    "anthropic-ratelimit-unified-7d-reset",
};

constexpr size_t HEADER_COUNT = sizeof(HEADER_KEYS) / sizeof(HEADER_KEYS[0]);

constexpr const char* REQUEST_BODY =
    R"({"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]})";

int pctFromHeader(const String& v) {
    if (v.length() == 0) return 0;
    float f = v.toFloat();
    int p = static_cast<int>(f * 100.0F + 0.5F);
    if (p < 0) return 0;
    if (p > 100) return 100;
    return p;
}

int minutesUntil(const String& reset_ts) {
    if (reset_ts.length() == 0) return 0;
    time_t now = time(nullptr);
    if (now < REASONABLE_EPOCH) return 0;
    long target = reset_ts.toInt();
    long mins = (target - static_cast<long>(now) + 30) / 60;
    if (mins < 0) return 0;
    if (mins > 99999) return 99999;
    return static_cast<int>(mins);
}

uint16_t colorForPct(int pct) {
    if (pct >= 90) return 0xF800;  // red
    if (pct >= 70) return 0xFD20;  // orange
    if (pct >= 40) return 0xFFE0;  // yellow
    return 0x07E0;                  // green
}

String formatResetTime(int minutes) {
    if (minutes <= 0) return "—";
    if (minutes < 60) return String(minutes) + "m";
    int hours = minutes / 60;
    int rem_m = minutes % 60;
    if (hours < 24) {
        if (rem_m == 0) return String(hours) + "h";
        return String(hours) + "h " + String(rem_m) + "m";
    }
    int days = hours / 24;
    int rem_h = hours % 24;
    if (rem_h == 0) return String(days) + "d";
    return String(days) + "d " + String(rem_h) + "h";
}

void drawBar(Arduino_GFX* gfx, int16_t x, int16_t y, int16_t w, int16_t h, int pct, uint16_t fg) {
    constexpr uint16_t BAR_BG = 0x39E7;
    gfx->fillRect(x, y, w, h, BAR_BG);
    int fill = (w * pct) / 100;
    if (fill < 0) fill = 0;
    if (fill > w) fill = w;
    if (fill > 0) gfx->fillRect(x, y, fill, h, fg);
    gfx->drawRect(x, y, w, h, 0xFFFF);
}

void drawCenteredText(Arduino_GFX* gfx, int16_t y, const String& text, uint8_t size, uint16_t fg, uint16_t bg) {
    int16_t w = static_cast<int16_t>(text.length()) * 6 * size;
    int16_t x = (LCD_W - w) / 2;
    if (x < 0) x = 0;
    gfx->setTextSize(size);
    gfx->setTextColor(fg, bg);
    gfx->setCursor(x, y);
    gfx->print(text);
}

constexpr int16_t HEADER_H = 34;
constexpr int16_t FOOTER_Y = 213;

// Europe/Warsaw UTC offset in seconds (CET=+3600, CEST=+7200). Day-of-month
// rules approximate the last-Sunday transitions (good to within ~1 day around
// the change). Avoids depending on newlib TZ which ESP8266 SNTP overwrites.
int warsawOffsetSec(const struct tm& utc) {
    int m = utc.tm_mon + 1;
    int d = utc.tm_mday;
    if (m < 3 || m > 10) return 3600;
    if (m > 3 && m < 10) return 7200;
    if (m == 3) return (d >= 25) ? 7200 : 3600;
    return (d < 25) ? 7200 : 3600;  // October
}

void renderHeader(Arduino_GFX* gfx) {
    gfx->fillRect(0, 0, LCD_W, HEADER_H, 0x0000);
    // Title size 2 centered (15 chars * 12 = 180 px wide)
    const char* title = "ClaudeCubeMeter";
    int16_t title_w = static_cast<int16_t>(strlen(title)) * 6 * 2;
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setCursor((LCD_W - title_w) / 2, 2);
    gfx->print(title);

    time_t now = time(nullptr);
    if (now > REASONABLE_EPOCH) {
        struct tm utc;
        gmtime_r(&now, &utc);
        time_t local = now + warsawOffsetSec(utc);
        struct tm tm_local;
        gmtime_r(&local, &tm_local);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_local.tm_hour, tm_local.tm_min,
                 tm_local.tm_sec);
        int16_t clock_w = 8 * 6;
        gfx->setTextSize(1);
        gfx->setTextColor(0x8410, 0x0000);
        gfx->setCursor((LCD_W - clock_w) / 2, 22);
        gfx->print(buf);
    }
    gfx->drawFastHLine(10, HEADER_H - 2, LCD_W - 20, 0x39E7);
}

String formatAgo(time_t last) {
    if (last <= 0) return "(never)";
    time_t now = time(nullptr);
    if (now <= last) return "0s ago";
    long delta = now - last;
    if (delta < 60) return String(delta) + "s ago";
    if (delta < 3600) return String(delta / 60) + "m ago";
    return String(delta / 3600) + "h ago";
}

void renderFooter(Arduino_GFX* gfx, const ClawdState& s, bool draw_status) {
    gfx->fillRect(0, FOOTER_Y, LCD_W, LCD_H - FOOTER_Y, 0x0000);
    gfx->drawFastHLine(10, FOOTER_Y - 1, LCD_W - 20, 0x39E7);
    uint16_t status_color = 0x07E0;
    if (s.status == "allowed_warning") status_color = 0xFD20;
    else if (s.status != "allowed" && s.status != "ok") status_color = 0xF800;
    gfx->fillCircle(14, 224, 3, status_color);
    gfx->setTextSize(1);
    gfx->setTextColor(0xBDF7, 0x0000);
    gfx->setCursor(22, 220);
    if (draw_status) {
        gfx->print(s.status);
        gfx->print(" - ");
        gfx->print(formatAgo(s.last_poll_unix));
    } else {
        gfx->print("waiting...");
    }
}

}  // namespace

void ClawdManager::begin() {
    g_next_poll_ms = millis() + FIRST_POLL_DELAY_MS;
    g_first_poll_pending = true;
    g_splash_until_ms = millis() + SPLASH_HOLD_MS;
    // Poland CET / CEST with DST rules (POSIX TZ): standard offset +1, DST +2
    // (last Sunday of March 02:00 → last Sunday of October 03:00).
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    Logger::info("ClawdManager initialized (TZ=Europe/Warsaw)", "Clawd");
}

bool ClawdManager::isConfigured() {
    const char* tok = configManager.getAnthropicToken();
    return tok != nullptr && strlen(tok) > 0;
}

void ClawdManager::drawCalibration() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    if (gfx == nullptr) return;
    int16_t w = gfx->width();
    int16_t h = gfx->height();
    gfx->fillScreen(0x0000);
    // 1-px border on the FULL claimed area (white)
    gfx->drawRect(0, 0, w, h, 0xFFFF);
    // Inner 1-px border at 10px inset (yellow) — known-visible safe area check
    gfx->drawRect(10, 10, w - 20, h - 20, 0xFFE0);
    // Tick marks every 20px on all 4 edges (cyan)
    for (int16_t i = 0; i < w; i += 20) {
        gfx->drawFastVLine(i, 0, 4, 0x07FF);
        gfx->drawFastVLine(i, h - 4, 4, 0x07FF);
    }
    for (int16_t j = 0; j < h; j += 20) {
        gfx->drawFastHLine(0, j, 4, 0x07FF);
        gfx->drawFastHLine(w - 4, j, 4, 0x07FF);
    }
    // Center crosshair (red)
    gfx->drawFastHLine(w / 2 - 10, h / 2, 20, 0xF800);
    gfx->drawFastVLine(w / 2, h / 2 - 10, 20, 0xF800);
    // Corner labels
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setCursor(2, 2);
    gfx->print("0,0");
    gfx->setCursor(w - 36, 2);
    gfx->print(String(w - 1) + ",0");
    gfx->setCursor(2, h - 10);
    gfx->print("0," + String(h - 1));
    gfx->setCursor(w - 50, h - 10);
    gfx->print(String(w - 1) + "," + String(h - 1));
    // Dimensions label centered
    gfx->setTextSize(2);
    gfx->setCursor(w / 2 - 36, h / 2 + 15);
    gfx->print(String(w) + "x" + String(h));
    Logger::info(("Calibration drawn at " + String(w) + "x" + String(h)).c_str(), "Clawd");
}

void ClawdManager::setStateFromProxy(int session_pct, int session_reset_min, int weekly_pct,
                                     int weekly_reset_min, const String& status) {
    g_state.session_pct = session_pct;
    g_state.session_reset_min = session_reset_min;
    g_state.weekly_pct = weekly_pct;
    g_state.weekly_reset_min = weekly_reset_min;
    g_state.status = status.length() > 0 ? status : String("ok");
    g_state.last_poll_unix = time(nullptr);
    g_state.last_error = "";
    g_state.http_code = 200;
    g_state.valid = true;
    // Push out the next direct-polling attempt; proxy is authoritative now.
    g_next_poll_ms = millis() + POLL_INTERVAL_MS;
    g_rendered_once = false;
}

const ClawdState& ClawdManager::state() { return g_state; }

void ClawdManager::loop() {
    unsigned long now = millis();

    // Keep splash visible after boot until SPLASH_HOLD_MS elapses.
    if (static_cast<long>(now - g_splash_until_ms) < 0) {
        return;
    }

    // 1 Hz refresh of header (clock) and footer (last-poll-ago).
    static unsigned long last_tick_ms = 0;
    if (g_rendered_once && now - last_tick_ms >= 1000UL) {
        last_tick_ms = now;
        Arduino_GFX* gfx = DisplayManager::getGfx();
        if (gfx != nullptr && g_state.valid) {
            renderHeader(gfx);
            renderFooter(gfx, g_state, true);
        }
    }

    // Initial render once splash hold is over: either "no token / waiting" or
    // the cached state if a proxy already pushed data.
    if (!g_rendered_once) {
        render();
        g_rendered_once = true;
    }
    // Auto direct-polling is intentionally disabled on this ESP8266 port —
    // BearSSL requires ~17 KB heap for the api.anthropic.com TLS handshake
    // and we run with ~22 KB free, which OOMs reliably. The companion
    // claude_proxy.py on the host polls Anthropic and POSTs to /clawd/data.
    // To force a direct poll attempt (e.g. for testing on a heap-rich port)
    // call POST /api/v1/clawd/refresh.
}


bool ClawdManager::pollNow() {
    const char* token = configManager.getAnthropicToken();
    if (token == nullptr || strlen(token) == 0) {
        g_state.last_error = "no token configured";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        g_state.last_error = "WiFi disconnected";
        return false;
    }

    static bool mfln_known = false;
    static uint16_t mfln_size = 0;
    if (!mfln_known) {
        for (uint16_t s : {512, 1024, 2048, 4096}) {
            if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(API_HOST, API_PORT, s)) {
                mfln_size = s;
                break;
            }
        }
        mfln_known = true;
        Logger::info(mfln_size > 0
                         ? (String("MFLN supported up to ") + String(mfln_size) + "B").c_str()
                         : "MFLN not supported, will use 16KB buffer",
                     "Clawd");
    }

    uint32_t free_before = ESP.getFreeHeap();        // NOLINT(readability-static-accessed-through-instance)
    uint32_t maxblk = ESP.getMaxFreeBlockSize();     // NOLINT(readability-static-accessed-through-instance)
    BearSSL::WiFiClientSecure tls;
    tls.setInsecure();
    uint16_t rx_buf = (mfln_size > 0) ? mfln_size : TLS_RX_BUFFER_LARGE;
    tls.setBufferSizes(rx_buf, TLS_TX_BUFFER);
    Logger::info(("heap=" + String(free_before) + "B contig=" + String(maxblk) + "B rxbuf=" +
                  String(rx_buf) + "B")
                     .c_str(),
                 "Clawd");

    HTTPClient http;
    http.setTimeout(15000);
    http.setReuse(false);

    if (!http.begin(tls, "https://api.anthropic.com/v1/messages")) {
        g_state.last_error = "http.begin failed";
        Logger::error("HTTPClient begin failed", "Clawd");
        return false;
    }

    http.collectHeaders(HEADER_KEYS, HEADER_COUNT);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "oauth-2025-04-20");
    http.addHeader("User-Agent", "claude-code/2.1.5");
    http.addHeader("Authorization", String("Bearer ") + token);

    int code = http.POST(reinterpret_cast<const uint8_t*>(REQUEST_BODY), strlen(REQUEST_BODY));
    g_state.http_code = code;

    Logger::info(("POST /v1/messages -> " + String(code)).c_str(), "Clawd");

    if (code <= 0) {
        g_state.last_error = http.errorToString(code);
        http.end();
        Logger::error(("Request failed: " + g_state.last_error).c_str(), "Clawd");
        return false;
    }

    String s_util = http.header(HEADER_KEYS[0]);
    String s_reset = http.header(HEADER_KEYS[1]);
    String s_status = http.header(HEADER_KEYS[2]);
    String w_util = http.header(HEADER_KEYS[3]);
    String w_reset = http.header(HEADER_KEYS[4]);

    http.end();

    if (code == 401 || code == 403) {
        g_state.last_error = "unauthorized — token expired?";
        g_state.valid = false;
        return false;
    }

    // Note: Anthropic returns rate-limit headers even on 429 / errors when
    // the request itself was parsable. We accept anything 2xx-4xx that
    // returned headers.
    if (s_util.length() == 0 && w_util.length() == 0) {
        g_state.last_error = String("no rate-limit headers (HTTP ") + code + ")";
        return false;
    }

    g_state.session_pct = pctFromHeader(s_util);
    g_state.session_reset_min = minutesUntil(s_reset);
    g_state.weekly_pct = pctFromHeader(w_util);
    g_state.weekly_reset_min = minutesUntil(w_reset);
    g_state.status = s_status.length() > 0 ? s_status : String("ok");
    g_state.last_poll_unix = time(nullptr);
    g_state.last_error = "";
    g_state.valid = true;

    return true;
}

bool ClawdManager::sendShortcut(const String& key) {
    const char* url = configManager.getWebhookUrl();
    if (url == nullptr || strlen(url) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    String body = String("{\"key\":\"") + key + "\"}";

    HTTPClient http;
    http.setTimeout(5000);
    http.setReuse(false);
    WiFiClient plain;
    BearSSL::WiFiClientSecure tls;

    bool ok;
    String url_s = url;
    if (url_s.startsWith("https://")) {
        tls.setInsecure();
        tls.setBufferSizes(TLS_RX_BUFFER_LARGE, TLS_TX_BUFFER);
        ok = http.begin(tls, url_s);
    } else {
        ok = http.begin(plain, url_s);
    }
    if (!ok) {
        Logger::error("Webhook begin failed", "Clawd");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    http.end();
    Logger::info(("Webhook " + key + " -> " + String(code)).c_str(), "Clawd");
    return code > 0 && code < 400;
}

void ClawdManager::render() {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    if (gfx == nullptr) return;

    gfx->fillScreen(0x0000);

    if (!isConfigured()) {
        drawCenteredText(gfx, 80, "Claude Code", 2, 0xFFFF, 0x0000);
        drawCenteredText(gfx, 120, "no token", 2, 0xF800, 0x0000);
        drawCenteredText(gfx, 150, "open /clawd.html", 1, 0x8410, 0x0000);
        return;
    }

    if (!g_state.valid) {
        drawCenteredText(gfx, 70, "ClaudeCubeMeter", 2, 0xFFFF, 0x0000);
        if (g_state.last_error.length() == 0 && g_state.http_code == 0) {
            // No poll attempted yet, no proxy push yet.
            drawCenteredText(gfx, 110, "waiting for", 2, 0xFFE0, 0x0000);
            drawCenteredText(gfx, 135, "proxy data", 2, 0xFFE0, 0x0000);
            drawCenteredText(gfx, 170, "run claude_proxy.py", 1, 0x8410, 0x0000);
        } else {
            drawCenteredText(gfx, 110, "polling failed", 2, 0xF800, 0x0000);
            drawCenteredText(gfx, 140, g_state.last_error, 1, 0x8410, 0x0000);
            if (g_state.http_code != 0) {
                drawCenteredText(gfx, 160, "HTTP " + String(g_state.http_code), 1, 0x8410, 0x0000);
            }
        }
        return;
    }

    renderHeader(gfx);

    // Session block
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410, 0x0000);
    gfx->setCursor(10, 36);
    gfx->print("5h SESSION");
    String s_pct_str = String(g_state.session_pct) + "%";
    drawCenteredText(gfx, 50, s_pct_str, 5, colorForPct(g_state.session_pct), 0x0000);
    drawBar(gfx, 10, 92, LCD_W - 20, 10, g_state.session_pct, colorForPct(g_state.session_pct));
    gfx->setTextSize(1);
    gfx->setTextColor(0xBDF7, 0x0000);
    gfx->setCursor(10, 108);
    gfx->print("resets in ");
    gfx->print(formatResetTime(g_state.session_reset_min));

    // Weekly block
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410, 0x0000);
    gfx->setCursor(10, 128);
    gfx->print("7d WEEKLY");
    drawCenteredText(gfx, 138, String(g_state.weekly_pct) + "%", 3, colorForPct(g_state.weekly_pct), 0x0000);
    drawBar(gfx, 10, 168, LCD_W - 20, 8, g_state.weekly_pct, colorForPct(g_state.weekly_pct));
    gfx->setTextSize(1);
    gfx->setTextColor(0xBDF7, 0x0000);
    gfx->setCursor(10, 180);
    gfx->print("resets in ");
    gfx->print(formatResetTime(g_state.weekly_reset_min));

    renderFooter(gfx, g_state, true);
}
