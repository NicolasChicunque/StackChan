/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <hal/hal_toggles.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    const bool skip_mooncake = GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;

    if (!skip_mooncake) {
        // Install apps
        const int launcher_app_id = GetMooncake().installApp(std::make_unique<AppLauncher>());
        hal_toggles::register_app_id(hal_toggles::App::AiAgent,
                                     GetMooncake().installApp(std::make_unique<AppAiAgent>()));
        hal_toggles::register_app_id(hal_toggles::App::Avatar, GetMooncake().installApp(std::make_unique<AppAvatar>()));
        hal_toggles::register_app_id(hal_toggles::App::EspnowRemote,
                                     GetMooncake().installApp(std::make_unique<AppEspnowControl>()));
        hal_toggles::register_app_id(hal_toggles::App::AppCenter,
                                     GetMooncake().installApp(std::make_unique<AppAppCenter>()));
        hal_toggles::register_app_id(hal_toggles::App::Ezdata, GetMooncake().installApp(std::make_unique<AppEzdata>()));
        hal_toggles::register_app_id(hal_toggles::App::Dance, GetMooncake().installApp(std::make_unique<AppDance>()));
#if STACKCHAN_ENABLE_APP_MUSIC
        hal_toggles::register_app_id(hal_toggles::App::Music,
                                     GetMooncake().installApp(std::make_unique<AppMusicPlayer>()));
#endif
#if STACKCHAN_ENABLE_APP_GEMINI
        hal_toggles::register_app_id(hal_toggles::App::Gemini, GetMooncake().installApp(std::make_unique<AppGemini>()));
#endif
#if STACKCHAN_ENABLE_APP_MCP
        hal_toggles::register_app_id(hal_toggles::App::Mcp, GetMooncake().installApp(std::make_unique<AppMcp>()));
#endif
#if STACKCHAN_ENABLE_APP_ALARM
        const int alarm_app_id = GetMooncake().installApp(std::make_unique<AppAlarm>());
        hal_toggles::register_app_id(hal_toggles::App::Alarm, alarm_app_id);
#endif
        GetMooncake().installApp(std::make_unique<AppSetup>());

#if STACKCHAN_ENABLE_APP_ALARM
        // Start background alarm scheduler (index in launcher list, excluding the launcher itself)
        {
            int alarm_page_index = 0;
            int idx              = 0;
            for (const auto& props : GetMooncake().getAllAppProps()) {
                if (props.appID == launcher_app_id) {
                    continue;
                }
                if (props.appID == alarm_app_id) {
                    alarm_page_index = idx;
                    break;
                }
                ++idx;
            }
            AlarmService::instance().start(alarm_page_index, alarm_app_id);
        }
#else
        (void)launcher_app_id;
#endif

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
