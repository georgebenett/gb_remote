# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# main/version.h is the one version source: it names the app descriptor
# (PROJECT_VER) and gives C the numeric parts.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _fw_ver REGEX "#define FW_VERSION ")
string(REGEX REPLACE ".*\"(.*)\".*" "\\1" PROJECT_VER "${_fw_ver}")
string(REPLACE "." ";" _fw_parts "${PROJECT_VER}")
list(GET _fw_parts 0 _major)
list(GET _fw_parts 1 _minor)
list(GET _fw_parts 2 _patch)
idf_build_set_property(COMPILE_DEFINITIONS "-DFW_VERSION_MAJOR=${_major}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DFW_VERSION_MINOR=${_minor}" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DFW_VERSION_PATCH=${_patch}" APPEND)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/main/version.h")

# LVGL's object pool (CONFIG_LV_MEM_SIZE_KILOBYTES) is malloc'd at lv_init()
# instead of being a static array in internal RAM, which Bluetooth needs.
# Blocks over CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL go to PSRAM.
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_MEM_POOL_INCLUDE=<stdlib.h>" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_MEM_POOL_ALLOC=malloc" APPEND)

project(gb_remote)

# Receiver image the remote pushes over BLE (main/rx_ota.c). gb_receiver/bundle_to_remotes.sh
# puts gb_receiver_v<ver>.bin here; pack_rx_fw.py compresses it and idf.py flash writes
# the result into rx_fw. Not being an app image, it also passes esptool's chip check.
file(GLOB RX_FW_BIN CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/receiver_fw/gb_receiver_v*.bin")
list(LENGTH RX_FW_BIN _rx_fw_count)
if(_rx_fw_count GREATER 1)
    message(FATAL_ERROR "More than one receiver image in receiver_fw/: ${RX_FW_BIN}")
elseif(RX_FW_BIN)
    idf_build_get_property(python PYTHON)
    set(RX_FW_OUT "${CMAKE_BINARY_DIR}/rx_fw.bin")
    add_custom_command(OUTPUT "${RX_FW_OUT}"
        COMMAND ${python} "${CMAKE_SOURCE_DIR}/pack_rx_fw.py" "${RX_FW_BIN}" "${RX_FW_OUT}"
        DEPENDS "${RX_FW_BIN}" "${CMAKE_SOURCE_DIR}/pack_rx_fw.py" VERBATIM)
    add_custom_target(rx_fw_bin ALL DEPENDS "${RX_FW_OUT}")
    esptool_py_flash_to_partition(flash "rx_fw" "${RX_FW_OUT}")
    add_dependencies(flash rx_fw_bin)
else()
    message(WARNING "No receiver_fw/gb_receiver_v*.bin: this remote will not update receivers")
endif()
