// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "sdkconfig.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace stackchan::wifi_config {

// Base URL of the GitHub Pages catalog (firmware/ + voices/). Empty means
// in-app release-fetch / voice-fetch is disabled.
inline const char* update_base_url()
{
#ifdef CONFIG_STACKCHAN_UPDATE_BASE_URL
    return CONFIG_STACKCHAN_UPDATE_BASE_URL;
#else
    return "";
#endif
}

inline bool update_catalog_configured()
{
    return update_base_url()[0] != '\0';
}

// Join base + path ("firmware/..." or "/voices.json"). Strips a trailing
// slash on the base. Returns the snprintf result (bytes that would be written).
inline int format_update_url(char* buf, std::size_t cap, const char* path)
{
    const char* base = update_base_url();
    if (buf == nullptr || cap == 0) return 0;
    if (base[0] == '\0' || path == nullptr) {
        buf[0] = '\0';
        return 0;
    }
    std::size_t n = std::strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        --n;
    }
    if (*path == '/') {
        ++path;
    }
    return std::snprintf(buf, cap, "%.*s/%s", static_cast<int>(n), base, path);
}

} // namespace stackchan::wifi_config
