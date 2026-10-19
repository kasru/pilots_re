# Standalone project on purpose: this is a diagnostic, not part of the game, and
# building it must not require configuring PILOT_RV (or vice versa). The
# toolchain file does the real work - see the note at the top of the repository's
# CMakeLists.txt about why CMake needs both -DPLATFORM_SWITCH and the toolchain.
cmake_minimum_required(VERSION 3.16)
project(pilots_memmap_probe C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)

add_executable(PILOT_MAP memmap_probe.c)

if(PLATFORM_SWITCH)
  nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/PILOT_MAP.nacp"
                   NAME    "Pilots memmap probe"
                   AUTHOR  "pilots_re reconstruction"
                   VERSION "0.1")
  nx_create_nro(PILOT_MAP NACP "${CMAKE_CURRENT_BINARY_DIR}/PILOT_MAP.nacp")
  message(STATUS "PILOT_MAP: nro target PILOT_MAP_nro -> ${CMAKE_CURRENT_BINARY_DIR}/PILOT_MAP.nro")
endif()
