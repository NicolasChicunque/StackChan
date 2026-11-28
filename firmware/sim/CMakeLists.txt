cmake_minimum_required(VERSION 3.20)
project(stackchan_sim LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------------------
# SDL2: auto-fetch prebuilt VC package on Windows, or find_package on Unix
# ------------------------------------------------------------------------------
if(WIN32)
    include(FetchContent)
    FetchContent_Declare(
        sdl2_prebuilt
        URL "https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-VC.zip"
        URL_HASH SHA256=6e524b5f0e3ecd9fe3b439336b5e7c3fe597e8823e8e62c8e3074d335e9ca7d9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sdl2_prebuilt)

    set(SDL2_INCLUDE_DIR "${sdl2_prebuilt_SOURCE_DIR}/include")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SDL2_LIB_DIR "${sdl2_prebuilt_SOURCE_DIR}/lib/x64")
    else()
        set(SDL2_LIB_DIR "${sdl2_prebuilt_SOURCE_DIR}/lib/x86")
    endif()

    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${SDL2_LIB_DIR}/SDL2.dll"
        IMPORTED_IMPLIB "${SDL2_LIB_DIR}/SDL2.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}"
    )
    set(SDL2_DLL_PATH "${SDL2_LIB_DIR}/SDL2.dll")
else()
    find_package(SDL2 REQUIRED)
endif()

# ------------------------------------------------------------------------------
# LVGL 9.5 (reusing firmware/managed_components/lvgl__lvgl directly)
# ------------------------------------------------------------------------------
set(LV_BUILD_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl" "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
target_link_libraries(lvgl PUBLIC SDL2::SDL2)
target_compile_definitions(lvgl PUBLIC SDL_MAIN_HANDLED)

if(MSVC)
    # Silence noisy third-party C warnings inside LVGL on MSVC
    target_compile_options(lvgl PRIVATE /W0 /utf-8)
endif()

# ------------------------------------------------------------------------------
# StackChan Desktop UI Simulator executable
# ------------------------------------------------------------------------------
set(SIM_SOURCES
    sim_main.cpp
    sim_app_music.cpp
    sim_app_alarm.cpp
    sim_app_gemini.cpp
    sim_app_speaker.cpp
)

if(WIN32)
    add_executable(stackchan_sim WIN32 ${SIM_SOURCES})
    if(MSVC)
        set_target_properties(stackchan_sim PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()
else()
    add_executable(stackchan_sim ${SIM_SOURCES})
endif()

target_include_directories(stackchan_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(stackchan_sim PRIVATE
    lvgl::lvgl
    SDL2::SDL2
)

if(MSVC)
    target_compile_options(stackchan_sim PRIVATE /W3 /utf-8 /EHsc)
endif()

if(WIN32 AND DEFINED SDL2_DLL_PATH)
    add_custom_command(TARGET stackchan_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_DLL_PATH}"
            "$<TARGET_FILE_DIR:stackchan_sim>/SDL2.dll"
        COMMENT "Copying SDL2.dll next to stackchan_sim.exe"
    )
endif()
