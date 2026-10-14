#!/bin/sh
# Fetches the parts of STM32CubeWB that the firmware needs into third_party/.
# The SDK is not part of this repository (size, separate license).
set -e

COMMIT=788f3c33191cb1b88992dec98cdc25bcf0ae68c3 # STM32CubeWB master, 2026-08-26 (FW.WB.1.24.0)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/third_party/STM32CubeWB"

if [ -d "$DIR/.git" ]; then
  echo "SDK already present in $DIR"
  exit 0
fi

git clone --filter=blob:none --no-checkout https://github.com/STMicroelectronics/STM32CubeWB.git "$DIR"
cd "$DIR"
git sparse-checkout set \
  Drivers/CMSIS/Core \
  Drivers/CMSIS/Include \
  Drivers/CMSIS/Device/ST/STM32WBxx \
  Drivers/STM32WBxx_HAL_Driver \
  Middlewares/ST/STM32_USB_Device_Library \
  Middlewares/ST/STM32_WPAN \
  Middlewares/Third_Party/FatFs \
  Middlewares/Third_Party/FreeRTOS \
  Projects/P-NUCLEO-WB55.Nucleo/Applications/BLE/BLE_p2pServer
git checkout "$COMMIT"
