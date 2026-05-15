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

#ifndef API_H
#define API_H

#include "web/Webserver.h"

void setCorsHeaders(Webserver* webserver);
void registerApiEndpoints(Webserver* webserver);
void handleOtaUpload(Webserver* webserver, int mode);
void handleOtaFinished(Webserver* webserver);
void handleReboot(Webserver* webserver);
void handleOtaStatus(Webserver* webserver);
void handleOtaCancel(Webserver* webserver);

void handleGifUpload(Webserver* webserver);
void handleListGifs(Webserver* webserver);
void handlePlayGif(Webserver* webserver);
void handleStopGif(Webserver* webserver);

void handleWifiScan(Webserver* webserver);
void handleWifiConnect(Webserver* webserver);
void handleWifiStatus(Webserver* webserver);

void handleNtpSync(Webserver* webserver);
void handleNtpStatus(Webserver* webserver);
void handleNtpConfigGet(Webserver* webserver);
void handleNtpConfigSet(Webserver* webserver);
void handleDisplayRotationGet(Webserver* webserver);
void handleDisplayRotationSet(Webserver* webserver);

void handleTokenCheck(Webserver* webserver);
void handleTokenSave(Webserver* webserver);

void handleLogsGet(Webserver* webserver);
void handleLogsDownload(Webserver* webserver);
void handleLogsClear(Webserver* webserver);

void handleClawdStatus(Webserver* webserver);
void handleClawdConfigGet(Webserver* webserver);
void handleClawdConfigSet(Webserver* webserver);
void handleClawdRefresh(Webserver* webserver);
void handleClawdShortcut(Webserver* webserver);
void handleClawdData(Webserver* webserver);
void handleClawdCalibrate(Webserver* webserver);
void handleClawdSplash(Webserver* webserver);

#endif  // API_H
