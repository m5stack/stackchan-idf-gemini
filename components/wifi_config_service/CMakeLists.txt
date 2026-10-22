# wifi_config_service — Wi-Fi-side configuration endpoint.
#
# Mirrors the BLE GATT settings service over HTTP once Wi-Fi STA is up.
# Same X25519 + AES-256-GCM application-layer crypto session as
# config_service/crypto.cpp, just framed in HTTP request/response bodies
# instead of GATT attribute reads/writes. mDNS publishes the hostname and
# `_stackchan-config._tcp.local` service for browser discovery.

idf_component_register(
    SRCS
        "wifi_config_service.cpp"
        "http_handlers.cpp"
        "mcp_events.cpp"
        "release_ota.cpp"
        "https_fetch.cpp"
        "voice_fetch.cpp"
        "sano_fetch.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES tl_expected config_service avatar_vm esp_http_server crash_report
    PRIV_REQUIRES
        flash_layout
        esp_https_ota
        esp_app_format
        app_update
        esp_event
        esp_http_client
        esp-tls
        esp_partition
        esp_system
        esp_timer
        json
        mbedtls
        espressif__mdns
    # NB: settings_wifi.html is NOT listed in EMBED_TXTFILES; the in-page
    # Avatar DSL editor needs the JS compiler bundle inlined into it, which
    # is done by `tools/avatar_dsl/inject.mjs` at build time. The processed
    # file is then attached via target_add_binary_data below (same linker
    # symbol name — `_binary_settings_wifi_html_*` — because the basename
    # is unchanged).
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# --- Build-time injection: bundle the DSL compiler + inline the default DSL
# source into a copy of settings_wifi.html under the build dir. Gemini-only
# builds then translate the complete injected artifact (including the inlined
# settings_common.js / gemini_ui.js) before embedding it. Translating the final
# self-contained page is important: a browser-side overlay misses static text
# and strings produced after the overlay runs. IDF disallows add_custom_command
# during the requirements-collection phase, so this MUST sit after the
# idf_component_register() call.
find_program(NODE_EXECUTABLE NAMES node REQUIRED
    DOC "Node.js (>= 18) used to bundle the avatar DSL compiler into settings_wifi.html")
find_program(PYTHON3_EXECUTABLE NAMES python3 REQUIRED
    DOC "Python 3 used for the deterministic English settings-page build")

set(_SETTINGS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/settings_wifi.html")
set(_SETTINGS_INJECTED "${CMAKE_CURRENT_BINARY_DIR}/settings_wifi.injected.html")
set(_SETTINGS_OUT "${CMAKE_CURRENT_BINARY_DIR}/settings_wifi.html")
set(_DSL_INJECT "${PROJECT_DIR}/tools/avatar_dsl/inject.mjs")
set(_SETTINGS_I18N_DIR "${PROJECT_DIR}/tools/settings_i18n")
set(_SETTINGS_I18N "${_SETTINGS_I18N_DIR}/translate.py")
set(_SETTINGS_I18N_CATALOG "${_SETTINGS_I18N_DIR}/catalog.json")
set(_SETTINGS_I18N_OVERRIDES "${_SETTINGS_I18N_DIR}/overrides.json")
set(_SETTINGS_I18N_POLISH "${_SETTINGS_I18N_DIR}/polish.json")
# DSL presets shipped in the page selector. Order matters — the first preset
# is the textarea's initial value. Keep aligned with wasm/build.sh so the
# Wi-Fi page and the standalone preview offer the same lineup.
set(_DSL_PRESETS
    "default=${PROJECT_DIR}/assets/default_face.avdsl"
    "omega=${PROJECT_DIR}/assets/omega_mouth.avdsl"
    "aokko=${PROJECT_DIR}/assets/aokko_face.avdsl"
)
set(_DSL_PRESET_FILES
    "${PROJECT_DIR}/assets/default_face.avdsl"
    "${PROJECT_DIR}/assets/omega_mouth.avdsl"
    "${PROJECT_DIR}/assets/aokko_face.avdsl"
)
# The inject script depends on every JS source file it bundles, so rebuilds
# pick up edits to the compiler. settings_common.js is inlined into the page
# via the SETTINGS_COMMON placeholder, so it is a rebuild trigger too.
file(GLOB _DSL_JS "${PROJECT_DIR}/tools/avatar_dsl/*.js" "${PROJECT_DIR}/tools/avatar_dsl/*.mjs")
set(_SETTINGS_COMMON_JS "${PROJECT_DIR}/tools/settings_common.js")
set(_GEMINI_UI_JS "${PROJECT_DIR}/tools/gemini_ui.js")

add_custom_command(
    OUTPUT "${_SETTINGS_INJECTED}"
    COMMAND ${NODE_EXECUTABLE} "${_DSL_INJECT}" "${_SETTINGS_SRC}" "${_SETTINGS_INJECTED}" ${_DSL_PRESETS}
    DEPENDS "${_SETTINGS_SRC}" ${_DSL_PRESET_FILES} ${_DSL_JS} "${_SETTINGS_COMMON_JS}" "${_GEMINI_UI_JS}"
    COMMENT "[wifi_config_service] injecting avatar DSL compiler + presets into settings_wifi.html"
    VERBATIM
)

if(CONFIG_STACKCHAN_SETTINGS_ENGLISH)
    add_custom_command(
        OUTPUT "${_SETTINGS_OUT}"
        COMMAND ${PYTHON3_EXECUTABLE} "${_SETTINGS_I18N}" "${_SETTINGS_I18N_CATALOG}"
                "${_SETTINGS_INJECTED}" "${_SETTINGS_OUT}"
        DEPENDS "${_SETTINGS_INJECTED}" "${_SETTINGS_I18N}" "${_SETTINGS_I18N_CATALOG}"
                "${_SETTINGS_I18N_OVERRIDES}" "${_SETTINGS_I18N_POLISH}"
        COMMENT "[wifi_config_service] translating complete settings page to English"
        VERBATIM
    )
else()
    add_custom_command(
        OUTPUT "${_SETTINGS_OUT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_SETTINGS_INJECTED}" "${_SETTINGS_OUT}"
        DEPENDS "${_SETTINGS_INJECTED}"
        COMMENT "[wifi_config_service] keeping upstream Japanese settings page"
        VERBATIM
    )
endif()

add_custom_target(wifi_settings_html_processed DEPENDS "${_SETTINGS_OUT}")
add_dependencies(${COMPONENT_LIB} wifi_settings_html_processed)
target_add_binary_data(${COMPONENT_LIB} "${_SETTINGS_OUT}" TEXT)
