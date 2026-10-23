// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <functional>

#include <config_service/config_service.hpp>

#include "avatar/canvas.hpp"

namespace stackchan::app::first_run {

// Initialise the Gemini-only setup gate. When `required` is true the prompt
// owns the panel until the user starts setup or skips it. The HTTP route
// watcher is created here; routes become live once the SoftAP HTTP server is
// up.
void init(bool required, const config::DeviceConfig& cfg, bool button_only);

// Register the one-shot continuation invoked when the user skips setup. This
// lets app_main defer conversation-task startup until the avatar is actually
// going to be shown. Safe to call before or after init().
void set_ready_callback(std::function<void()> callback);

// Screen adapter hooks used by main/screens.cpp.
bool screen_active();
bool draw(avatar::RichCanvas& canvas);
bool handle_tap(int x, int y);
void poll_button();

// Leave setup and return to the normal avatar UI. If the first-run SoftAP is
// active this tears it down first and only reports success after the AP has
// stopped. Safe to call from the prompt or the on-device QR page.
bool skip_setup();

// True while the first-run prompt or setup flow owns boot. app_main uses this
// to suppress conversation startup until Skip. Completion reboots.
bool active();

} // namespace stackchan::app::first_run
