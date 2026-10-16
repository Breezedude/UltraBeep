cmake_minimum_required(VERSION 3.22)
project(ultrabeep C ASM)
# The stock bootloader installs only files named ultrabip*.update.
set(UPDATE_IMAGE ultrabip_ultrabeep.update)

set(CUBE    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/STM32CubeWB)
set(HAL     ${CUBE}/Drivers/STM32WBxx_HAL_Driver)
set(CMSIS_DEV ${CUBE}/Drivers/CMSIS/Device/ST/STM32WBxx)
set(FATFS   ${CUBE}/Middlewares/Third_Party/FatFs/src)
set(RTOS    ${CUBE}/Middlewares/Third_Party/FreeRTOS/Source)
set(USBLIB  ${CUBE}/Middlewares/ST/STM32_USB_Device_Library)
set(WPAN    ${CUBE}/Middlewares/ST/STM32_WPAN)
set(WPAN_IF ${WPAN}/interface/patterns/ble_thread)
set(TOOLS   ${CMAKE_CURRENT_SOURCE_DIR}/../tools)

if(NOT EXISTS ${HAL})
  message(FATAL_ERROR "STM32CubeWB not found in ${CUBE} - run tools/fetch_sdk.sh first")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(APP_SOURCES
  src/main.c
  src/app/backup.c
  src/app/battery.c
  src/app/ble.c
  src/app/biplink.c
  src/app/ble_console.c
  src/app/button.c
  src/app/config.c
  src/app/console.c
  src/app/console_pack.c
  src/app/dump.c
  src/app/crc32.c
  src/app/flight.c
  src/app/fusion.c
  src/app/gesture.c
  src/app/gps.c
  src/app/health.c
  src/app/identity.c
  src/app/igc.c
  src/app/remote.c
  src/app/indicator.c
  src/app/ini.c
  src/app/kml.c
  src/app/log.c
  src/app/nmea.c
  src/app/power.c
  src/app/rawlog.c
  src/app/recorder.c
  src/app/rtc_time.c
  src/app/sensors.c
  src/app/sha256.c
  src/app/sound.c
  src/app/storage.c
  src/app/summary.c
  src/app/tap.c
  src/app/announce.c
  src/app/thermal.c
  src/app/maneuver.c
  src/app/tilt.c
  src/app/system.c
  src/app/timeconv.c
  src/app/vario.c
  src/app/vario_audio.c
  src/drivers/analog.c
  src/drivers/audio.c
  src/drivers/board.c
  src/drivers/cksum.c
  src/drivers/clock.c
  src/drivers/diskio.c
  src/drivers/dps310.c
  src/drivers/faults.c
  src/drivers/handoff.c
  src/drivers/icm42605.c
  src/drivers/qoa.c
  src/drivers/sdcard.c
  src/drivers/sensor_bus.c
  src/drivers/uart_console.c
  src/drivers/syscalls.c
  src/drivers/usb_device.c
  src/drivers/usbd_conf.c
)

set(SDK_SOURCES
  ${CMSIS_DEV}/Source/Templates/gcc/startup_stm32wb55xx_cm4.s
  ${CMSIS_DEV}/Source/Templates/system_stm32wbxx.c
  ${HAL}/Src/stm32wbxx_hal.c
  ${HAL}/Src/stm32wbxx_hal_adc.c
  ${HAL}/Src/stm32wbxx_hal_adc_ex.c
  ${HAL}/Src/stm32wbxx_hal_cortex.c
  ${HAL}/Src/stm32wbxx_hal_dma.c
  ${HAL}/Src/stm32wbxx_hal_flash.c
  ${HAL}/Src/stm32wbxx_hal_flash_ex.c
  ${HAL}/Src/stm32wbxx_hal_gpio.c
  ${HAL}/Src/stm32wbxx_hal_iwdg.c
  ${HAL}/Src/stm32wbxx_hal_pcd.c
  ${HAL}/Src/stm32wbxx_hal_pcd_ex.c
  ${HAL}/Src/stm32wbxx_hal_pwr.c
  ${HAL}/Src/stm32wbxx_hal_pwr_ex.c
  ${HAL}/Src/stm32wbxx_hal_rcc.c
  ${HAL}/Src/stm32wbxx_hal_rcc_ex.c
  ${HAL}/Src/stm32wbxx_hal_tim.c
  ${HAL}/Src/stm32wbxx_hal_tim_ex.c
  ${HAL}/Src/stm32wbxx_hal_uart.c
  ${HAL}/Src/stm32wbxx_hal_uart_ex.c
  ${HAL}/Src/stm32wbxx_ll_usb.c
  ${USBLIB}/Core/Src/usbd_core.c
  ${USBLIB}/Core/Src/usbd_ctlreq.c
  ${USBLIB}/Core/Src/usbd_ioreq.c
  ${USBLIB}/Class/CDC/Src/usbd_cdc.c
  ${USBLIB}/Class/MSC/Src/usbd_msc.c
  ${USBLIB}/Class/MSC/Src/usbd_msc_bot.c
  ${USBLIB}/Class/MSC/Src/usbd_msc_data.c
  ${USBLIB}/Class/MSC/Src/usbd_msc_scsi.c
  ${USBLIB}/Class/CompositeBuilder/Src/usbd_composite_builder.c
  ${FATFS}/ff.c
  ${FATFS}/option/unicode.c
  ${RTOS}/tasks.c
  ${RTOS}/queue.c
  ${RTOS}/list.c
  ${RTOS}/timers.c
  ${RTOS}/event_groups.c
  ${RTOS}/stream_buffer.c
  ${RTOS}/portable/GCC/ARM_CM4F/port.c
  ${RTOS}/portable/MemMang/heap_4.c
  ${WPAN_IF}/tl/tl_mbox.c
  ${WPAN_IF}/tl/shci_tl.c
  ${WPAN_IF}/tl/shci_tl_if.c
  ${WPAN_IF}/tl/hci_tl.c
  ${WPAN_IF}/tl/hci_tl_if.c
  ${WPAN_IF}/shci/shci.c
  ${WPAN}/utilities/stm_list.c
  ${WPAN}/ble/core/template/osal.c
  ${WPAN}/ble/core/auto/ble_gap_aci.c
  ${WPAN}/ble/core/auto/ble_gatt_aci.c
  ${WPAN}/ble/core/auto/ble_hal_aci.c
  ${WPAN}/ble/core/auto/ble_hci_le.c
  ${WPAN}/ble/core/auto/ble_l2cap_aci.c
  ${CUBE}/Projects/P-NUCLEO-WB55.Nucleo/Applications/BLE/BLE_p2pServer/STM32_WPAN/Target/hw_ipcc.c
)

# IGC signing key (HMAC-SHA-256): keys/igc_signing.key, 64 hex digits, never in git
# (tools/make_signing_key.py creates it). Without it the firmware writes no G record.
set(IGC_KEY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/keys/igc_signing.key)
set(IGC_KEY_C ${CMAKE_CURRENT_BINARY_DIR}/generated/igc_key.c)
if(EXISTS ${IGC_KEY_FILE})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${IGC_KEY_FILE})
  file(READ ${IGC_KEY_FILE} IGC_KEY_HEX)
  string(STRIP "${IGC_KEY_HEX}" IGC_KEY_HEX)
  string(LENGTH "${IGC_KEY_HEX}" IGC_KEY_LEN)
  if(NOT IGC_KEY_LEN EQUAL 64 OR NOT IGC_KEY_HEX MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "${IGC_KEY_FILE}: expected 64 hex digits")
  endif()
  string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," IGC_KEY_BYTES "${IGC_KEY_HEX}")
  set(IGC_KEY_AVAILABLE true)
  message(STATUS "IGC signing key: ${IGC_KEY_FILE}")
else()
  set(IGC_KEY_BYTES "0")
  set(IGC_KEY_AVAILABLE false)
  message(STATUS "IGC signing key: none (keys/igc_signing.key missing) - no G records")
endif()
file(CONFIGURE OUTPUT ${IGC_KEY_C} CONTENT
"/* Generated by CMakeLists.txt from keys/igc_signing.key - do not commit. */
#include \"igc_key.h\"
const bool igc_key_available = ${IGC_KEY_AVAILABLE};
const uint8_t igc_key[IGC_KEY_SIZE] = {${IGC_KEY_BYTES}};
")

add_executable(${PROJECT_NAME} ${APP_SOURCES} ${IGC_KEY_C} ${SDK_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".elf")

target_include_directories(${PROJECT_NAME} PRIVATE
  inc
  config
  ${HAL}/Inc
  ${CMSIS_DEV}/Include
  ${CUBE}/Drivers/CMSIS/Core/Include
  ${CUBE}/Drivers/CMSIS/Include
  ${FATFS}
  ${RTOS}/include
  ${RTOS}/portable/GCC/ARM_CM4F
  ${USBLIB}/Core/Inc
  ${USBLIB}/Class/CDC/Inc
  ${USBLIB}/Class/MSC/Inc
  ${USBLIB}/Class/CompositeBuilder/Inc
  config/wpan
  ${WPAN}
  ${WPAN}/ble
  ${WPAN}/ble/core
  ${WPAN}/ble/core/template
  ${WPAN}/utilities
  ${WPAN_IF}
  ${WPAN_IF}/tl
  ${WPAN_IF}/shci
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  STM32WB55xx
  USE_HAL_DRIVER
  CORE_CM4
  USER_VECT_TAB_ADDRESS
  VECT_TAB_OFFSET=0x0000F000U
)

target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)

# Third-party code is compiled as it is; keep its warnings out of our output.
set_source_files_properties(${SDK_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

target_link_options(${PROJECT_NAME} PRIVATE
  -T${CMAKE_CURRENT_SOURCE_DIR}/linker/ultrabip_app.ld
  -Wl,-Map=${PROJECT_NAME}.map
  -u _printf_float
)
target_link_libraries(${PROJECT_NAME} PRIVATE m)

# Raw binary -> .update image with header, trailer and checksum.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
  COMMAND ${Python3_EXECUTABLE} ${TOOLS}/mkupdate.py build ${PROJECT_NAME}.bin ${UPDATE_IMAGE}
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
  COMMENT "Packaging ${UPDATE_IMAGE}"
)
