# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spp_client_demo)

set(PARTITION_TABLE_CSV "partitions.csv")
#fix wrong spi host defined in the hagl library
idf_build_set_property(COMPILE_DEFINITIONS "-DCONFIG_MIPI_DISPLAY_SPI_HOST=SPI2_HOST" APPEND)