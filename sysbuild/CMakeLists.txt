# SPDX-License-Identifier: Apache-2.0
#
# Sysbuild entry point, a copy of Zephyr's default template
# (zephyr/share/sysbuild/template/CMakeLists.txt) with one addition.
#
# Sysbuild resolves the board in its own CMake scope, before it descends into
# the application - so the BOARD_ROOT in the application's CMakeLists.txt comes
# too late for `west build --sysbuild`. Setting it here covers that scope; from
# there Zephyr merges it into every image (see zephyr/cmake/modules/root.cmake).

cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${APP_DIR})

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)
