# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Make this repository's own board root known before Zephyr resolves the board.
# zephyr/module.yml already does this, but only when the repo is registered as a
# Zephyr module (west workspace). This line keeps `west build -b distancer`
# working after a plain `git clone` into an existing workspace as well.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(distancer_ibeacon)

target_sources(app PRIVATE src/main.c src/leds.c src/battery.c)
