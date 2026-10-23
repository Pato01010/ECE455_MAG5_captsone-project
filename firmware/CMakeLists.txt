# Magnetometer control-subsystem firmware: STM32G474RE on the NUCLEO-G474RE.
#
#   powershell -File build.ps1            (release; or: build.ps1 debug, build.ps1 tcxo)
#   powershell -File flash.ps1            (needs the Nucleo on USB)

cmake_minimum_required(VERSION 3.20)
project(magnetometer_fw C ASM)

set(MAG_HSE_HZ 24000000 CACHE STRING "HSE frequency in Hz: the Nucleo's crystal, or the TCXO")
option(MAG_HSE_BYPASS "TCXO on OSC_IN: HSE in bypass (Nucleo SB28 fitted, SB25/SB26 removed)" OFF)
option(MAG_N5_SHDN_PB7 "Rev C power board only: drive PB7 (N5_SHDN) high at boot" OFF)
set(MAG_VCP_BAUD 115200 CACHE STRING "Console baud rate on the ST-LINK virtual COM port")
option(MAG_GPS_TX "Drive USART1_TX (PC4, D1) to the GPS; off: the firmware only listens" OFF)

set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_C_FLAGS_DEBUG "-Og -g3")

add_executable(magnetometer
    app/app.c
    app/fit.c
    app/fmt.c
    app/gps.c
    app/noise.c
    app/patha.c
    app/shot.c
    app/xfer.c
    app/xfer_model.c
    bsp/board.c
    bsp/emu.c
    bsp/hw.c
    bsp/main.c
    bsp/startup_stm32g474xx.s
    bsp/syscalls.c
    vendor/device/Source/system_stm32g4xx.c
)
set_target_properties(magnetometer PROPERTIES SUFFIX ".elf")

target_include_directories(magnetometer PRIVATE
    app bsp vendor/cmsis/Include vendor/device/Include vendor/ll/Inc)

target_compile_definitions(magnetometer PRIVATE
    STM32G474xx
    HSE_VALUE=${MAG_HSE_HZ}u
    MAG_HSE_BYPASS=$<BOOL:${MAG_HSE_BYPASS}>
    MAG_N5_SHDN_PB7=$<BOOL:${MAG_N5_SHDN_PB7}>
    MAG_VCP_BAUD=${MAG_VCP_BAUD}u
    MAG_GPS_TX=$<BOOL:${MAG_GPS_TX}>
    MAG_TARGET=1
    $<$<CONFIG:Debug>:MAG_DEBUG_BUILD=1>)

target_compile_options(magnetometer PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Wshadow -Wdouble-promotion -fno-common>)

target_link_options(magnetometer PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/bsp/stm32g474re.ld
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/magnetometer.map
    -Wl,--print-memory-usage
    -Wl,--no-warn-rwx-segments)
target_link_libraries(magnetometer PRIVATE m)

add_custom_command(TARGET magnetometer POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:magnetometer> ${CMAKE_CURRENT_BINARY_DIR}/magnetometer.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:magnetometer> ${CMAKE_CURRENT_BINARY_DIR}/magnetometer.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:magnetometer>
    VERBATIM)
