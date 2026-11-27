cmake_minimum_required(VERSION 3.16)

# The component manager's "new versions available" check reads newer releases'
# manifests too, and esp_hosted 3.1.0's has rules on Kconfig options nothing
# defines (ESP_HOSTED_HOST_USB_*_MANIFEST), which stops the configure with
# "Missing required kconfig option after retry". dependencies.lock pins the
# versions anyway, so skip the check.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} 0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ais_display)
