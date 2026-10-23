# Toolchain for the STM32G474RE (Cortex-M4F) - the GNU tools that ship with STM32CubeCLT.
#
# Override the location with -DCUBECLT_ROOT=... or the CUBECLT_ROOT environment variable.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(DEFINED ENV{CUBECLT_ROOT} AND NOT CUBECLT_ROOT)
    set(CUBECLT_ROOT "$ENV{CUBECLT_ROOT}")
endif()
if(NOT CUBECLT_ROOT)
    set(CUBECLT_ROOT "C:/ST/STM32CubeCLT_1.21.0")
endif()
set(CUBECLT_ROOT "${CUBECLT_ROOT}" CACHE PATH "STM32CubeCLT install folder")

set(_bin "${CUBECLT_ROOT}/GNU-tools-for-STM32/bin")
if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
else()
    set(_exe "")
endif()

set(CMAKE_C_COMPILER   "${_bin}/arm-none-eabi-gcc${_exe}")
set(CMAKE_ASM_COMPILER "${_bin}/arm-none-eabi-gcc${_exe}")
set(CMAKE_OBJCOPY      "${_bin}/arm-none-eabi-objcopy${_exe}" CACHE FILEPATH "")
set(CMAKE_SIZE         "${_bin}/arm-none-eabi-size${_exe}" CACHE FILEPATH "")

# The compiler cannot link a test program without our linker script.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
