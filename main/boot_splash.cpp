// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "boot_splash.hpp"

#include "sdkconfig.h"

#include <cstdint>

#include <M5GFX.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app::boot_splash {

namespace {
constexpr const char* kTag = "boot-splash";

#if CONFIG_STACKCHAN_BOOT_SPLASH
extern "C" {
extern const std::uint8_t splash_cores3_png_start[] asm("_binary_splash_cores3_png_start");
extern const std::uint8_t splash_cores3_png_end[] asm("_binary_splash_cores3_png_end");
extern const std::uint8_t splash_atom_png_start[] asm("_binary_splash_atom_png_start");
extern const std::uint8_t splash_atom_png_end[] asm("_binary_splash_atom_png_end");
extern const std::uint8_t splash_stopwatch_png_start[] asm("_binary_splash_stopwatch_png_start");
extern const std::uint8_t splash_stopwatch_png_end[] asm("_binary_splash_stopwatch_png_end");
}

std::int64_t g_shown_us = 0;

void blit(M5GFX& display, const std::uint8_t* start, const std::uint8_t* end)
{
    const std::size_t len = static_cast<std::size_t>(end - start);
    display.startWrite();
    display.fillScreen(TFT_WHITE);
    display.drawPng(start, len, 0, 0);
    display.endWrite();
    display.display();
}
#endif
} // namespace

void show(M5GFX& display, bool /*circular_display*/)
{
#if CONFIG_STACKCHAN_BOOT_SPLASH
    const int w = display.width();
    if (w >= 400) {
        blit(display, splash_stopwatch_png_start, splash_stopwatch_png_end);
    } else if (w >= 240) {
        blit(display, splash_cores3_png_start, splash_cores3_png_end);
    } else {
        blit(display, splash_atom_png_start, splash_atom_png_end);
    }
    g_shown_us = esp_timer_get_time();
    ESP_LOGI(kTag, "Google Cloud splash %dx%d", display.width(), display.height());
#else
    (void)display;
#endif
}

void wait_min_time()
{
#if CONFIG_STACKCHAN_BOOT_SPLASH
    if (g_shown_us == 0) return;
#ifdef CONFIG_STACKCHAN_BOOT_SPLASH_MS
    const std::int64_t min_us = static_cast<std::int64_t>(CONFIG_STACKCHAN_BOOT_SPLASH_MS) * 1000;
#else
    const std::int64_t min_us = 2000 * 1000;
#endif
    const std::int64_t elapsed = esp_timer_get_time() - g_shown_us;
    if (elapsed < min_us) {
        const std::uint32_t remain_ms =
            static_cast<std::uint32_t>((min_us - elapsed + 999) / 1000);
        vTaskDelay(pdMS_TO_TICKS(remain_ms));
    }
#endif
}

} // namespace stackchan::app::boot_splash
