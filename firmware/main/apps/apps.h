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
#if STACKCHAN_ENABLE_APP_FX
#include "app_fx/app_fx.h"
#endif
#if STACKCHAN_ENABLE_APP_HEALTH
#include "app_health/app_health.h"
#endif
#if STACKCHAN_ENABLE_APP_GAMES
#include "app_games/app_games.h"
#endif
#if STACKCHAN_ENABLE_APP_DECK
#include "app_deck/app_deck.h"
#endif
#if STACKCHAN_ENABLE_APP_NFC
#include "app_nfc/app_nfc.h"
#endif
#if STACKCHAN_ENABLE_APP_HOME
#include "app_home/app_home.h"
#endif
#if STACKCHAN_ENABLE_APP_CALENDAR
#include "app_calendar/app_calendar.h"
#endif
#if STACKCHAN_ENABLE_APP_ENGLISH
#include "app_english/app_english.h"
#endif
#if STACKCHAN_ENABLE_APP_TRAKT
#include "app_trakt/app_trakt.h"
#endif
#if STACKCHAN_ENABLE_APP_FILES
#include "app_files/app_files.h"
#endif
#if STACKCHAN_ENABLE_APP_COMPANION
#include "app_companion/app_companion.h"
#endif
#if STACKCHAN_ENABLE_APP_SENTINEL
#include "app_sentinel/app_sentinel.h"
#endif
