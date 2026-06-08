cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(table-football C CXX ASM)
pico_sdk_init()

set(COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/lib/mfrc522
    ${CMAKE_CURRENT_LIST_DIR}/lib/ea_dogl128
)

# --- mfrc522 driver library ---
add_library(mfrc522_lib STATIC lib/mfrc522/mfrc522.c)
target_include_directories(mfrc522_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lib/mfrc522)
target_link_libraries(mfrc522_lib pico_stdlib hardware_spi)

# --- display driver library ---
add_library(ea_dogl128_lib STATIC lib/ea_dogl128/ea_dogl128.c)
target_include_directories(ea_dogl128_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lib/ea_dogl128)
target_link_libraries(ea_dogl128_lib pico_stdlib)

# --- main application ---
add_executable(table-football
    src/main.c
    src/rfid_handler.c
    src/i2c_comms.c
    src/game_logic.c
    src/display_manager.c
)
target_include_directories(table-football PRIVATE ${COMMON_INCLUDES})
pico_set_program_name(table-football "table-football")
pico_enable_stdio_uart(table-football 0)
pico_enable_stdio_usb(table-football 1)
target_link_libraries(table-football
    pico_stdlib
    hardware_spi
    hardware_i2c
    pico_i2c_slave
    mfrc522_lib
    ea_dogl128_lib
)
pico_add_extra_outputs(table-football)

# --- I2C simulator (second Pico) ---
add_executable(test_i2c_simulator tests/test_i2c_simulator.c)
target_include_directories(test_i2c_simulator PRIVATE ${COMMON_INCLUDES})
pico_enable_stdio_uart(test_i2c_simulator 0)
pico_enable_stdio_usb(test_i2c_simulator 1)
target_link_libraries(test_i2c_simulator
    pico_stdlib
    hardware_i2c
    m
)
pico_add_extra_outputs(test_i2c_simulator)
