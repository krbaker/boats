cmake_minimum_required(VERSION 3.16)

# The component manager's "new versions available" check reads newer releases'
# manifests too, and esp_hosted 3.1.0's has rules on Kconfig options nothing
# defines (ESP_HOSTED_HOST_USB_*_MANIFEST), which stops the configure with
# "Missing required kconfig option after retry". dependencies.lock pins the
# versions anyway, so skip the check.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} 0)

# Which board this build is for (BOARDS_PLAN.md). build.sh sets it and keeps a
# build directory per board; plain `idf.py build` builds the 3.4C.
set(AIS_BOARDS lcd34c lcd4c lcd10 headless)
set(AIS_BOARD lcd34c CACHE STRING "Board: one of ${AIS_BOARDS}")
if(NOT AIS_BOARD IN_LIST AIS_BOARDS)
    message(FATAL_ERROR "AIS_BOARD=${AIS_BOARD}: use one of ${AIS_BOARDS}")
endif()
# The shared defaults, then the board's own.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${AIS_BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project name goes into the image's app description, and a web update is
# refused unless it matches the running firmware's: so each board's name is its
# own, and a display can't be sent another board's firmware. The 3.4C keeps the
# original name, so displays already in use still take its updates.
if(AIS_BOARD STREQUAL "lcd34c")
    project(ais_display)
else()
    project(ais_display_${AIS_BOARD})
endif()

# An sdkconfig keeps its board once made: catch one made for another board
# (e.g. AIS_BOARD changed on an existing build directory).
idf_build_get_property(ais_sdkconfig_cmake SDKCONFIG_CMAKE)
include(${ais_sdkconfig_cmake})
if(NOT "${CONFIG_AIS_BOARD_NAME}" STREQUAL "${AIS_BOARD}")
    message(FATAL_ERROR "This build directory's sdkconfig is for the ${CONFIG_AIS_BOARD_NAME} board, not "
                        "${AIS_BOARD}. Use ./build.sh ${AIS_BOARD}, which keeps a build directory per board.")
endif()
