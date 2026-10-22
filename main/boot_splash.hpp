// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

namespace stackchan::app::boot_splash {

// Draw the Google Cloud startup logo on `display` (board-native resolution)
// and record the show timestamp. No-op when CONFIG_STACKCHAN_BOOT_SPLASH is
// unset. Safe to call immediately after Board::begin().
void show(M5GFX& display, bool circular_display);

// Block until the minimum splash time has elapsed (CONFIG_STACKCHAN_BOOT_SPLASH_MS,
// default 2000). Call just before the face render task takes over the panel.
void wait_min_time();

} // namespace stackchan::app::boot_splash
