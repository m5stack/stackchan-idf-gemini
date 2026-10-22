cmake_minimum_required(VERSION 3.22)

# stackchan-gemini default board: a plain `idf.py build` (e.g. the ESP-IDF
# VS Code / Cursor extension) builds the Stack-chan CoreS3 configuration.
# `make build BOARD=...` and CI pass SDKCONFIG_DEFAULTS explicitly and are
# unaffected.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.cores3")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
    endif()
endif()
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET "esp32s3")
endif()
# The avatar DSL is compiled with Node at build time. IDE build tasks often
# don't load ~/.nvm, so fall back to an nvm-installed Node when PATH has none.
if(NOT DEFINED NODE_EXECUTABLE)
    find_program(_SC_NODE node)
    if(NOT _SC_NODE)
        file(GLOB _SC_NVM_NODES "$ENV{HOME}/.nvm/versions/node/*/bin/node")
        if(_SC_NVM_NODES)
            list(SORT _SC_NVM_NODES COMPARE NATURAL ORDER DESCENDING)
            list(GET _SC_NVM_NODES 0 _SC_NODE)
        endif()
    endif()
    if(_SC_NODE)
        set(NODE_EXECUTABLE "${_SC_NODE}" CACHE FILEPATH "Node.js used to compile the avatar DSL")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stackchan_idf)
