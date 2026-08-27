cmake_minimum_required(VERSION 3.16)
set(CMAKE_C_STANDARD 11)

# Bluepad32 repo layout: ESP-IDF component lives under src/components/bluepad32.
# Point at that component dir so cmd_nvs/cmd_system from the submodule are not
# registered (this project already has its own copies under components/).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/bluepad32/src/components/bluepad32")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(panzer2_idf)
