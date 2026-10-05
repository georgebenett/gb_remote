# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL's object pool (CONFIG_LV_MEM_SIZE_KILOBYTES) is malloc'd at lv_init()
# instead of being a static array in internal RAM, which Bluetooth needs.
# Blocks over CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL go to PSRAM.
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_MEM_POOL_INCLUDE=<stdlib.h>" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_MEM_POOL_ALLOC=malloc" APPEND)

project(gb_controller_lite)

set(PARTITION_TABLE_CSV "partitions.csv")
#fix wrong spi host defined in the hagl library
idf_build_set_property(COMPILE_DEFINITIONS "-DCONFIG_MIPI_DISPLAY_SPI_HOST=SPI2_HOST" APPEND)