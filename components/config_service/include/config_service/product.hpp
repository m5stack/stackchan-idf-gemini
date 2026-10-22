// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

// Product-profile helpers for the Google / Gemini customer build.
// All gates default off so a build without CONFIG_STACKCHAN_GEMINI_ONLY
// keeps upstream behaviour. New NVS keys are additive; Provider numeric
// values are never renumbered.

#include "sdkconfig.h"

#include <config_service/config_service.hpp>

#include <string_view>

namespace stackchan::config {

inline constexpr bool gemini_only()
{
#ifdef CONFIG_STACKCHAN_GEMINI_ONLY
    return static_cast<bool>(CONFIG_STACKCHAN_GEMINI_ONLY);
#else
    return false;
#endif
}

inline Provider effective_provider(const DeviceConfig& cfg)
{
    return gemini_only() ? Provider::Gemini : cfg.provider;
}

inline const char* default_gemini_model()
{
#ifdef CONFIG_STACKCHAN_GEMINI_DEFAULT_MODEL
    return CONFIG_STACKCHAN_GEMINI_DEFAULT_MODEL;
#else
    return "gemini-3.8-live";
#endif
}

inline const char* default_gemini_voice()
{
#ifdef CONFIG_STACKCHAN_GEMINI_DEFAULT_VOICE
    return CONFIG_STACKCHAN_GEMINI_DEFAULT_VOICE;
#else
    return "Aoede";
#endif
}

// UI / persona language. NVS key `lang` is "en" / "ja"; empty follows the
// product default (English when GEMINI_ONLY, Japanese otherwise). There is
// no settings UI to change this — the key exists so upstream-compatible
// installs can still store Japanese.
void set_ui_lang(std::string_view nvs_lang);
bool ui_japanese();

inline const char* tr(const char* en, const char* ja)
{
    return ui_japanese() ? ja : en;
}

} // namespace stackchan::config
