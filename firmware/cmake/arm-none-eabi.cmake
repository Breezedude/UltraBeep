# Toolchain file for the Arm GNU Toolchain (arm-none-eabi).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ARM_TOOLCHAIN_DIR)
  set(ARM_TOOLCHAIN_DIR "$ENV{ARM_TOOLCHAIN_DIR}")
endif()
if(ARM_TOOLCHAIN_DIR)
  set(_prefix "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-")
else()
  set(_prefix "arm-none-eabi-")
endif()
if(CMAKE_HOST_WIN32)
  set(_suffix ".exe")
endif()

set(CMAKE_C_COMPILER   "${_prefix}gcc${_suffix}")
set(CMAKE_ASM_COMPILER "${_prefix}gcc${_suffix}")
set(CMAKE_OBJCOPY      "${_prefix}objcopy${_suffix}")
set(CMAKE_SIZE         "${_prefix}size${_suffix}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS} -ffunction-sections -fdata-sections -fno-common -Wall -Wextra")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MCU_FLAGS} -specs=nano.specs -Wl,--gc-sections -Wl,--print-memory-usage")
