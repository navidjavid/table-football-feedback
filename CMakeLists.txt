cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(table-football C CXX ASM)
pico_sdk_init()

# ---------------------------------------------------------------------------
# Shared include paths
# ---------------------------------------------------------------------------
set(COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/lib/mfrc522
    ${CMAKE_CURRENT_LIST_DIR}/lib/ea_dogl128
)

# ---------------------------------------------------------------------------
# Hardware driver libraries
# ---------------------------------------------------------------------------
add_library(mfrc522_lib STATIC
    lib/mfrc522/mfrc522.c
)
target_include_directories(mfrc522_lib PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/lib/mfrc522
)
target_link_libraries(mfrc522_lib
    pico_stdlib
    hardware_spi
)

# ea_dogl128 driver — add ea_dogl128.c here when ready
# add_library(ea_dogl128_lib STATIC
#     lib/ea_dogl128/ea_dogl128.c
# )

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
add_executable(table-football
    src/main.c
    src/game_logic.c
    src/display_manager.c
    src/rfid_handler.c
    src/i2c_comms.c
)

target_include_directories(table-football PRIVATE ${COMMON_INCLUDES})

pico_set_program_name(table-football "table-football")
pico_set_program_version(table-football "0.1")

pico_enable_stdio_uart(table-football 0)
pico_enable_stdio_usb(table-football 1)

target_link_libraries(table-football
    pico_stdlib
    hardware_spi
    hardware_i2c
    mfrc522_lib
)

pico_add_extra_outputs(table-football)

# ---------------------------------------------------------------------------
# Test: RFID only  (swap main for test to run hardware checks)
# ---------------------------------------------------------------------------
add_executable(test_rfid
    tests/test_rfid.c
    src/rfid_handler.c
)

target_include_directories(test_rfid PRIVATE ${COMMON_INCLUDES})

pico_enable_stdio_uart(test_rfid 0)
pico_enable_stdio_usb(test_rfid 1)

target_link_libraries(test_rfid
    pico_stdlib
    hardware_spi
    mfrc522_lib
)

pico_add_extra_outputs(test_rfid)
