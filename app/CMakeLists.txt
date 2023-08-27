# Copyright (c) 2023 Jamie M.
# All right reserved. This code is not apache or FOSS/copyleft licensed.

cmake_minimum_required(VERSION 3.24.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(central_esp)

target_sources(app PRIVATE src/main.c)
