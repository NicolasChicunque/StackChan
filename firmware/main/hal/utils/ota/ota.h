/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool start_ota_update(const char* url, void (*on_progress)(int progress));
bool start_ota_from_file(const char* filepath, void (*on_progress)(int progress));

#ifdef __cplusplus
}
#endif
