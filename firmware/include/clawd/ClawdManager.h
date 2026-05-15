// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Clawdmeter for GeekMagic Open Firmware
 * Polls Anthropic API rate-limit headers and draws Claude Code usage
 * on a 240x240 ST7789. Companion module to the existing GeekMagic stack.
 */

#ifndef CLAWD_MANAGER_H
#define CLAWD_MANAGER_H

#include <Arduino.h>

struct ClawdState {
    bool valid = false;
    int session_pct = 0;
    int session_reset_min = 0;
    int weekly_pct = 0;
    int weekly_reset_min = 0;
    String status = "unknown";
    String last_error;
    time_t last_poll_unix = 0;
    int http_code = 0;
};

class ClawdManager {
   public:
    static void begin();
    static void loop();
    static bool pollNow();
    static const ClawdState& state();
    static void render();
    static bool sendShortcut(const String& key);
    static bool isConfigured();
    static void setStateFromProxy(int session_pct, int session_reset_min, int weekly_pct,
                                  int weekly_reset_min, const String& status);
    static void drawCalibration();
};

#endif  // CLAWD_MANAGER_H
