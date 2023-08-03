# Copyright (c) 2023 Jamie M.
# All right reserved. This code is not apache or FOSS/copyleft licensed.

cmake_minimum_required(VERSION 3.24.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(central_esp)

target_sources(app PRIVATE src/main.c)

if(CONFIG_BOARD_BL654_DVK)
  # For BL654, include fake magic files to allow returning to bootloader and for bootloader to
  # think firmware is genuine, allowing it to be booted
  target_sources(app PRIVATE src/magic.c)
  zephyr_linker_sources(ROM_START bl654.ld)
endif()
