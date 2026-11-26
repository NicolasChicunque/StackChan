/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "app_launcher/app_launcher.h"
#include "app_ai_agent/app_ai_agent.h"
#include "app_avatar/app_avatar.h"
#include "app_setup/app_setup.h"
#include "app_espnow_ctrl/app_espnow_ctrl.h"
#include "app_app_center/app_app_center.h"
#include "app_ezdata/app_ezdata.h"
#include "app_dance/app_dance.h"
#if STACKCHAN_ENABLE_APP_MUSIC
#include "app_music_player/app_music_player.h"
#endif
#if STACKCHAN_ENABLE_APP_GEMINI
#include "app_gemini/app_gemini.h"
#endif
#if STACKCHAN_ENABLE_APP_MCP
#include "app_mcp/app_mcp.h"
#endif
#if STACKCHAN_ENABLE_APP_ALARM
#include "app_alarm/app_alarm.h"
#endif
