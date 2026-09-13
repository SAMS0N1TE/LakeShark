cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${MIXRF_VENDOR_DRIVER}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mixrf_probe)
